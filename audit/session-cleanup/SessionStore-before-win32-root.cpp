#include "SessionStore.h"
#include "core/ScopedDirectory.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QLockFile>
#include <QSet>
#include <QRegularExpression>
#include <QFileInfo>
namespace ade {
namespace {
bool validAgentId(const QString& id){return QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(id).hasMatch();}
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
}
SessionStore::SessionStore(QString directory) : m_directory(std::move(directory)) {}
QList<Conversation> SessionStore::load(QString* error) const {
    if(error)error->clear();
    m_loaded=true;m_valid=false;m_existed=false;m_revision.clear();m_pendingCleanup.clear();
    m_canonicalDirectory=QFileInfo(m_directory).canonicalFilePath();
    QFile file(m_directory + "/sessions.json");
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
        if (!value.isObject()||!validAgentId(id)||ids.contains(id.toCaseFolded())||!o["messages"].isArray()||!o["title"].isString()||!validDraftAttachments(o["draftAttachments"])) {
            if(error)*error="Conversation storage contains invalid or duplicate records. Original data was preserved.";
            return {};
        }
        ids.insert(id.toCaseFolded());
        Conversation s;
        s.id=o["id"].toString(); s.title=o["title"].toString(); s.workspace=o["workspace"].toString();
        s.providerId=o["providerId"].toString(); s.model=o["model"].toString();
        s.presetId=o["presetId"].toString();s.draft=o["draft"].toString();
        s.draftAttachments=o["draftAttachments"].toArray();
        s.created=o["created"].toInteger(); s.updated=o["updated"].toInteger();
        s.archived=o["archived"].toBool(); s.messages=o["messages"].toArray();
        s.titleLocked=o["titleLocked"].toBool();s.autoTitleAttempted=o["autoTitleAttempted"].toBool();
        result.push_back(s);
    }
    const auto pending=doc.object()["pendingCleanup"];
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
bool SessionStore::save(const QList<Conversation>& sessions, QString* error) const {
    if(!m_loaded){QString loadError;load(&loadError);if(!loadError.isEmpty()){if(error)*error=loadError;return false;}}
    return saveWithCleanup(sessions,m_pendingCleanup,error);
}
bool SessionStore::saveWithCleanup(const QList<Conversation>& sessions,const QStringList& pendingCleanup,QString* error) const {
    if(error)error->clear();
    if(!m_loaded){QString loadError;load(&loadError);if(!loadError.isEmpty()){if(error)*error=loadError;return false;}}
    if(!m_valid){if(error)*error="Conversation storage failed validation; refusing to overwrite it.";return false;}
    if(!m_canonicalDirectory.isEmpty()&&QFileInfo(m_directory).canonicalFilePath().compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0){
        if(error)*error="Conversation storage moved; reload before saving.";
        return false;
    }
    if (!QDir().mkpath(m_directory)) { if (error) *error="Cannot create data directory"; return false; }
    QLockFile lock(m_directory+"/sessions.lock");
    if(!lock.tryLock(0)){if(error)*error="Another window is saving these conversations. Retry shortly.";return false;}
    if(!validateRevision(m_directory,error))return false;
    return writeState(m_directory,sessions,pendingCleanup,error);
}
bool SessionStore::validateRevision(const QString& directory,QString* error) const {
    if(!m_canonicalDirectory.isEmpty()&&QFileInfo(directory).canonicalFilePath().compare(m_canonicalDirectory,Qt::CaseInsensitive)!=0){
        if(error)*error="Conversation storage moved; reload before saving.";
        return false;
    }
    QFile existing(directory+"/sessions.json");
    if(existing.exists()!=m_existed){if(error)*error="Conversation storage changed in another window. Reload before saving.";return false;}
    if(m_existed){if(!existing.open(QIODevice::ReadOnly)||existing.size()>64*1024*1024||QCryptographicHash::hash(existing.readAll(),QCryptographicHash::Sha256)!=m_revision){if(error)*error="Conversation storage changed in another window. Reload before saving.";return false;}}
    return true;
}
bool SessionStore::writeState(const QString& directory,const QList<Conversation>& sessions,const QStringList& pendingCleanup,QString* error) const {
    QJsonArray records;
    QSet<QString> ids;
    for (const auto& s : sessions) {
        if(!validAgentId(s.id)||ids.contains(s.id.toCaseFolded())||!validDraftAttachments(s.draftAttachments)){if(error)*error="Cannot save invalid or duplicate conversation records.";return false;}
        ids.insert(s.id.toCaseFolded());
        records.append(QJsonObject{
        {"id",s.id},{"title",s.title},{"workspace",s.workspace},{"providerId",s.providerId},
        {"model",s.model},{"presetId",s.presetId},{"draft",s.draft},{"draftAttachments",s.draftAttachments},{"messages",s.messages},{"created",s.created},{"updated",s.updated},{"archived",s.archived},{"titleLocked",s.titleLocked},{"autoTitleAttempted",s.autoTitleAttempted}});
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
    m_canonicalDirectory=QFileInfo(directory).canonicalFilePath();
    return true;
}
bool SessionStore::processPendingCleanup(const QList<Conversation>& sessions,const Cleanup& cleanup,QString* error) const {
    if(error)error->clear();
    if(!m_loaded){QString failure;load(&failure);if(!failure.isEmpty()){if(error)*error=failure;return false;}}
    if(!m_valid||!cleanup){if(error)*error="Conversation cleanup cannot use invalid storage.";return false;}
    if(m_pendingCleanup.isEmpty())return true;
    ScopedDirectorySnapshot directory;bool truncated=false;
    if(!readScopedDirectory(m_directory,m_canonicalDirectory,&directory,error,0,nullptr,true,&truncated))return false;
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
