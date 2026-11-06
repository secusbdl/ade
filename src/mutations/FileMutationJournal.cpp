#include "FileMutationJournal.h"
#include "FileMutationStore.h"
#include "FileMutationDiff.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QUuid>

namespace ade {
namespace {
struct Binding {QString root,identity;bool writable=false;};
QString nonce(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
FileMutationOutcome rejected(const QString& error){return {error,true,{},{}};}
QString logicalPath(QString path,const QMap<QString,Binding>& mounts,QString* error){
    path=QDir::fromNativeSeparators(path);
    if(path.isEmpty()||path.contains(QChar::Null)){*error="A nonempty file path is required.";return {};}
    if(QFileInfo(path).isAbsolute()){
        const auto root=mounts.value("ws1").root;
        if(root.isEmpty()||!path.startsWith(root+'/',Qt::CaseInsensitive)){*error="Absolute mutation paths must be below the connected workspace.";return {};}
        path="ws1/"+path.mid(root.size()+1);
    }else{
        auto prefix=path.section('/',0,0);
        if(prefix=="w1"||prefix=="workspace")path="ws1"+path.mid(prefix.size());
        else if(!mounts.contains(prefix)){
            if(QStringList{"att","apps","plugins","logs","shells","memory","plans"}.contains(prefix)||QRegularExpression("^w(?:s)?[0-9a-f]+$").match(prefix).hasMatch()){*error="The requested mutation mount is unavailable.";return {};}
            path="ws1/"+path;
        }
    }
    return path;
}
FileMutationOutcome outcome(const mutations::Result& result,const QString& agent){
    QJsonObject record{{"mutationId",result.mutationId},{"agentId",agent},{"committed",result.success},{"recoveryRequired",result.recoveryRequired}};
    QJsonObject value=result.output;if(!result.error.isEmpty())value[result.success?"warning":"error"]=result.error;
    if(!value.contains("_diff"))value["_diff"]=QJsonValue::Null;
    QJsonObject metadata{{"mutations",QJsonArray{record}}};
    if(result.success&&!result.changes.isEmpty())metadata["fileDiff"]=mutationDiffMetadata(result.changes);
    return {QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)),!result.success,metadata,result.changes};
}
}
struct FileMutationJournal::State {
    QString directory,agent;
    QString initialError;
    std::shared_ptr<void> storagePin;
    std::atomic_bool revoked=false;
    QMutex mutex;
    QMap<QString,Binding> mounts;
    std::function<void(const QString&,int)> observer;
};
namespace {
QMutex registryMutex;
QHash<QString,QList<std::weak_ptr<void>>> registry;
QString registryKey(const QString& directory,const QString& agent){return QDir::cleanPath(QFileInfo(directory).absoluteFilePath()).toCaseFolded()+'\n'+agent.toCaseFolded();}
}
FileMutationJournal::FileMutationJournal(QString directory,QString agent):m_state(std::make_shared<State>()){
    m_state->directory=QDir::cleanPath(QFileInfo(directory).absoluteFilePath());m_state->agent=std::move(agent);
    mutations::FileMutationStore storage(m_state->directory,m_state->agent);
    if(storage.open(&m_state->initialError))m_state->storagePin=storage.storagePin();
    QMutexLocker lock(&registryMutex);registry[registryKey(directory,m_state->agent)].append(m_state);
}
bool FileMutationJournal::bindMount(const QString& prefix,const QString& directory,bool writable,QString* error){
    if(error)error->clear();
    QMutexLocker lock(&m_state->mutex);
    if(m_state->revoked){if(error)*error="Mutation context was revoked.";return false;}
    mutations::FileMutationStore probe(m_state->directory,m_state->agent);QString failure;
    if(!probe.bindMount(prefix,directory,writable,&failure)){if(error)*error=failure;return false;}
    m_state->mounts[prefix]={QDir::fromNativeSeparators(QFileInfo(directory).canonicalFilePath()),probe.mountIdentity(prefix),writable};return true;
}
void FileMutationJournal::unbindMount(const QString& prefix){QMutexLocker lock(&m_state->mutex);m_state->mounts.remove(prefix);}
FileMutationExecutor FileMutationJournal::executor()const{
    const auto state=m_state;
    return [state](const FileMutationInvocation& input,const std::atomic_bool* canceled){
        if(!state->initialError.isEmpty())return rejected(state->initialError);
        if(state->revoked||(canceled&&canceled->load()))return rejected("Mutation canceled before preparation.");
        QMap<QString,Binding> mounts;std::function<void(const QString&,int)> observer;{QMutexLocker lock(&state->mutex);mounts=state->mounts;observer=state->observer;}
        if(input.workspace.compare(mounts.value("ws1").root,Qt::CaseInsensitive)||input.appsDirectory.compare(mounts.value("apps").root,Qt::CaseInsensitive))return rejected("Tool roots no longer match this chat's bound mutation context.");
        QString error;mutations::FileMutationStore store(state->directory,state->agent);
        if(!store.open(&error))return rejected(error);
        for(auto it=mounts.cbegin();it!=mounts.cend();++it)if(!store.bindMount(it.key(),it->root,it->writable,&error,it->identity))return rejected(error);
        store.setFaultHook([state,observer](const QString& boundary,int index){if(observer)observer(boundary,index);return state->revoked&&!boundary.startsWith("rollback-")?mutations::Fault::Fail:mutations::Fault::None;});
        for(const auto& recovered:store.recover())if(!recovered.success)return rejected("Pending mutation recovery requires attention: "+recovered.error);
        mutations::Request request;request.agentId=state->agent;request.nonce=nonce();request.toolCallId=input.requestId;request.tool=input.tool;request.raw=input.arguments["raw"].toBool();
        const auto& args=input.arguments;
        const auto rawPath=args.value(input.tool=="copy"?"input_path":"path");if(!rawPath.isString())return rejected("Mutation path must be a string.");
        request.path=logicalPath(rawPath.toString(),mounts,&error);if(!error.isEmpty())return rejected(error);
        if(input.tool=="copy"){
            if(!args["output_path"].isString()||!args["move"].isBool())return rejected("copy requires output_path and an explicit move boolean.");
            request.outputPath=logicalPath(args["output_path"].toString(),mounts,&error);if(!error.isEmpty())return rejected(error);request.move=args["move"].toBool();
        }else if(input.tool=="write"){
            if(!args["content"].isString()||args["content"].toString().size()>4*1024*1024)return rejected("write requires bounded string content.");
            request.content=args["content"].toString();
        }else if(input.tool=="writeBytes"){
            if(!args["base64"].isString()||args["base64"].toString().size()>4*((4*1024*1024+2)/3))return rejected("Binary body exceeds4MiB or is invalid.");
            const auto decoded=QByteArray::fromBase64Encoding(args["base64"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!decoded)return rejected("Invalid base64 file data.");request.bytes=decoded.decoded;request.raw=true;
        }else if(input.tool=="multiEdit"){
            if(!args["edits"].isArray()||args["edits"].toArray().isEmpty()||args["edits"].toArray().size()>1000)return rejected("multiEdit requires one to1000 edits.");
            for(const auto& value:args["edits"].toArray()){const auto edit=value.toObject();if(!edit["old_string"].isString()||!edit["new_string"].isString()||(edit.contains("replace_all")&&!edit["replace_all"].isBool()))return rejected("Invalid multiEdit item.");request.edits.append({edit["old_string"].toString(),edit["new_string"].toString(),edit["replace_all"].toBool()});}
        }
        return outcome(store.execute(request,canceled),state->agent);
    };
}
FileMutationOutcome FileMutationJournal::undo(const QStringList& ids,const QString& requestId,const std::atomic_bool* canceled)const{
    Q_UNUSED(requestId);
    const auto state=m_state;if(!state->initialError.isEmpty())return rejected(state->initialError);if(state->revoked)return rejected("Mutation context was revoked.");
    QMap<QString,Binding> mounts;{QMutexLocker lock(&state->mutex);mounts=state->mounts;}
    mutations::FileMutationStore store(state->directory,state->agent);QString error;if(!store.open(&error))return rejected(error);
    for(auto it=mounts.cbegin();it!=mounts.cend();++it)if(!store.bindMount(it.key(),it->root,it->writable,&error,it->identity))return rejected(error);
    store.setFaultHook([state](const QString& boundary,int){return state->revoked&&!boundary.startsWith("rollback-")?mutations::Fault::Fail:mutations::Fault::None;});
    for(const auto& recovered:store.recover())if(!recovered.success)return rejected("Pending mutation recovery requires attention: "+recovered.error);
    return outcome(store.undoMany(ids,nonce(),canceled),state->agent);
}
QJsonArray FileMutationJournal::history(QString* error,int limit)const{
    if(!m_state->initialError.isEmpty()){if(error)*error=m_state->initialError;return {};}
    if(m_state->revoked){if(error)*error="Mutation context was revoked.";return {};}
    mutations::FileMutationStore store(m_state->directory,m_state->agent);if(!store.open(error))return {};return store.history(error,limit);
}
FileMutationOutcome FileMutationJournal::recover(const std::atomic_bool* canceled)const{
    const auto state=m_state;if(!state->initialError.isEmpty())return rejected(state->initialError);
    if(state->revoked||(canceled&&canceled->load()))return rejected("Mutation recovery was canceled.");
    QMap<QString,Binding> mounts;{QMutexLocker lock(&state->mutex);mounts=state->mounts;}
    mutations::FileMutationStore store(state->directory,state->agent);QString error;if(!store.open(&error))return rejected(error);
    for(auto it=mounts.cbegin();it!=mounts.cend();++it)if(!store.bindMount(it.key(),it->root,it->writable,&error,it->identity))return rejected(error);
    // Once recovery starts, finish or report a conflict for each retained record;
    // never interrupt a rollback merely because its UI observer disappeared.
    FileMutationOutcome result;QJsonArray records;
    for(const auto& recovered:store.recover()){
        records.append(QJsonObject{{"mutationId",recovered.mutationId},{"recovered",recovered.success},{"recoveryRequired",recovered.recoveryRequired},{"error",recovered.error}});
        result.error=result.error||!recovered.success;result.changes.append(recovered.changes);
    }
    result.output=QString::fromUtf8(QJsonDocument(QJsonObject{{"message",result.error?"Some mutations require conflict resolution.":"Mutation recovery completed."},{"records",records}}).toJson(QJsonDocument::Compact));return result;
}
void FileMutationJournal::revoke(){m_state->revoked=true;}
void FileMutationJournal::setBoundaryObserverForTests(std::function<void(const QString&,int)> observer){QMutexLocker lock(&m_state->mutex);m_state->observer=std::move(observer);}
bool FileMutationJournal::removeAgent(const QString& directory,const QString& agent,QString* error){
    {QMutexLocker lock(&registryMutex);const auto key=registryKey(directory,agent);for(const auto& weak:registry.take(key))if(auto value=weak.lock())std::static_pointer_cast<State>(value)->revoked=true;}
    mutations::FileMutationStore store(directory,agent);return store.open(error)&&store.removeAgent(error);
}
}
