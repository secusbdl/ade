#pragma once
#include "GitService.h"
#include "WorktreeUsageJournal.h"
#include <QObject>
#include <QPointer>
#include <functional>

namespace ade {
struct WorktreeLiveOwner { QString path; qint64 activity=0; };
struct WorktreeCleanupRow { WorktreeCleanupFacts facts; WorktreeCleanupDecision decision; };
// Owner-thread coordinator. It records only successfully persisted host state;
// scans and user-reviewed removals run through the existing Git worker pool.
class WorktreeCleanupController final : public QObject {
    Q_OBJECT
public:
    WorktreeCleanupController(GitService *service,QString dataDirectory,
        const QByteArray &historyRevision,QObject *parent=nullptr);
    ~WorktreeCleanupController() override;
    void updateOwners(const QMap<QString,WorktreeLiveOwner> &owners);
    void setMutationGuard(std::function<bool(const QString &,GitOperation)> guard);
    bool closeClean(const QByteArray &historyRevision,QString *error=nullptr);
    QList<WorktreeCleanupRow> rows() const { return m_rows; }
    bool busy() const { return m_scan||m_remove||!m_removing.isEmpty()||!m_queue.isEmpty(); }
    bool trackingReady() const;
    QString trackingError() const;
    void refresh();
    bool snooze(const QStringList &identityKeys,QString *error=nullptr);
    void removeReviewed(const QStringList &identityKeys);
signals:
    void changed();
    void errorOccurred(const QString &message);
    void mutationStarted(const QString &path,ade::GitOperation operation);
    void mutationFinished(const QString &path,ade::GitOperation operation,bool success);
    void worktreeRemovalStarted(const QString &path);
    void worktreeRemovalFinished(const QString &path,bool success);
private:
    struct Pending { QString owner,path; qint64 activity=0; };
    void finished(quint64 id,const GitResult &result);
    void invalidate();
    void removeNext();
    QPointer<GitService> m_service;
    WorktreeUsageJournal m_journal;
    QMap<QString,WorktreeLiveOwner> m_owners;
    QMap<QString,WorktreeUsageIdentity> m_identities;
    QMap<quint64,Pending> m_pending;
    QSet<QString> m_failedOwners;
    QList<WorktreeCleanupRow> m_rows;
    QStringList m_queue;
    std::function<bool(const QString &,GitOperation)> m_guard;
    std::shared_ptr<std::atomic_bool> m_lease;
    std::shared_ptr<std::atomic_bool> m_scanLease;
    quint64 m_scan=0,m_remove=0;
    QString m_removing;
    bool m_closed=false;
};
}
