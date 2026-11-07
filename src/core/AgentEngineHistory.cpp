#include "AgentEngine.h"
#include "HarnessHistory.h"
#include "TurnFailure.h"
#include <QUuid>

namespace ade {
namespace {
int lastUserIndex(const QJsonArray& history){
    for(int i=history.size()-1;i>=0;--i)if(history[i].toObject()["role"]=="user")return i;
    return -1;
}
QJsonArray head(const QJsonArray& history,int count){
    QJsonArray result;
    for(int i=0;i<count&&i<history.size();++i)result.append(history[i]);
    return result;
}
}

bool AgentEngine::validTurnFailureShape(const QJsonObject& record){return TurnFailure::validShape(record);}

bool AgentEngine::validTurnFailure(const QJsonArray& history,const QJsonObject& record){
    if(!validTurnFailureShape(record))return false;
    const int index=lastUserIndex(history);if(index<0)return false;
    const auto row=record["userRowId"].toString();
    const auto display=history[index].toObject()["metadata"].toObject()["display"].toObject();
    if(display["version"]!=1||display["id"]!=row)return false;
    // The projection uses the saved ID only for a matching, non-colliding
    // envelope. Legacy and collided rows are never retryable after a restart.
    const auto projected=AgentPresentation::history(head(history,index+1));
    if(projected.isEmpty())return false;
    const auto user=projected.last().toObject();
    return user["role"]=="user"&&user["id"]==row;
}

bool AgentEngine::restoreTurnFailure(const QJsonObject& record){
    if(m_busy||m_committingQueue)return false;
    m_turnFailure={};
    if(record.isEmpty())return true;
    if(!validTurnFailure(m_messages,record))return false;
    m_turnFailure=record;m_turnUserRowId=record["userRowId"].toString();
    return true;
}

bool AgentEngine::retryFailedTurn(const QString& userRowId,QString* error){
    const auto refuse=[error](const QString& text){if(error)*error=text;return false;};
    if(error)error->clear();
    if(m_busy||m_committingQueue||isCompressing())return refuse("Wait for the agent to finish before retrying.");
    if(m_turnFailure.isEmpty()||m_turnFailure["userRowId"].toString()!=userRowId)return refuse("This turn can no longer be retried.");
    // A settlement the journal committed but this session never applied would
    // otherwise block Retry indefinitely; the recovery callback can re-enter.
    {const QPointer<AgentEngine> guard(this);reconcileMutationHistory();if(!guard)return false;}
    if(m_busy||m_committingQueue||isCompressing())return refuse("Wait for the agent to finish before retrying.");
    if(m_turnFailure.isEmpty()||m_turnFailure["userRowId"].toString()!=userRowId||!validTurnFailure(m_messages,m_turnFailure))
        return refuse("This turn can no longer be retried.");
    const int index=lastUserIndex(m_messages);
    const auto original=m_messages[index].toObject();
    if(!original["content"].isString())return refuse("This turn can no longer be retried.");
    // A late settlement whose invocation row is removed would be dropped.
    if(unsettledMutationAfter(index+1))
        return refuse("File changes from the failed turn are still being recorded. Retry after they settle.");
    // Same semantics as a history change through setMessages, but while idle and
    // before the provider is resolved: stopping can preserve a question row.
    const auto before=m_messages;const auto generation=m_generation;const QPointer<AgentEngine> self(this);
    stopHarness();
    if(!self)return false;
    m_fallback.reset();
    if(m_busy||m_committingQueue||generation!=m_generation||m_messages!=before)return refuse("The conversation changed; try again.");
    // A failed harness turn binds nothing, so the prefix could still resume the
    // earlier external session. Force a fresh session with the canonical transcript.
    const auto prefix=HarnessHistory::withoutBindings(head(m_messages,index));
    const auto content=original["content"].toString();
    auto metadata=original["metadata"].toObject();metadata.remove("display");
    QJsonObject rebuilt{{"role","user"},{"content",content}};
    if(original.contains("ade_compressedHistory"))rebuilt["ade_compressedHistory"]=original["ade_compressedHistory"];
    if(!metadata.isEmpty())rebuilt["metadata"]=metadata;
    // Native difference: upstream reuses the message ID; a fresh display envelope
    // keeps projection identity unambiguous after replacement.
    const auto displayId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    rebuilt=AgentPresentation::withMetadata(rebuilt,QJsonObject{{"version",1},{"id",displayId},{"parts",QJsonArray{QJsonObject{{"id",displayId+"/0"},{"type","text"},{"text",content},{"state","done"}}}}});
    QString startError;
    const auto rejected=connect(this,&AgentEngine::queueError,this,[&startError](const QString& text){startError=text;});
    const auto failure=connect(this,&AgentEngine::failed,this,[&startError](const QString& text){startError=text;});
    m_queuePaused=false;
    const bool accepted=startUserMessage(content,metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject(),{},rebuilt,prefix);
    if(!self)return accepted;
    disconnect(rejected);disconnect(failure);
    if(!accepted){
        m_queuePaused=true;
        return refuse(startError.isEmpty()?QString("The failed turn could not be retried."):startError);
    }
    return true;
}
}
