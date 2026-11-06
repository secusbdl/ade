#pragma once
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <atomic>
#include <memory>
namespace ade {
// Supplied by a freshly authorized Git/admin-directory inspection. The token
// identifies an instance, not a branch name or just a reusable pathname.
struct WorktreeUsageIdentity {
    QString commonGitDir,path,instanceToken;
    bool verified=false;
    QString key() const;
    bool valid() const;
};
enum class WorktreeUsageEvidence { UnknownImportedHistory,VerifiedLiveUse,VerifiedNewInstance };
struct WorktreeUsageRecord {
    WorktreeUsageIdentity identity;
    WorktreeUsageEvidence evidence=WorktreeUsageEvidence::UnknownImportedHistory;
    qint64 firstObservedAt=-1,lastUsedAt=-1,createdAt=-1,dismissedAt=-1;
};
class WorktreeMountRegistry final {
public:
    bool replace(const QString &owner,const QList<WorktreeUsageIdentity> &mounts,QString *error=nullptr);
    // Reserve an asynchronous identity refresh. The new canonical paths are
    // protected immediately; old mounted identities stay protected until a
    // matching completion replaces them. Old/released tickets cannot remount.
    quint64 beginUpdate(const QString &owner,const QStringList &canonicalPaths,QString *error=nullptr);
    bool completeUpdate(const QString &owner,quint64 ticket,const QList<WorktreeUsageIdentity> &mounts,QString *error=nullptr);
    void release(const QString &owner);
    bool isMounted(const WorktreeUsageIdentity &identity) const;
    int ownerCount(const WorktreeUsageIdentity &identity) const;
    int pendingOwnerCount() const;
private:
    struct Owner {QList<WorktreeUsageIdentity> mounts;QStringList pendingPaths;quint64 ticket=0;bool pending=false;};
    QHash<QString,Owner> m_owners;
    quint64 m_nextTicket=0;
};
inline constexpr qint64 WorktreeUnusedMs=7LL*24*60*60*1000;
struct WorktreeCleanupFacts {
    WorktreeUsageIdentity identity;
    bool authorized=false,managed=false,registered=false,main=false,detached=false;
    bool statusKnown=false,dirty=false,mounted=false,busy=false,merged=false;
    QString branch,mergedInto,defaultBranch,headSha;
};
struct WorktreeUsageHealth {
    bool storageHealthy=false,trackingScopeComplete=false;
    qint64 recoveryProtectionSince=-1;
};
struct WorktreeCleanupDecision { bool eligible=false;QString reason; qint64 lastUsedAt=-1; };
// Explicitly reviewed, revocable removal authority; never a bare age flag.
struct WorktreeCleanupPermit {
    WorktreeUsageRecord record;
    WorktreeUsageHealth health;
    QString journalPath;
    QByteArray journalRevision;
    std::shared_ptr<std::atomic_bool> valid;
    QString branch,headSha;
};
WorktreeCleanupDecision evaluateWorktreeCleanup(const WorktreeCleanupFacts &facts,
    const WorktreeUsageRecord *usage,const WorktreeUsageHealth &health,qint64 now);
}
