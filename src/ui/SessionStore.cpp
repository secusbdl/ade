#include "SessionStore.h"
#include "core/QueuedMessages.h"
#include "core/TurnFailure.h"
#include "core/ScopedDirectory.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QLockFile>
#include <QSet>
#include <QHash>
#include <QRegularExpression>
#include <QFileInfo>
#include <vector>
#include <algorithm>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace ade {
namespace {
bool validAgentId(const QString& id){return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch();}
bool validSideChatParents(const QList<Conversation>& sessions){
    QHash<QString,QString> parents;
    for(const auto& chat:sessions)parents.insert(chat.id,chat.sideChatParentId);
    for(const auto& chat:sessions)if(!chat.sideChatParentId.isEmpty())
        if(chat.sideChatParentId==chat.id||!parents.contains(chat.sideChatParentId)||!parents.value(chat.sideChatParentId).isEmpty())return false;
    return true;
}
QString resolvedDirectory(const QString& path) {
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return {};
    const auto close=std::shared_ptr<void>(handle,[](void* value){CloseHandle(static_cast<HANDLE>(value));});
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))return {};
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size||size>32768)return {};
    std::vector<wchar_t> buffer(size+1);
    const DWORD count=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=buffer.size())return {};
    auto result=QString::fromWCharArray(buffer.data(),count);
    if(result.startsWith("\\\\?\\UNC\\"))result="\\\\"+result.mid(8);else if(result.startsWith("\\\\?\\"))result.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(result));
#else
    return QFileInfo(path).canonicalFilePath();
