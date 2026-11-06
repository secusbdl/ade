#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

namespace ade::mutation_stage {
struct MutationEntry;
struct Edit { QString before,after; bool all=false; };
struct Request {
    QString agentId,nonce,toolCallId,tool,path,outputPath,content;
    QList<Edit> edits;
    bool move=false;
};
struct Result {
    bool success=false,recoveryRequired=false;
    QString mutationId,error;
    QJsonObject output;
};
enum class Fault { None,Fail,Interrupt };
// Private prototype seam: injection changes timing/failure only, never scope.
using FaultHook=std::function<Fault(const QString& boundary,int entry)>;
class FileMutationStore {
public:
    explicit FileMutationStore(QString dataDirectory,QString agentId);
    ~FileMutationStore();
    FileMutationStore(const FileMutationStore&)=delete;
    FileMutationStore& operator=(const FileMutationStore&)=delete;
    bool open(QString* error);
    bool bindMount(const QString& prefix,const QString& root,bool writable,QString* error);
    Result execute(const Request&,const std::atomic_bool* cancelled=nullptr);
    Result undo(const QString& agentId,const QString& originalNonce,const QString& undoNonce,const std::atomic_bool* cancelled=nullptr);
    QList<Result> recover();
    QJsonObject describe(const QString& nonce,QString* error);
    void setFaultHook(FaultHook hook){m_fault=std::move(hook);}
private:
    struct Mount {QString root,identity;bool writable=false;};
    Result apply(const Request&,QList<MutationEntry>,QJsonObject output,const QString& undoOf,const std::atomic_bool*);
    bool rollback(const QString& nonce,QList<MutationEntry>&,QString* error);
    bool cleanup(QList<MutationEntry>&,QString* error);
    bool status(const QString& nonce,const QString& state,const QString& error,QString* failure);
    bool load(const QString& nonce,QString* agent,QString* state,QList<MutationEntry>* entries,QString* error);
    QString m_directory,m_connection,m_agentId;
    QSqlDatabase m_db;
    QMap<QString,Mount> m_mounts;
    FaultHook m_fault;
    std::shared_ptr<void> m_storagePin;
};
}
