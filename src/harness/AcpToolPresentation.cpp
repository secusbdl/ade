#include "AcpToolPresentation.h"
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <cmath>
namespace ade {
namespace {
QString primary(const QList<AcpDisplayMount> &mounts) {
  static const QSet<QString> special{"plugins","apps","att","plans","logs","memory"};
  for (const auto &mount : mounts)
    if (!special.contains(mount.prefix)) return mount.prefix;
  return {};
}
bool absolute(const QString &path) {
  return QDir::isAbsolutePath(path) ||
         QRegularExpression("^[A-Za-z]:[/\\\\]").match(path).hasMatch() ||
         path.startsWith("\\\\");
}
QString clean(QString path) {
  path.replace('\\','/');
  return QDir::cleanPath(path);
}
QString inputPath(const QString &path,const QList<AcpDisplayMount> &mounts) {
  if (absolute(path)) return AcpToolPresentation::mountedPath(path,mounts);
  const auto prefix=primary(mounts);
  if(path.isEmpty())return prefix;
  return prefix.isEmpty()?path:prefix+'/'+(path.startsWith("./")?path.mid(2):path);
}
QString quoted(const QString &text) {
  return QRegularExpression("['\"]([^'\"]+)['\"]").match(text).captured(1);
}
struct Budget {
  int left=AcpToolPresentation::MaximumTotalText;
  bool truncated=false;
  QString take(const QString &text,int limit=AcpToolPresentation::MaximumText) {
    int size=qMin(text.size(),qMin(left,limit));
    if(size>0&&size<text.size()&&text[size-1].isHighSurrogate())--size;
    truncated|=size<text.size();left-=size;return text.left(size);
  }
};
QString output(const QJsonObject &tool,Budget &budget,bool *complete) {
  QString result;bool found=false,clipped=false;
  auto append=[&](const QString &text){
    const int remain=AcpToolPresentation::MaximumText-result.size();
    if(found&&remain>0)result+='\n';else if(found)clipped=true;
    const int room=AcpToolPresentation::MaximumText-result.size();
    int accepted=qMin(room,text.size());
    if(accepted>0&&accepted<text.size()&&text[accepted-1].isHighSurrogate())--accepted;
    result+=text.left(accepted);clipped|=text.size()>accepted;found=true;
  };
  for(const auto &value:tool["content"].toArray()) {
    const auto item=value.toObject(),content=item["content"].toObject();
    if(item["type"]=="content"&&content["type"]=="text")append(content["text"].toString());
  }
  if(!found) {
    const auto terminal=tool["_meta"].toObject()["terminal_output"].toObject();
    const auto raw=tool["rawOutput"].toObject();
    if(terminal["data"].isString())append(terminal["data"].toString());
    else if(raw["formatted_output"].isString())append(raw["formatted_output"].toString());
    else if(raw["output"].isString())append(raw["output"].toString());
    else append(tool["rawOutput"].toString());
  }
  const auto accepted=budget.take(result);*complete=!clipped&&accepted.size()==result.size();
  budget.truncated|=clipped;return accepted;
}
QStringList lines(const QString &text) {
  QStringList result;int start=0;
  while(start<text.size()) {
    const int newline=text.indexOf('\n',start);
    const int end=newline<0?text.size():newline+1;
    result.append(text.mid(start,end-start));start=end;
  }
  return result;
}
void lineCounts(QJsonObject &entry) {
  auto before=lines(entry["before"].toString()),after=lines(entry["after"].toString());
  int prefix=0;while(prefix<before.size()&&prefix<after.size()&&before[prefix]==after[prefix])++prefix;
  int suffix=0;while(suffix<before.size()-prefix&&suffix<after.size()-prefix&&before[before.size()-1-suffix]==after[after.size()-1-suffix])++suffix;
  const int n=before.size()-prefix-suffix,m=after.size()-prefix-suffix;
  if(qint64(n+1)*(m+1)>1000000)return;
  QVector<int> previous(m+1),current(m+1);
  for(int i=0;i<n;++i) {
    for(int j=0;j<m;++j)current[j+1]=before[prefix+i]==after[prefix+j]?previous[j]+1:qMax(previous[j+1],current[j]);
    previous.swap(current);current.fill(0);
  }
  int added=m-previous[m],removed=n-previous[m];
  entry["added"]=added;entry["removed"]=removed;
}
bool onlyKeys(const QJsonObject &object,const QSet<QString> &allowed) {
  for(auto i=object.begin();i!=object.end();++i)if(!allowed.contains(i.key()))return false;
  return true;
}
}
QString AcpToolPresentation::sourceName(const QJsonObject &tool) {
  if(!tool["name"].toString().isEmpty())return tool["name"].toString();
  const auto meta=tool["_meta"].toObject();
  for(auto i=meta.begin();i!=meta.end();++i) {
    const auto name=i.value().toObject()["toolName"].toString();
    if(!name.isEmpty())return name;
  }
  return {};
}
bool AcpToolPresentation::isQuestionWrapper(const QJsonObject &tool) {
  const auto name=sourceName(tool);
  return name=="request_user_input" || name.contains("stagewise_request_user_input") ||
      tool.value("title").toString().contains("stagewise_request_user_input") ||
      tool.value("rawInput").toObject().value("tool")=="stagewise_request_user_input";
}
bool AcpToolPresentation::suppressQuestionWrapperPart(const QJsonObject &part) {
  const auto mapped=part.value("acpPresentation").toObject();
  if(part.value("type")!="tool" || mapped.value("questionWrapper")!=true ||
      part.value("state")=="approval-requested" || part.value("approvalSeen")==true)return false;
  return valid(mapped);
}
QString AcpToolPresentation::mountedPath(const QString &path,const QList<AcpDisplayMount> &mounts) {
  if(!absolute(path)) {
    for(const auto &mount:mounts)if(path.section('/',0,0)==mount.prefix)return path;
    return inputPath(path,mounts);
  }
  const auto normalized=clean(path);QString best,root;
  for(const auto &mount:mounts) {
    const auto candidate=clean(mount.root);
    // Match Node's lexical boundary semantics. No case folding/file lookups.
    const auto boundary=candidate.endsWith('/')?candidate:candidate+'/';
    if((normalized==candidate||normalized.startsWith(boundary))&&candidate.size()>root.size()) {
      root=candidate;best=mount.prefix;
    }
  }
  if(root.isEmpty())return path;
  auto relative=normalized.mid(root.size());if(relative.startsWith('/'))relative.remove(0,1);
  return relative.isEmpty()?best:best+'/'+relative;
}
QJsonObject AcpToolPresentation::map(const QJsonObject &tool,const QList<AcpDisplayMount> &mounts) {
  Budget budget;bool outputComplete=true;
  const auto text=output(tool,budget,&outputComplete);
  const bool failed=tool["status"]=="failed",complete=failed||tool["status"]=="completed";
  QJsonArray entries;int omitted=0,index=0;
  const auto title=tool["title"].toString();
  auto base=[&](const QString &kind,int number){
    QJsonObject value{{"id",QString::number(number)},{"kind",kind}};
    if(failed)value["errorText"]=budget.take(text.isEmpty()?tool["title"].toString("Tool")+" failed":text);
    return value;
  };
  for(const auto &value:tool["content"].toArray()) {
    const auto diff=value.toObject();if(diff["type"]!="diff")continue;
    if(entries.size()>=MaximumEntries){++omitted;continue;}
    const bool deleted=tool["kind"]=="delete"||diff["_meta"].toObject()["stagewiseOperation"]=="delete";
    const bool added=diff["oldText"].isNull()||diff["oldText"].isUndefined();
    auto entry=base(deleted?"delete":added?"write":"edit",index++);
    entry["path"]=budget.take(mountedPath(diff["path"].toString(),mounts),4096);
    QJsonValue before=added?QJsonValue(QJsonValue::Null):diff["oldText"];
    if(deleted&&added)before=QString();
    const QJsonValue after=deleted?QJsonValue(QJsonValue::Null):diff["newText"];
    const auto size=before.toString().size()+after.toString().size();
    const bool available=diff["path"].isString()&&!diff["path"].toString().isEmpty()&&
        diff["path"].toString().size()<=4096&&(before.isNull()||before.isString())&&
        (after.isNull()||after.isString())&&before.toString().size()<=MaximumText&&
        after.toString().size()<=MaximumText&&size<=budget.left;
    entry["diffAvailable"]=available;
    if(available){entry["before"]=before;entry["after"]=after;budget.left-=size;lineCounts(entry);}
    else budget.truncated=true;
    entries.append(entry);
  }
  if(entries.isEmpty()) {
    const auto input=tool["rawInput"].toObject();auto kind=tool["kind"].toString();
    const auto locations=tool["locations"].toArray();
    QJsonObject entry;
    if(kind=="execute") {
      entry=base("execute",0);QString command;
      if(input["command"].isString()) {
        command=budget.take(input["command"].toString());
        for(const auto &arg:input["args"].toArray())if(arg.isString())command+=budget.take(' '+arg.toString(),MaximumText-command.size());
      }else command=budget.take(tool["title"].toString("Command"));
      entry["command"]=command;entry["explanation"]=budget.take(title,4096);
      const auto terminal=tool["_meta"].toObject()["terminal_exit"].toObject();
      const auto exit=terminal["exit_code"].isDouble()?terminal["exit_code"]:tool["rawOutput"].toObject()["exit_code"];
      if(exit.isDouble())entry["exitCode"]=exit;else if(complete)entry["exitCode"]=failed?1:0;
      entry["completed"]=complete;entry["output"]=text;
    }else if(kind=="read"&&!locations.isEmpty()&&!locations[0].toObject()["path"].toString().isEmpty()) {
      entry=base("read",0);entry["path"]=budget.take(mountedPath(locations[0].toObject()["path"].toString(),mounts),4096);
      if(complete)entry["output"]=text;
    }else if(kind=="read"&&title.startsWith("List ")) {
      entry=base("list",0);entry["path"]=budget.take(inputPath(quoted(title),mounts),4096);
    }else if(kind=="search") {
      entry=base("search",0);auto query=quoted(title);if(query.isEmpty())query=tool["title"].toString("search");
      entry["query"]=budget.take(query,4096);entry["mount"]=budget.take(primary(mounts),4096);
      if(complete){entry["output"]=text;if(outputComplete)entry["matches"]=text.split('\n',Qt::SkipEmptyParts).size();}
    }else {
      entry=base("generic",0);const auto name=sourceName(tool);
      entry["name"]=budget.take("acp."+(name.isEmpty()?tool["kind"].toString("tool"):name),4096);
      entry["title"]=budget.take(title,4096);if(complete)entry["output"]=text;
    }
    entries.append(entry);
  }
  QJsonObject result{{"version",1},{"entries",entries},{"omitted",omitted},{"truncated",budget.truncated||omitted>0}};
  if(isQuestionWrapper(tool))result["questionWrapper"]=true;
  return result;
}
bool AcpToolPresentation::valid(const QJsonObject &value) {
  if(!onlyKeys(value,{"version","entries","omitted","truncated","questionWrapper"})||value["version"]!=1||!value["entries"].isArray()||!value["truncated"].isBool()||!value["omitted"].isDouble())return false;
  if(value.contains("questionWrapper")&&!value.value("questionWrapper").isBool())return false;
  const auto entries=value["entries"].toArray();const double omitted=value["omitted"].toDouble();
  if(entries.isEmpty()||entries.size()>MaximumEntries||omitted<0||omitted>1000000||omitted!=std::floor(omitted))return false;
  int total=0;
  for(int i=0;i<entries.size();++i) {
    const auto entry=entries[i].toObject();const auto kind=entry["kind"].toString();
    if(!entries[i].isObject()||entry["id"]!=QString::number(i)||!QStringList{"generic","read","list","search","execute","write","edit","delete"}.contains(kind)||!onlyKeys(entry,{"id","kind","path","query","mount","command","explanation","output","errorText","exitCode","completed","matches","name","title","diffAvailable","before","after","added","removed"}))return false;
    for(auto j=entry.begin();j!=entry.end();++j) {
      const auto key=j.key();if(key=="id"||key=="kind")continue;
      if(key=="completed"||key=="diffAvailable"){if(!j.value().isBool())return false;continue;}
      if(key=="exitCode"||key=="matches"||key=="added"||key=="removed") {if(!j.value().isDouble()||!std::isfinite(j.value().toDouble()))return false;if(key!="exitCode"&&(j.value().toDouble()<0||j.value().toDouble()>MaximumText||j.value().toDouble()!=std::floor(j.value().toDouble())))return false;continue;}
      if((key=="before"||key=="after")&&j.value().isNull())continue;
      if(!j.value().isString())return false;
      const auto size=j.value().toString().size();const int cap=QStringList{"path","query","mount","explanation","name","title"}.contains(key)?4096:MaximumText;
      if(size>cap||(total+=size)>MaximumTotalText)return false;
    }
    if((kind=="read"||kind=="list")&&!entry["path"].isString())return false;
    if(kind=="search"&&(!entry["query"].isString()||!entry["mount"].isString()))return false;
    if(kind=="execute"&&(!entry["command"].isString()||!entry["completed"].isBool()))return false;
    if(kind=="generic"&&!entry["name"].isString())return false;
    if(kind=="write"||kind=="edit"||kind=="delete") {
      if(!entry["path"].isString()||!entry["diffAvailable"].isBool()||entry.contains("added")!=entry.contains("removed"))return false;
      if(entry["diffAvailable"].toBool()) {
        if(!entry.contains("before")||!entry.contains("after"))return false;
        if(kind=="write"&&(!entry["before"].isNull()||!entry["after"].isString()))return false;
        if(kind=="delete"&&(!entry["before"].isString()||!entry["after"].isNull()))return false;
        if(kind=="edit"&&(!entry["before"].isString()||!entry["after"].isString()))return false;
      }else if(entry.contains("before")||entry.contains("after")||entry.contains("added")||entry.contains("removed"))return false;
    }else if(entry.contains("before")||entry.contains("after")||entry.contains("diffAvailable")||entry.contains("added")||entry.contains("removed"))return false;
  }
  return true;
}
}
