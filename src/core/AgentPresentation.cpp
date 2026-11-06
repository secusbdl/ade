#include "AgentPresentation.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace ade {
namespace {
QString freshId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString contentText(const QJsonValue& content){
    if(content.isString())return content.toString();
    QString result;
    for(const auto& value:content.toArray()){const auto part=value.toObject();if(part["type"]=="text"||part["type"]=="output_text")result+=part["text"].toString();}
    return result;
}
bool validDisplay(const QJsonObject& display){
    if(display["version"]!=1||!display["id"].isString()||display["id"].toString().isEmpty()||display["id"].toString().size()>128||!display["parts"].isArray()||display["parts"].toArray().size()>AgentPresentation::MaximumParts)return false;
    QSet<QString> ids;
    for(const auto& value:display["parts"].toArray()){
        const auto part=value.toObject();const auto id=part["id"].toString(),type=part["type"].toString();
        if(!value.isObject()||id.isEmpty()||id.size()>256||ids.contains(id)||!QStringList{"text","reasoning","tool","error"}.contains(type))return false;
        if(type!="tool"&&!part["text"].isString())return false;
        if(type=="tool"&&(!part["toolCallId"].isString()||!part["name"].isString()||!part["inputText"].isString()))return false;
        if(type=="tool"){
            if(!QStringList{"input-streaming","input-available","approval-requested","running","output-available","output-error","output-denied","canceled"}.contains(part["state"].toString()))return false;
        }else if(!QStringList{"streaming","done","error","canceled"}.contains(part["state"].toString()))return false;
        if(part.contains("durationMs")&&(!part["durationMs"].isDouble()||part["durationMs"].toDouble()<0))return false;
        ids.insert(id);
    }
    return true;
}
bool displayMatches(const QJsonObject& display,const QJsonObject& message){
    if(!validDisplay(display))return false;
    QString text,reasoning;QJsonArray tools;
    for(const auto& value:display["parts"].toArray()){
        const auto part=value.toObject();
        if(part["type"]=="text")text+=part["text"].toString();
        else if(part["type"]=="reasoning")reasoning+=part["text"].toString();
        else if(part["type"]=="tool")tools.append(part);
    }
    if(text!=contentText(message["content"]))return false;
    if(message["reasoning_content"].isString()&&reasoning!=message["reasoning_content"].toString())return false;
    const auto calls=message["tool_calls"].toArray();
    if(!calls.isEmpty()&&calls.size()!=tools.size())return false;
    QSet<QString> matched;
    for(const auto& value:calls){const auto call=value.toObject(),fn=call["function"].toObject();bool found=false;
        for(const auto& toolValue:tools){const auto tool=toolValue.toObject();if(call["id"]==tool["toolCallId"]&&!matched.contains(tool["id"].toString())){
            if(fn["name"]!=tool["name"]||fn["arguments"]!=tool["inputText"])return false;
            matched.insert(tool["id"].toString());found=true;break;
        }}
        if(!found)return false;
    }
    return true;
}
QJsonObject toolPart(const QString& id,const QJsonObject& call){
    const auto fn=call["function"].toObject();const auto inputText=fn["arguments"].toString();
    QJsonObject part{{"id",id},{"type","tool"},{"toolCallId",call["id"]},{"name",fn["name"]},{"inputText",inputText},{"state","input-available"}};
    QJsonParseError error;const auto input=QJsonDocument::fromJson(inputText.toUtf8(),&error);if(error.error==QJsonParseError::NoError&&input.isObject())part["input"]=input.object();return part;
}
QJsonArray legacyParts(const QJsonObject& message,const QString& id){
    QJsonArray parts;const auto calls=message["tool_calls"].toArray();int toolIndex=0;
    const auto appendText=[&](const QString& type,const QString& text){if(!text.isEmpty())parts.append(QJsonObject{{"id",id+"/"+QString::number(parts.size())},{"type",type},{"text",text},{"state","done"}});};
    const auto appendTool=[&]{if(toolIndex<calls.size())parts.append(toolPart(id+"/"+QString::number(parts.size()),calls[toolIndex++].toObject()));};
    const auto native=message["ade_native"].toObject();const auto protocol=native["protocol"].toString();
    if(message["role"]=="assistant"&&!native["content"].toArray().isEmpty()){
        for(const auto& value:native["content"].toArray()){
            const auto part=value.toObject();const auto type=part["type"].toString();
            if(protocol=="anthropic"){
                if(type=="thinking")appendText("reasoning",part["thinking"].toString());
                else if(type=="text")appendText("text",part["text"].toString());
                else if(type=="tool_use")appendTool();
            }else if(protocol=="google"){
                appendText(part["thought"].toBool()?"reasoning":"text",part["text"].toString());
                if(part["functionCall"].isObject())appendTool();
            }else if(protocol=="responses"){
                if(type=="message")for(const auto& item:part["content"].toArray()){const auto itemObject=item.toObject();appendText("text",itemObject["type"]=="refusal"?itemObject["refusal"].toString():itemObject["text"].toString());}
                else if(type=="reasoning")for(const auto& item:part["summary"].toArray())appendText("reasoning",item.toObject()["text"].toString());
                else if(type=="function_call")appendTool();
            }
        }
    }
    if(parts.isEmpty()){appendText("reasoning",message["reasoning_content"].toString());appendText("text",contentText(message["content"]));}
    while(toolIndex<calls.size())appendTool();
    return parts;
}
QString legacyId(const QJsonObject& message,QHash<QString,int>* occurrences){
    auto stable=message;auto metadata=stable["metadata"].toObject();metadata.remove("display");if(metadata.isEmpty())stable.remove("metadata");else stable["metadata"]=metadata;
    const auto hash=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(stable).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex().left(24));
    return "legacy-"+hash+'-'+QString::number((*occurrences)[hash]++);
}
}
void AgentPresentation::begin(){m_id=freshId();m_parts={};m_keys.clear();m_started.clear();m_clock.start();m_limitExceeded=false;}
int AgentPresentation::findPart(const QString& key)const{return m_keys.value(key,-1);}
bool AgentPresentation::allowNewPart(){if(m_parts.size()<MaximumParts)return true;m_limitExceeded=true;return false;}
void AgentPresentation::closeAt(int index,const QString& state){
    if(index<0||index>=m_parts.size())return;
    auto part=m_parts[index].toObject();
    if(part["type"]=="tool")return;
    if(part["state"]!="streaming")return;
    part["state"]=state;if(part["type"]=="reasoning"&&m_started.contains(index))part["durationMs"]=qMax<qint64>(0,m_clock.elapsed()-m_started.take(index));m_parts[index]=part;
}
void AgentPresentation::text(const QString& type,const QString& delta,const QString& key){
    // An explicit native part-start has an identity before its first delta.
    if(delta.isEmpty()&&key.isEmpty())return;
    if(m_id.isEmpty())begin();
    int index=key.isEmpty()?-1:findPart(key);
    if(index<0&&key.isEmpty()&&!m_parts.isEmpty()){
        const auto last=m_parts.last().toObject();if(last["type"]==type&&last["state"]=="streaming")index=m_parts.size()-1;
    }
    if(index<0){
        if(!allowNewPart())return;
        if(!m_parts.isEmpty())closeAt(m_parts.size()-1,"done");
        index=m_parts.size();
        m_parts.append(QJsonObject{{"id",m_id+"/"+QString::number(index)},{"type",type},{"text",QString()},{"state","streaming"}});
        if(!key.isEmpty())m_keys.insert(key,index);
        if(type=="reasoning")m_started[index]=m_clock.elapsed();
    }
    auto part=m_parts[index].toObject();part["text"]=part["text"].toString()+delta;m_parts[index]=part;
}
void AgentPresentation::tool(int index,const QString& callId,const QString& name,const QString& arguments){
    if(m_id.isEmpty())begin();
    const auto key="tool/"+QString::number(index);int position=findPart(key);
    if(position<0){if(!allowNewPart())return;if(!m_parts.isEmpty())closeAt(m_parts.size()-1,"done");position=m_parts.size();m_keys.insert(key,position);m_parts.append(QJsonObject{{"id",m_id+"/"+QString::number(position)},{"type","tool"},{"state","input-streaming"}});}
    auto part=m_parts[position].toObject();part["toolCallId"]=callId;part["name"]=name;part["inputText"]=arguments;
    QJsonParseError error;const auto input=QJsonDocument::fromJson(arguments.toUtf8(),&error);if(error.error==QJsonParseError::NoError&&input.isObject())part["input"]=input.object();else part.remove("input");m_parts[position]=part;
}
void AgentPresentation::setText(const QString& type,const QString& value,const QString& key){const int index=findPart(key);if(index<0){text(type,value,key);return;}auto part=m_parts[index].toObject();part["text"]=value;m_parts[index]=part;}
void AgentPresentation::closePart(const QString& key,const QString& state){closeAt(findPart(key),state);}
void AgentPresentation::toolState(const QString& callId,const QString& state){for(int i=0;i<m_parts.size();++i){auto part=m_parts[i].toObject();if(part["type"]=="tool"&&part["toolCallId"]==callId){part["state"]=state;m_parts[i]=part;return;}}}
QString AgentPresentation::toolOutput(const QString& callId)const{for(const auto& value:m_parts){const auto part=value.toObject();if(part["type"]=="tool"&&part["toolCallId"]==callId)return part["output"].toString();}return {};}
bool AgentPresentation::setToolOutput(const QString& callId,const QString& output){for(int i=0;i<m_parts.size();++i){auto part=m_parts[i].toObject();if(part["type"]=="tool"&&part["toolCallId"]==callId){if(part["output"]==output)return false;part["output"]=output;m_parts[i]=part;return true;}}return false;}
void AgentPresentation::finish(const QString& state){for(int i=0;i<m_parts.size();++i){auto part=m_parts[i].toObject();if(part["type"]=="tool"){if(part["state"]=="input-streaming"){part["state"]=state=="done"?"input-available":state=="error"?"output-error":state;m_parts[i]=part;}}else closeAt(i,state);}}
void AgentPresentation::error(const QString& text,const QString& state){if(text.isEmpty())return;if(m_id.isEmpty())begin();if(!allowNewPart())return;m_parts.append(QJsonObject{{"id",m_id+"/"+QString::number(m_parts.size())},{"type","error"},{"text",text},{"errorText",text},{"state",state}});}
QJsonArray AgentPresentation::parts()const{
    auto result=m_parts;for(auto it=m_started.cbegin();it!=m_started.cend();++it){auto part=result[it.key()].toObject();part["durationMs"]=qMax<qint64>(0,m_clock.elapsed()-it.value());result[it.key()]=part;}return result;
}
QJsonObject AgentPresentation::metadata(bool uiOnly)const{QJsonObject result{{"version",1},{"id",m_id},{"parts",parts()}};if(uiOnly)result["uiOnly"]=QJsonObject{{"version",1},{"kind","interrupted-reasoning"}};return result;}
QJsonObject AgentPresentation::row(bool provisional)const{return {{"id",m_id},{"role","assistant"},{"provisional",provisional},{"parts",parts()}};}
QJsonObject AgentPresentation::withMetadata(QJsonObject message,const QJsonObject& display){auto metadata=message["metadata"].toObject();metadata["display"]=display;message["metadata"]=metadata;return message;}
bool AgentPresentation::isUiOnly(const QJsonObject& message){
    const auto display=message["metadata"].toObject()["display"].toObject(),marker=display["uiOnly"].toObject();
    if(message["role"]!="assistant"||!validDisplay(display)||marker["version"]!=1||marker["kind"]!="interrupted-reasoning")return false;
    // Only the exact host-created envelope is eligible. Unknown replay fields
    // (legacy function_call, refusal, audio, or future provider fields) must not
    // disappear merely because an imported record carries a UI marker.
    for(auto it=message.begin();it!=message.end();++it)
        if(it.key()!="role"&&it.key()!="content"&&it.key()!="metadata")return false;
    if(!message["content"].isString()||!message["content"].toString().isEmpty()||message.contains("tool_calls")||message.contains("ade_native")||message.contains("reasoning_content"))return false;
    const auto metadata=message["metadata"].toObject();if((metadata.contains("attachments")&&(!metadata["attachments"].isArray()||!metadata["attachments"].toArray().isEmpty()))||metadata.contains("fileReads"))return false;
    bool reasoning=false;for(const auto& value:display["parts"].toArray()){const auto part=value.toObject();if(part["type"]=="reasoning")reasoning=true;else if(part["type"]!="error")return false;if(part["state"]!="error"&&part["state"]!="canceled"&&part["state"]!="done")return false;}return reasoning;
}
QJsonArray AgentPresentation::history(const QJsonArray& messages){
    QJsonArray result;QHash<QString,int> occurrences;int assistant=-1;QSet<QString> completed,rowIds;
    for(const auto& value:messages){
        const auto message=value.toObject();const auto role=message["role"].toString();if(role=="system")continue;
        if(role=="tool"&&assistant>=0){
            auto row=result[assistant].toObject();auto parts=row["parts"].toArray();const auto callId=message["tool_call_id"].toString();
            if(!completed.contains(callId))for(int i=0;i<parts.size();++i){auto part=parts[i].toObject();if(part["type"]!="tool"||part["toolCallId"]!=callId)continue;
                const auto output=contentText(message["content"]);const auto stored=message["metadata"].toObject()["display"].toObject();auto state=stored["version"]==1?stored["toolState"].toString():QString();
                if(!QStringList{"output-available","output-error","output-denied","canceled"}.contains(state))state=output.startsWith("Error:")?"output-error":"output-available";
                part["state"]=state;part["output"]=output;if(state!="output-available")part["errorText"]=output;parts[i]=part;completed.insert(callId);break;
            }
            row["parts"]=parts;const auto attachments=message["metadata"].toObject()["attachments"].toArray();if(!attachments.isEmpty()){auto all=row["attachments"].toArray();for(const auto& entry:attachments)all.append(entry);row["attachments"]=all;}result[assistant]=row;continue;
        }
        assistant=-1;completed.clear();const auto display=message["metadata"].toObject()["display"].toObject();const bool saved=displayMatches(display,message)&&!rowIds.contains(display["id"].toString());auto id=saved?display["id"].toString():legacyId(message,&occurrences);
        const auto baseId=id;int collision=1;while(rowIds.contains(id))id=baseId+'~'+QString::number(collision++);rowIds.insert(id);
        QJsonObject row{{"id",id},{"role",role},{"provisional",false},{"parts",saved?display["parts"].toArray():legacyParts(message,id)}};
        const auto attachments=message["metadata"].toObject()["attachments"].toArray();if(!attachments.isEmpty())row["attachments"]=attachments;
        result.append(row);if(role=="assistant")assistant=result.size()-1;
    }
    return result;
}
}
