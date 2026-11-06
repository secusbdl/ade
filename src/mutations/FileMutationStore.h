#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include "FileMutationExecutor.h"

namespace ade::mutations {
struct MutationEntry;
struct DirectoryEntry;
struct Edit { QString before,after; bool all=false; };
struct Request {
    QString agentId,nonce,toolCallId,tool,path,outputPath,content;
    QList<Edit> edits;
    bool move=false;
    QByteArray bytes;
    bool raw=false;
    QString invocationId;
};
struct Result {
    bool success=false,recoveryRequired=false;
    QString mutationId,error;
    QJsonObject output;
    QList<ade::FileMutationChange> changes;
};
enum class Fault { None,Fail,Interrupt };
// Fault-injection seam: changes timing/failure only, never filesystem authority.
using FaultHook=std::function<Fault(const QString& boundary,int entry)>;
class FileMutationStore {
public:
    explicit FileMutationStore(QString dataDirectory,QString agentId);
    ~FileMutationStore();
    FileMutationStore(const FileMutationStore&)=delete;
    FileMutationStore& operator=(const FileMutationStore&)=delete;
    bool open(QString* error);
    std::shared_ptr<void> storagePin()const{return m_storagePin;}
    bool bindMount(const QString& prefix,const QString& root,bool writable,QString* error,const QString& expectedIdentity={});
    QString mountIdentity(const QString& prefix) const;
    Result execute(const Request&,const std::atomic_bool* cancelled=nullptr);
    Result undo(const QString& agentId,const QString& originalNonce,const QString& undoNonce,const std::atomic_bool* cancelled=nullptr);
    Result undoMany(const QStringList& originalNonces,const QString& undoNonce,const std::atomic_bool* cancelled=nullptr);
    QList<Result> recover();
    QJsonObject describe(const QString& nonce,QString* error);
    QJsonArray history(QString* error,int limit=200);
    // Read only: exact owner/nonce selection, independent of current files.
    // A path requests a bounded text preview in addition to the full summary.
    QJsonObject turnFiles(const QStringList& mutationIds,const QString& path,QString* error);
    QJsonArray settledInvocations(const QStringList& invocationIds,QString* error);
    bool removeAgent(QString* error);
    void setFaultHook(FaultHook hook){m_fault=std::move(hook);}
private:
    struct Mount {QString root,identity;bool writable=false;};
    Result apply(const Request&,QList<MutationEntry>,QJsonObject output,const QString& undoOf,const std::atomic_bool*);
    bool rollback(const QString& nonce,QList<MutationEntry>&,QString* error);
    bool cleanup(QList<MutationEntry>&,QString* error);
    bool status(const QString& nonce,const QString& state,const QString& error,QString* failure);
    bool load(const QString& nonce,QString* agent,QString* state,QList<MutationEntry>* entries,QString* error);
    bool loadDirectories(const QString& nonce,QString* error);
    bool applyDirectories(const QString& nonce,bool creates,QString* error);
    bool rollbackDirectories(const QString& nonce,bool creates,QString* error);
    QString m_directory,m_connection,m_agentId;
    QSqlDatabase m_db;
    QMap<QString,Mount> m_mounts;
    FaultHook m_fault;
    QList<DirectoryEntry> m_directories;
    std::shared_ptr<void> m_storagePin;
};
}
