#include "WorktreeUsageJournal.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QFileInfo>
#include <cmath>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace ade {
namespace {
constexpr qint64 MaxBytes=4*1024*1024,MaxTime=9007199254740991LL;
constexpr int MaxRecords=10000;
bool validTime(qint64 time,bool optional=false){return time>=(optional?-1:0)&&time<=MaxTime;}
bool validRevision(const QByteArray &revision){return revision.isEmpty()||revision.size()==32;}
bool integer(const QJsonValue &value,qint64 *out,bool optional=false){if(!value.isDouble())return false;const double n=value.toDouble();if(!std::isfinite(n)||std::floor(n)!=n||n<(optional?-1:0)||n>double(MaxTime))return false;*out=qint64(n);return true;}
struct State {QMap<QString,WorktreeUsageRecord> records;bool trackingOpen=false;qint64 recoverySince=-1;QByteArray historyRevision;};
bool pinStorageDirectory(QString *directory,std::shared_ptr<void> *pin,QString *error){
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(*directory).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE){*error="Cannot pin the worktree usage directory.";return false;}
    std::shared_ptr<void> held(handle,[](void *value){CloseHandle(static_cast<HANDLE>(value));});BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){*error="Worktree usage storage is not a directory.";return false;}
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size||size>32768){*error="Cannot resolve the pinned worktree usage directory.";return false;}
    std::vector<wchar_t> buffer(size+1);const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size()){*error="Cannot resolve the pinned worktree usage directory.";return false;}
    auto path=QString::fromWCharArray(buffer.data(),written);if(path.startsWith("\\\\?\\UNC\\"))path="\\\\"+path.mid(8);else if(path.startsWith("\\\\?\\"))path.remove(0,4);
    *directory=QDir::cleanPath(QDir::fromNativeSeparators(path));*pin=std::move(held);return true;
#else
    const auto path=QFileInfo(*directory).canonicalFilePath();if(path.isEmpty()){*error="Cannot resolve the worktree usage directory.";return false;}*directory=path;Q_UNUSED(pin);return true;
