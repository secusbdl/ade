#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <functional>
inline void recordAttachmentEvidence(const QString &protocol,const QJsonObject &frame){
  const auto log=qEnvironmentVariable("ADE_ATTACHMENT_LOG");
  const auto allowed=QFileInfo(qEnvironmentVariable("ADE_ATTACHMENT_ROOT")).canonicalFilePath();
  if(log.isEmpty()||allowed.isEmpty())return;
  QJsonArray files;
  auto record=[&](const QString &path){
    QJsonObject item{{"path",path}};
    const auto actual=QFileInfo(path).canonicalFilePath();
    if(!actual.startsWith(allowed+'/',Qt::CaseInsensitive)){item["error"]="fixture path outside its own scratch root";files.append(item);return;}
    QFile input(actual);
    if(!input.open(QIODevice::ReadOnly)||input.size()>64LL*1024*1024){item["error"]="fixture cannot open bounded file";files.append(item);return;}
    QCryptographicHash hash(QCryptographicHash::Sha256);qint64 size=0;
    while(!input.atEnd()){
      auto chunk=input.read(65536);size+=chunk.size();
      if(size>64LL*1024*1024||input.error()!=QFileDevice::NoError){item["error"]="fixture read failed";break;}
      hash.addData(chunk);
    }
    item["size"]=size;item["sha256"]=QString::fromLatin1(hash.result().toHex());files.append(item);
  };
  std::function<void(const QJsonValue&,int)> visit=[&](const QJsonValue &v,int depth){
    if(depth>16||files.size()>32)return;
    if(v.isArray()){for(const auto &x:v.toArray())visit(x,depth+1);return;}
    if(!v.isObject())return;
    const auto object=v.toObject();
    if(object["type"]=="resource_link")record(QUrl(object["uri"].toString()).toLocalFile());
    if(object["type"]=="text")for(const auto &line:object["text"].toString().split('\n'))
      if(line.startsWith("ADE_ATTACHMENT_V1 ")){
        const auto descriptor=QJsonDocument::fromJson(line.mid(18).toUtf8()).object();
        if(descriptor["path"].isString())record(descriptor["path"].toString());
      }
    for(auto it=object.begin();it!=object.end();++it)if(it.value().isArray()||it.value().isObject())visit(it.value(),depth+1);
  };
  visit(frame,0);QFile output(log);
  if(output.open(QIODevice::WriteOnly|QIODevice::Append))
    output.write(QJsonDocument(QJsonObject{{"protocol",protocol},{"frame",frame},{"files",files}}).toJson(QJsonDocument::Compact)+'\n');
}

