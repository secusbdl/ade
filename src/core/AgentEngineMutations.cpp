#include "AgentEngine.h"
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace ade {
namespace {
struct Binding { int assistant=-1, entry=-1, result=-1, end=-1; QJsonObject value; };
QString rowId(const QJsonObject& row) { return row["metadata"].toObject()["display"].toObject()["id"].toString(); }
Binding locate(const QJsonArray& history,const QString& invocation,const QString& agent) {
    Binding found;
    for(int i=0;i<history.size();++i){
        const auto assistant=history[i].toObject();
        if(assistant["role"]!="assistant")continue;
        const auto entries=assistant["metadata"].toObject()["mutationInvocations"].toArray();
        for(int j=0;j<entries.size();++j){
            const auto entry=entries[j].toObject();
            if(entry["invocationId"]!=invocation||entry["agentId"]!=agent)continue;
            // Ambiguous copied tokens never select a different batch.
            if(found.assistant>=0)return {};
            const auto call=entry["toolCallId"].toString();int matchingCalls=0;
            for(const auto& value:assistant["tool_calls"].toArray())if(value.toObject()["id"]==call)++matchingCalls;
            if(call.isEmpty()||matchingCalls!=1||rowId(assistant).isEmpty())return {};
            found={i,j,-1,i+1,entry};
            while(found.end<history.size()&&history[found.end].toObject()["role"]=="tool"){
                if(history[found.end].toObject()["tool_call_id"]==call){if(found.result>=0)return {};found.result=found.end;}
                ++found.end;
            }
        }
    }
    return found;
}
QJsonObject mergeMetadata(QJsonObject target,const QJsonObject& incoming) {
    auto records=target["mutations"].toArray();QSet<QString> keys;
    for(const auto& value:records){const auto record=value.toObject();keys.insert(record["agentId"].toString()+"/"+record["mutationId"].toString());}
    for(const auto& value:incoming["mutations"].toArray()){
        const auto record=value.toObject();const auto key=record["agentId"].toString()+"/"+record["mutationId"].toString();
        if(!keys.contains(key)){records.append(record);keys.insert(key);}
    }
    if(!records.isEmpty())target["mutations"]=records;
    if(incoming.contains("fileDiff"))target["fileDiff"]=incoming["fileDiff"];
    return target;
}
void setEntry(QJsonArray& history,const Binding& binding,const QJsonObject& entry){
    auto assistant=history[binding.assistant].toObject();auto metadata=assistant["metadata"].toObject();
    auto entries=metadata["mutationInvocations"].toArray();entries[binding.entry]=entry;
    metadata["mutationInvocations"]=entries;assistant["metadata"]=metadata;history[binding.assistant]=assistant;
}
}

void AgentEngine::setMutationContext(const QString& agentId,MutationRecovery recovery){
    if(m_busy||m_committingQueue)return;
    m_mutationAgentId=agentId;m_mutationRecovery=std::move(recovery);
    reconcileMutationHistory();
}

bool AgentEngine::registerMutation(const QString& invocation,const QString& call,bool external){
    if(!m_busy||m_committingQueue||QUuid(invocation).isNull()||call.isEmpty()||m_displayHistoryIndex<0||m_displayHistoryIndex>=m_messages.size())return false;
    auto assistant=m_messages[m_displayHistoryIndex].toObject();
    if(rowId(assistant)!=m_display.id()||assistant["role"]!="assistant")return false;
    int matchingCalls=0;for(const auto& value:assistant["tool_calls"].toArray())if(value.toObject()["id"]==call)++matchingCalls;
    if(matchingCalls!=1)return false;
    auto metadata=assistant["metadata"].toObject();auto entries=metadata["mutationInvocations"].toArray();
    // Bound dispatch attribution before launching any additional side effects.
    if(entries.size()>=256)return false;
    for(const auto& value:entries)if(value.toObject()["invocationId"]==invocation)return false;
    entries.append(QJsonObject{{"invocationId",invocation},{"toolCallId",call},{"agentId",m_mutationAgentId},{"external",external}});
    metadata["mutationInvocations"]=entries;assistant["metadata"]=metadata;
    auto candidate=m_messages;candidate[m_displayHistoryIndex]=assistant;
    const auto before=m_messages;const auto generation=m_generation;const QPointer<AgentEngine> self(this);QString error;
    const bool saved=commitQueue(candidate,m_queuedMessages,&error);
    if(!self||generation!=m_generation||!m_busy||m_messages!=before)return false;
    if(!saved){emit queueError("The file mutation was not started because its dispatch could not be saved. "+error);return false;}
    m_messages=std::move(candidate);if(m_queueCommitter)m_mutationHistoryDirty=false;return true;
}

bool AgentEngine::registerExternalMutation(const QString& invocation,const QString& request){
    const auto call=externalToolCallId(request);
    return !call.isEmpty()&&registerMutation(invocation,call,true);
}

void AgentEngine::settleExternalMutation(const QString& invocation,const QJsonObject& metadata){
    settleMutation(invocation,{},false,metadata);
}

void AgentEngine::settleMutation(const QString& invocation,const QString& output,bool error,const QJsonObject& metadata,bool recovering){
    auto binding=locate(m_messages,invocation,m_mutationAgentId);if(binding.assistant<0)return;
    if(binding.value["settled"].toBool()&&!recovering)return;
    auto candidate=m_messages;auto entry=binding.value;entry["settled"]=true;
    entry["resultMetadata"]=metadata;setEntry(candidate,binding,entry);
    const auto call=entry["toolCallId"].toString();
    if(!entry["external"].toBool()){
        auto message=binding.result>=0?candidate[binding.result].toObject():QJsonObject{{"role","tool"},{"tool_call_id",call}};
        message["content"]=(error?QStringLiteral("Error: "):QString{})+redact(output);
        message["metadata"]=mergeMetadata(message["metadata"].toObject(),metadata);
        message=AgentPresentation::withMetadata(message,QJsonObject{{"version",1},{"toolState",error?"output-error":"output-available"}});
        if(binding.result>=0)candidate[binding.result]=message;else candidate.insert(binding.end,message);
    }else if(binding.result>=0||recovering){
        auto message=binding.result>=0?candidate[binding.result].toObject():AgentPresentation::withMetadata(QJsonObject{{"role","tool"},{"tool_call_id",call},{"content","Action interrupted before the sandbox tool completed. Its recorded file changes were already settled."}},QJsonObject{{"version",1},{"toolState","canceled"}});
        message["metadata"]=mergeMetadata(message["metadata"].toObject(),metadata);
        if(binding.result>=0)candidate[binding.result]=message;else candidate.insert(binding.end,message);
    }
    const auto before=m_messages;const QPointer<AgentEngine> self(this);QString saveError;
    const bool saved=commitQueue(candidate,m_queuedMessages,&saveError);
    if(!self||m_messages!=before)return;
    m_messages=std::move(candidate);
    m_mutationHistoryDirty=!saved||!m_queueCommitter;
    // A late result can insert before the current streaming row.
    if(!m_display.id().isEmpty())for(int i=0;i<m_messages.size();++i)if(rowId(m_messages[i].toObject())==m_display.id()){m_displayHistoryIndex=i;break;}
    if(!saved){emit queueError("The committed file result remains in memory; its journal can recover it after restart. "+saveError);if(!self)return;}
    emit displayChanged(); // Deliberately does not advance or start another tool.
}

QJsonObject AgentEngine::mutationToolMessage(const QString& call,const QJsonObject& fallback)const{
    if(m_displayHistoryIndex<0||m_displayHistoryIndex>=m_messages.size())return fallback;
    const auto assistant=m_messages[m_displayHistoryIndex].toObject();if(rowId(assistant)!=m_display.id())return fallback;
    QJsonObject result=fallback;bool directSettled=false;
    for(const auto& value:assistant["metadata"].toObject()["mutationInvocations"].toArray()){
        const auto entry=value.toObject();if(entry["agentId"]!=m_mutationAgentId||entry["toolCallId"]!=call||!entry["settled"].toBool())continue;
        result["metadata"]=mergeMetadata(result["metadata"].toObject(),entry["resultMetadata"].toObject());
        directSettled|=!entry["external"].toBool();
    }
    if(directSettled)for(int i=m_displayHistoryIndex+1;i<m_messages.size()&&m_messages[i].toObject()["role"]=="tool";++i){
        const auto previous=m_messages[i].toObject();if(previous["tool_call_id"]==call)return previous;
    }
    return result;
}

void AgentEngine::storeToolMessage(const QString& call,const QJsonObject& message){
    if(m_displayHistoryIndex>=0&&m_displayHistoryIndex<m_messages.size()&&rowId(m_messages[m_displayHistoryIndex].toObject())==m_display.id()){
        for(int i=m_displayHistoryIndex+1;i<m_messages.size()&&m_messages[i].toObject()["role"]=="tool";++i)
            if(m_messages[i].toObject()["tool_call_id"]==call){m_messages[i]=message;return;}
    }
    m_messages.append(message);
}

void AgentEngine::reconcileMutationHistory(){
    if(m_busy||m_committingQueue||m_mutationAgentId.isEmpty()||!m_mutationRecovery)return;
    QStringList ids;
    for(const auto& value:m_messages)for(const auto& item:value.toObject()["metadata"].toObject()["mutationInvocations"].toArray()){
        const auto entry=item.toObject();const auto id=entry["invocationId"].toString();
        if(entry["agentId"]==m_mutationAgentId&&!QUuid(id).isNull()&&!ids.contains(id)){
            const auto binding=locate(m_messages,id,m_mutationAgentId);
            if(binding.assistant>=0&&(!entry["settled"].toBool()||binding.result<0))ids.append(id);
        }
    }
    const auto callback=m_mutationRecovery;const QPointer<AgentEngine> self(this);
    for(int offset=0;offset<ids.size();offset+=256){
        const auto requested=ids.mid(offset,256);const auto before=m_messages;QString error;const auto rows=callback(requested,&error);
        if(!self||m_busy||m_messages!=before)return;
        if(!error.isEmpty()){emit queueError("Saved file results could not be reconciled: "+error);return;}
        for(const auto& value:rows){
            const auto row=value.toObject();const auto id=row["invocationId"].toString();
            if(row["agentId"]!=m_mutationAgentId||!requested.contains(id))continue;
            const bool committed=row["committed"].toBool();auto output=row["output"].toObject();
            if(!row["error"].toString().isEmpty())output[committed?"warning":"error"]=row["error"];
            if(!output.contains("_diff"))output["_diff"]=QJsonValue::Null;
            const QJsonObject record{{"agentId",m_mutationAgentId},{"mutationId",row["mutationId"]},{"committed",committed},{"recoveryRequired",row["recoveryRequired"]}};
            settleMutation(id,QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact)),!committed,QJsonObject{{"mutations",QJsonArray{record}}},true);
            if(!self||m_busy)return;
        }
    }
    if(m_mutationHistoryDirty&&m_queueCommitter){
        const auto before=m_messages;QString error;const bool saved=commitQueue(before,m_queuedMessages,&error);
        if(!self||m_messages!=before)return;
        m_mutationHistoryDirty=!saved;
        if(!saved)emit queueError("Recovered file results could not be saved: "+error);
    }
}
}