#endif
}
bool readBytes(const QString &path,QByteArray *bytes,bool *exists,QString *error){
    QFile file(path);*exists=file.exists();bytes->clear();if(!*exists)return true;
    if(!file.open(QIODevice::ReadOnly)){*error="Cannot read worktree usage journal.";return false;}
    *bytes=file.read(MaxBytes+1);if(bytes->size()>MaxBytes||file.error()!=QFileDevice::NoError){*error="Worktree usage journal cannot be read within its 4 MiB bound.";return false;}return true;
}
QByteArray encode(const State &state){
    QJsonArray entries;
    for(const auto &r:state.records)entries.append(QJsonObject{{"repository",r.identity.commonGitDir},{"path",r.identity.path},{"instance",r.identity.instanceToken},{"evidence",int(r.evidence)},{"firstObservedAt",r.firstObservedAt},{"lastUsedAt",r.lastUsedAt},{"createdAt",r.createdAt},{"dismissedAt",r.dismissedAt}});
    return QJsonDocument(QJsonObject{{"schemaVersion",1},{"trackingOpen",state.trackingOpen},{"recoverySince",state.recoverySince},{"historyRevision",QString::fromLatin1(state.historyRevision.toHex())},{"records",entries}}).toJson(QJsonDocument::Compact);
}
bool decode(const QByteArray &bytes,State *state,QString *error){
    auto fail=[&]{*error="Worktree usage journal is invalid; original bytes were preserved.";return false;};
    QJsonParseError parse;const auto doc=QJsonDocument::fromJson(bytes,&parse);
    if(parse.error!=QJsonParseError::NoError||!doc.isObject())return fail();
    const auto o=doc.object();
    if(o["schemaVersion"].toInteger(-1)!=1||!o["trackingOpen"].isBool()||!o["historyRevision"].isString()||!o["records"].isArray()||!integer(o["recoverySince"],&state->recoverySince,true))return fail();
    const auto revision=o["historyRevision"].toString().toLatin1();
    if(!revision.isEmpty()&&(revision.size()!=64||QByteArray::fromHex(revision).toHex()!=revision))return fail();
    state->trackingOpen=o["trackingOpen"].toBool();state->historyRevision=QByteArray::fromHex(revision);const auto entries=o["records"].toArray();if(entries.size()>MaxRecords)return fail();
    for(const auto &item:entries){
        if(!item.isObject())return fail();
        const auto r=item.toObject();WorktreeUsageRecord record;
        if(!r["repository"].isString()||!r["path"].isString()||!r["instance"].isString())return fail();
        record.identity={r["repository"].toString(),r["path"].toString(),r["instance"].toString(),true};qint64 evidence=0;
        if(!record.identity.valid()||!integer(r["evidence"],&evidence)||evidence>2||!integer(r["firstObservedAt"],&record.firstObservedAt)||!integer(r["lastUsedAt"],&record.lastUsedAt,true)||!integer(r["createdAt"],&record.createdAt,true)||!integer(r["dismissedAt"],&record.dismissedAt,true))return fail();
        record.evidence=WorktreeUsageEvidence(evidence);
        if((record.evidence==WorktreeUsageEvidence::VerifiedLiveUse&&record.lastUsedAt<0)||(record.evidence==WorktreeUsageEvidence::VerifiedNewInstance&&record.createdAt<0)||state->records.contains(record.identity.key()))return fail();
        state->records.insert(record.identity.key(),record);
    }
    return true;
}
}
struct WorktreeUsageJournal::Private {
    QString directory,path,error;std::shared_ptr<void> storagePin;std::unique_ptr<QLockFile> lock;State state;bool opened=false,healthy=false,existed=false;QByteArray revision;
    bool fail(const QString &reason,QString *out,bool invalidate=false){error=reason;if(out)*out=reason;if(invalidate)healthy=false;return false;}
    bool verifyRevision(QString *out){
        if(!opened||!healthy||!lock||!lock->isLocked())return fail("Worktree usage tracking is unavailable.",out);
        QByteArray current;bool exists=false;QString message;
        if(!readBytes(path,&current,&exists,&message))return fail(message,out,true);
        if(exists!=existed||(exists&&QCryptographicHash::hash(current,QCryptographicHash::Sha256)!=revision))return fail("Worktree usage journal changed externally; reload and recover before tracking.",out,true);
        if(out)out->clear();
        return true;
    }
    bool persist(const State &next,QString *out){
        if(!opened||!healthy||!lock||!lock->isLocked())return fail("Worktree usage tracking is unavailable.",out);
        const auto bytes=encode(next);if(next.records.size()>MaxRecords||bytes.size()>MaxBytes)return fail("Worktree usage journal exceeds its record or byte bound.",out,true);
        if(!verifyRevision(out))return false;
        QSaveFile file(path);
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())return fail("Cannot write worktree usage journal.",out,true);
        if(!verifyRevision(out))return false;
        if(!file.commit())return fail("Cannot commit worktree usage journal.",out,true);
        state=next;revision=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);existed=true;error.clear();if(out)out->clear();return true;
    }
};
WorktreeUsageJournal::WorktreeUsageJournal(QString directory):d(std::make_unique<Private>()){d->directory=std::move(directory);d->path=QDir(d->directory).filePath("worktree-usage.json");}
WorktreeUsageJournal::~WorktreeUsageJournal()=default;
bool WorktreeUsageJournal::open(qint64 now,const QByteArray &history,QString *error){
    if(d->opened)return d->fail("Worktree usage journal is already open.",error);
    if(!validTime(now)||!validRevision(history)||d->directory.isEmpty()||d->directory.contains(QChar::Null))return d->fail("Invalid journal clock, history revision or directory.",error);
    if(!QDir().mkpath(d->directory))return d->fail("Cannot create worktree usage directory.",error);
    QString message;
    if(!pinStorageDirectory(&d->directory,&d->storagePin,&message))return d->fail(message,error);
    d->path=QDir(d->directory).filePath("worktree-usage.json");
    d->lock=std::make_unique<QLockFile>(QDir(d->directory).filePath("worktree-usage.lock"));d->lock->setStaleLockTime(0);
    if(!d->lock->tryLock(0)){d->storagePin.reset();return d->fail("Worktree usage journal is owned by another application instance.",error);}
    QByteArray bytes;bool exists=false;State state;
    if(!readBytes(d->path,&bytes,&exists,&message)||(exists&&!decode(bytes,&state,&message))){d->lock->unlock();d->storagePin.reset();return d->fail(message,error);}
    d->opened=true;d->healthy=true;d->existed=exists;d->revision=exists?QCryptographicHash::hash(bytes,QCryptographicHash::Sha256):QByteArray();
    if(!exists||state.trackingOpen||history.isEmpty()||state.historyRevision!=history)state.recoverySince=qMax(state.recoverySince,now);
    state.trackingOpen=true;state.historyRevision.clear();
    return d->persist(state,error);
}
bool WorktreeUsageJournal::closeClean(const QByteArray &history,QString *error){
    if(!validRevision(history))return d->fail("Invalid external history revision.",error);
    State next=d->state;next.trackingOpen=false;next.historyRevision=history;
    if(!d->persist(next,error))return false;
    d->healthy=false;d->opened=false;d->lock->unlock();d->storagePin.reset();return true;
}
bool WorktreeUsageJournal::update(const WorktreeUsageIdentity &identity,qint64 now,WorktreeUsageEvidence evidence,qint64 hint,QString *error){
    if(!identity.valid()||!validTime(now)||!validTime(hint,true))return d->fail("Invalid worktree usage identity or timestamp.",error);
    State next=d->state;const auto key=identity.key();auto found=next.records.find(key);
    if(found==next.records.end()){WorktreeUsageRecord r;r.identity=identity;r.firstObservedAt=now;found=next.records.insert(key,r);}else found->identity=identity;
    if(evidence==WorktreeUsageEvidence::VerifiedLiveUse){found->evidence=evidence;found->lastUsedAt=qMax(found->lastUsedAt,now);}
    else if(evidence==WorktreeUsageEvidence::VerifiedNewInstance&&found->evidence!=WorktreeUsageEvidence::VerifiedLiveUse){found->evidence=evidence;if(found->createdAt<0)found->createdAt=now;}
    if(hint>=0)found->lastUsedAt=qMax(found->lastUsedAt,hint);
    return d->persist(next,error);
}
bool WorktreeUsageJournal::observeImported(const WorktreeUsageIdentity &id,qint64 now,qint64 hint,QString *error){return update(id,now,WorktreeUsageEvidence::UnknownImportedHistory,hint,error);}
bool WorktreeUsageJournal::recordLiveUse(const WorktreeUsageIdentity &id,qint64 now,QString *error){return update(id,now,WorktreeUsageEvidence::VerifiedLiveUse,-1,error);}
bool WorktreeUsageJournal::recordNewInstance(const WorktreeUsageIdentity &id,qint64 now,QString *error){return update(id,now,WorktreeUsageEvidence::VerifiedNewInstance,-1,error);}
bool WorktreeUsageJournal::snooze(const QList<WorktreeUsageIdentity> &identities,qint64 now,QString *error){
    if(!validTime(now)||identities.size()>MaxRecords)return d->fail("Invalid dismissal timestamp or count.",error);
    State next=d->state;
    for(const auto &id:identities){if(!id.valid()||!next.records.contains(id.key()))return d->fail("Cannot dismiss a worktree without verified usage identity.",error);next.records[id.key()].dismissedAt=qMax(next.records[id.key()].dismissedAt,now);}
    return d->persist(next,error);
}
bool WorktreeUsageJournal::pruneSnoozes(const QList<WorktreeUsageIdentity> &active,bool complete,qint64 now,QString *error){
    if(!complete)return d->fail("Incomplete candidate scans cannot prune dismissals.",error);
    if(!validTime(now)||active.size()>MaxRecords)return d->fail("Invalid scan timestamp or candidate count.",error);
    QSet<QString> keys;
    for(const auto &id:active){if(!id.valid())return d->fail("Invalid candidate identity.",error);keys.insert(id.key());}
    State next=d->state;
    for(auto &record:next.records)if(record.dismissedAt>=0&&(!keys.contains(record.identity.key())||(record.dismissedAt<=now&&now-record.dismissedAt>=WorktreeUnusedMs)))record.dismissedAt=-1;
    return d->persist(next,error);
}
WorktreeUsageRecord WorktreeUsageJournal::record(const WorktreeUsageIdentity &identity)const{return identity.valid()?d->state.records.value(identity.key()):WorktreeUsageRecord();}
QList<WorktreeUsageRecord> WorktreeUsageJournal::records()const{return d->state.records.values();}
WorktreeUsageHealth WorktreeUsageJournal::health()const{return {d->opened&&d->healthy,false,d->state.recoverySince};}
WorktreeUsageHealth WorktreeUsageJournal::validatedHealth(bool complete,QString *error){const bool valid=d->verifyRevision(error);return {valid,valid&&complete,d->state.recoverySince};}
QString WorktreeUsageJournal::lastError()const{return d->error;}
}
