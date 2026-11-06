#pragma once
#include "WorktreeUsage.h"
#include <QByteArray>
#include <memory>
namespace ade {
// A durable usage ledger, not workspace authority. Call open before tracking;
// only explicit closeClean after all session writes records a clean shutdown.
// Default/unknown external history revision starts a new recovery floor.
class WorktreeUsageJournal final {
public:
    explicit WorktreeUsageJournal(QString dataDirectory);
    ~WorktreeUsageJournal();
    bool open(qint64 now,const QByteArray &externalHistoryRevision={},QString *error=nullptr);
    bool closeClean(const QByteArray &externalHistoryRevision={},QString *error=nullptr);
    bool observeImported(const WorktreeUsageIdentity&,qint64 now,qint64 lastUseHint=-1,QString *error=nullptr);
    bool recordLiveUse(const WorktreeUsageIdentity&,qint64 now,QString *error=nullptr);
    bool recordNewInstance(const WorktreeUsageIdentity&,qint64 now,QString *error=nullptr);
    bool snooze(const QList<WorktreeUsageIdentity>&,qint64 now,QString *error=nullptr);
    // A failed/partial scan must never prune dismissal records.
    bool pruneSnoozes(const QList<WorktreeUsageIdentity> &activeCandidates,bool scanComplete,qint64 now,QString *error=nullptr);
    WorktreeUsageRecord record(const WorktreeUsageIdentity&) const;
    QList<WorktreeUsageRecord> records() const;
    // Cached UI health never asserts complete cleanup tracking. Eligibility
    // requires a fresh revision check through validatedHealth, once per scan
    // and again immediately before each proposed removal.
    WorktreeUsageHealth health() const;
    WorktreeUsageHealth validatedHealth(bool trackingScopeComplete=false,QString *error=nullptr);
    QString lastError() const;
    QString path() const;
    QByteArray revision() const;
private:
    struct Private;std::unique_ptr<Private> d;
    bool update(const WorktreeUsageIdentity&,qint64 now,WorktreeUsageEvidence,qint64 hint,QString *error);
};
}