#endif
}
bool pinDirectory(const QString& path,const QString& expected,ScopedDirectorySnapshot* directory,QString* error) {
    const auto root=expected.isEmpty()?resolvedDirectory(path):expected;bool truncated=false;
    if(!readScopedDirectory(path,root,directory,error,0,nullptr,true,&truncated))return false;
    if(directory->canonicalPath.compare(root,Qt::CaseInsensitive)!=0){if(error)*error="Conversation storage moved; reload before saving.";return false;}
    return true;
}
bool validDraftAttachments(const QJsonValue& value) {
    if(value.isUndefined())return true;
    if(!value.isArray()||value.toArray().size()>32)return false;
    for(const auto& entry:value.toArray()) {
        const auto ref=entry.toObject();
        if(!entry.isObject()||!ref["path"].isString()||ref["path"].toString().isEmpty()||ref["path"].toString().size()>32768
            ||(ref.contains("originalFileName")&&(!ref["originalFileName"].isString()||ref["originalFileName"].toString().size()>1024)))return false;
    }
    return true;
}
bool validTurnFailure(const QJsonValue& value){
    // The writer and loader share one predicate; the key is absent when empty.
    return value.isUndefined()||(value.isObject()&&TurnFailure::validShape(value.toObject()));
}
bool validLinkProvenance(const QJsonValue& value){
    if(value.isUndefined())return true;
    if(!value.isObject())return false;
    const auto object=value.toObject();
    if(object.size()>8192||QJsonDocument(object).toJson(QJsonDocument::Compact).size()>2*1024*1024)return false;
    for(auto it=object.begin();it!=object.end();++it)if(it.key().isEmpty()||it.key().size()>128||!it.value().isObject())return false;
    return true;
}
}
SessionStore::SessionStore(QString directory) : m_directory(std::move(directory)) {}
QList<Conversation> SessionStore::load(QString* error) const {
    if(error)error->clear();
    m_loaded=true;m_valid=false;m_existed=false;m_revision.clear();m_pendingCleanup.clear();
    m_canonicalDirectory.clear();
    ScopedDirectorySnapshot directory;
    if(QFileInfo(m_directory).exists()){
        if(!pinDirectory(m_directory,{},&directory,error))return {};
        m_canonicalDirectory=directory.canonicalPath;
    }
    QFile file((m_canonicalDirectory.isEmpty()?m_directory:m_canonicalDirectory) + "/sessions.json");
    if (!file.exists()) {m_valid=true;return {};}
    m_existed=true;
    if (!file.open(QIODevice::ReadOnly)||file.size()>64*1024*1024) { if (error) *error = "Cannot read conversation storage, or it exceeds 64 MiB."; return {}; }
    const auto bytes=file.readAll();m_revision=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()||doc.object()["schemaVersion"].toInt()!=1||!doc.object()["sessions"].isArray()) {
        if (error) *error = "Conversation storage is invalid: " + parse.errorString();
        return {};
    }
    QList<Conversation> result;
    QSet<QString> ids;
    for (const auto& value : doc.object().value("sessions").toArray()) {
        const auto o = value.toObject();
        const auto id=o["id"].toString();
        if (!value.isObject()||!validAgentId(id)||ids.contains(id.toCaseFolded())||!o["messages"].isArray()||!o["title"].isString()||!validDraftAttachments(o["draftAttachments"])
            ||(o.contains("queuedMessages")&&(!o["queuedMessages"].isArray()||!QueuedMessages::validate(o["queuedMessages"].toArray())))||!validLinkProvenance(o["transcriptLinkProvenance"])||!validTurnFailure(o.value("turnFailure"))) {
            if(error)*error="Conversation storage contains invalid or duplicate records. Original data was preserved.";
            return {};
        }
        ids.insert(id.toCaseFolded());
        Conversation s;
        if(o.contains("sideChatParentId")&&!o["sideChatParentId"].isString()) {if(error)*error="Invalid side-chat parent identity.";return {};}
        s.sideChatParentId=o["sideChatParentId"].toString();
        s.id=o["id"].toString(); s.title=o["title"].toString(); s.workspace=o["workspace"].toString();
        s.providerId=o["providerId"].toString(); s.model=o["model"].toString();
        s.presetId=o["presetId"].toString();s.draft=o["draft"].toString();
        s.draftAttachments=o["draftAttachments"].toArray();
        s.queuedMessages=o["queuedMessages"].toArray();
        s.transcriptLinkProvenance=o["transcriptLinkProvenance"].toObject();
        s.turnFailure=o.value("turnFailure").toObject();
        s.created=o["created"].toInteger(); s.updated=o["updated"].toInteger();s.archivedAt=o["archivedAt"].toInteger();
        s.archived=o["archived"].toBool();s.alwaysAllow=o["alwaysAllow"].toBool(); s.messages=o["messages"].toArray();
        if(o.contains("lastMessageAt")&&(!o["lastMessageAt"].isDouble()||o["lastMessageAt"].toDouble()!=double(o["lastMessageAt"].toInteger())||o["lastMessageAt"].toInteger()<0)){
            if(error)*error="Conversation activity timestamp is invalid. Original data was preserved.";
            return {};
        }
        s.lastMessageAt=o.contains("lastMessageAt")?o["lastMessageAt"].toInteger():(s.messages.isEmpty()?0:qMax<qint64>(0,s.updated));
        s.titleLocked=o["titleLocked"].toBool();s.autoTitleAttempted=o["autoTitleAttempted"].toBool();
        result.push_back(s);
    }
    if(!validSideChatParents(result)){if(error)*error="Side chat refers to a missing, nested, or cyclic parent. Original data was preserved.";return {};}
    // Legacy stores omit this optional field. value() preserves Undefined and
    // owns the result; mutable operator[] inserts null and returns a proxy into
    // the temporary object, which is already destroyed at the next statement.
    const auto pending=doc.object().value("pendingCleanup");
    if(!pending.isUndefined()&&(!pending.isArray()||pending.toArray().size()>10000)) {
        if(error)*error="Conversation cleanup records are invalid. Original data was preserved.";
        return {};
    }
    QSet<QString> cleanupIds,physicalIds;
    for(const auto& session:result)physicalIds.insert(session.id.toCaseFolded());
    for(const auto& entry:pending.toArray()) {
        const auto id=entry.toString(),key=id.toCaseFolded();
        if(!entry.isString()||!validAgentId(id)
            ||cleanupIds.contains(key)||physicalIds.contains(key)) {
            if(error)*error="Conversation cleanup records conflict with a retained chat. Original data was preserved.";
            return {};
        }
        cleanupIds.insert(key);m_pendingCleanup.append(id);
    }
    m_valid=true;return result;
}
bool SessionStore::canAdoptReload(const SessionStore& candidate,QString* error) const {
    if(error)error->clear();
    if(!candidate.m_loaded||!candidate.m_valid){if(error)*error="The reloaded conversation storage is invalid.";return false;}
    if(candidate.m_directory!=m_directory||(!m_canonicalDirectory.isEmpty()&&candidate.m_canonicalDirectory.compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0)){
        if(error)*error="Conversation storage moved. Reopen the original data directory; this window has not adopted the replacement.";
        return false;
    }
    return true;
}
bool SessionStore::save(const QList<Conversation>& sessions, QString* error) const {
    if(!m_loaded){QString loadError;load(&loadError);if(!loadError.isEmpty()){if(error)*error=loadError;return false;}}
    return saveWithCleanup(sessions,m_pendingCleanup,error);
}
bool SessionStore::saveWithCleanup(const QList<Conversation>& sessions,const QStringList& pendingCleanup,QString* error) const {
    if(error)error->clear();
    if(!m_loaded){QString loadError;load(&loadError);if(!loadError.isEmpty()){if(error)*error=loadError;return false;}}
    if(!m_valid){if(error)*error="Conversation storage failed validation; refusing to overwrite it.";return false;}
    if(!m_canonicalDirectory.isEmpty()&&resolvedDirectory(m_directory).compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0){
        if(error)*error="Conversation storage moved; reload before saving.";
        return false;
    }
    if (!QDir().mkpath(m_directory)) { if (error) *error="Cannot create data directory"; return false; }
    ScopedDirectorySnapshot directory;
    if(!pinDirectory(m_directory,m_canonicalDirectory,&directory,error))return false;
    const auto path=directory.canonicalPath;
    QLockFile lock(path+"/sessions.lock");
    if(!lock.tryLock(0)){if(error)*error="Another window is saving these conversations. Retry shortly.";return false;}
    if(!validateRevision(path,error))return false;
    return writeState(path,sessions,pendingCleanup,error);
}
bool SessionStore::validateRevision(const QString& directory,QString* error) const {
    if(!m_canonicalDirectory.isEmpty()&&resolvedDirectory(directory).compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0){
        if(error)*error="Conversation storage moved; reload before saving.";
        return false;
    }
    QFile existing(directory+"/sessions.json");
    if(existing.exists()!=m_existed){if(error)*error="Conversation storage changed in another window. Reload before saving.";return false;}
    if(m_existed){if(!existing.open(QIODevice::ReadOnly)||existing.size()>64*1024*1024||QCryptographicHash::hash(existing.readAll(),QCryptographicHash::Sha256)!=m_revision){if(error)*error="Conversation storage changed in another window. Reload before saving.";return false;}}
    return true;
}
bool SessionStore::writeState(const QString& directory,const QList<Conversation>& sessions,const QStringList& pendingCleanup,QString* error) const {
    if(!validSideChatParents(sessions)){if(error)*error="Cannot save a side chat with a missing, nested, or cyclic parent.";return false;}
    QJsonArray records;
    QSet<QString> ids;
    for (const auto& s : sessions) {
        if(!validAgentId(s.id)||ids.contains(s.id.toCaseFolded())||!validDraftAttachments(s.draftAttachments)||!QueuedMessages::validate(s.queuedMessages)||!validLinkProvenance(s.transcriptLinkProvenance)||(!s.turnFailure.isEmpty()&&!TurnFailure::validShape(s.turnFailure))||s.lastMessageAt<0){if(error)*error="Cannot save invalid or duplicate conversation records.";return false;}
        ids.insert(s.id.toCaseFolded());
        QJsonObject record{
        {"id",s.id},{"sideChatParentId",s.sideChatParentId},{"title",s.title},{"workspace",s.workspace},{"providerId",s.providerId},{"queuedMessages",s.queuedMessages},{"transcriptLinkProvenance",s.transcriptLinkProvenance},
        {"model",s.model},{"presetId",s.presetId},{"draft",s.draft},{"draftAttachments",s.draftAttachments},{"messages",s.messages},{"created",s.created},{"updated",s.updated},{"archivedAt",s.archivedAt},{"lastMessageAt",s.lastMessageAt},{"archived",s.archived},{"alwaysAllow",s.alwaysAllow},{"titleLocked",s.titleLocked},{"autoTitleAttempted",s.autoTitleAttempted}};
        if(!s.turnFailure.isEmpty())record["turnFailure"]=s.turnFailure;
        records.append(record);
    }
    if(pendingCleanup.size()>10000){if(error)*error="Too many pending conversation cleanup records.";return false;}
    QSet<QString> cleanupIds,physicalIds;
    for(const auto& session:sessions)physicalIds.insert(session.id.toCaseFolded());
    for(const auto& id:pendingCleanup){
        const auto key=id.toCaseFolded();
        if(!validAgentId(id)||cleanupIds.contains(key)||physicalIds.contains(key)){
            if(error)*error="Cannot queue cleanup for an invalid, duplicate or retained conversation.";
        return false;
        }
        cleanupIds.insert(key);
    }
    QSaveFile file(directory+"/sessions.json");
    if (!file.open(QIODevice::WriteOnly)) { if(error) *error=file.errorString(); return false; }
    const auto data=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",records},{"pendingCleanup",QJsonArray::fromStringList(pendingCleanup)}}).toJson();
    if(data.size()>64*1024*1024){if(error)*error="Conversation storage exceeds 64 MiB; the previous file was preserved.";return false;}
    if(file.write(data)!=data.size() || !file.commit()) { if(error) *error=file.errorString(); return false; }
    m_revision=QCryptographicHash::hash(data,QCryptographicHash::Sha256);m_existed=true;m_pendingCleanup=pendingCleanup;
    m_canonicalDirectory=directory;
    return true;
}
bool SessionStore::processPendingCleanup(const QList<Conversation>& sessions,const Cleanup& cleanup,QString* error) const {
    if(error)error->clear();
    if(!m_loaded){QString failure;load(&failure);if(!failure.isEmpty()){if(error)*error=failure;return false;}}
    if(!m_valid||!cleanup){if(error)*error="Conversation cleanup cannot use invalid storage.";return false;}
    if(m_pendingCleanup.isEmpty())return true;
    ScopedDirectorySnapshot directory;
    if(!pinDirectory(m_directory,m_canonicalDirectory,&directory,error))return false;
    if(!m_canonicalDirectory.isEmpty()&&directory.canonicalPath.compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0){
        if(error)*error="Conversation storage moved; reload before cleanup.";
        return false;
    }
    const auto path=directory.canonicalPath;
    QLockFile lock(path+"/sessions.lock");
    if(!lock.tryLock(0)){if(error)*error="Another window is saving these conversations. Retry cleanup later.";return false;}
    if(!validateRevision(path,error))return false;
    // The same inter-window lock remains held from candidate validation through
    // owned cleanup and the final marker commit. Never publish a retained chat
    // between validating its absence and deleting its old storage.
    if(!writeState(path,sessions,m_pendingCleanup,error))return false;
    const auto pending=m_pendingCleanup;QStringList remaining,errors;
    for(const auto& id:pending){
        QString failure;bool removed=false;
        try{removed=cleanup(id,path,&failure);}catch(...){failure="An owned file cleanup operation failed.";}
        if(!removed){remaining.append(id);errors.append(failure.isEmpty()?QString("An owned file cleanup operation failed."):failure);}
    }
    QString commitError;
    if(remaining!=pending&&!writeState(path,sessions,remaining,&commitError))errors.append(commitError);
    if(error)*error=errors.join("; ");
    return errors.isEmpty();
}
}
