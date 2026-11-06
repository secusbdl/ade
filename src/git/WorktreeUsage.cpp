#include "WorktreeUsage.h"
#include <QCryptographicHash>
#include <QDir>
#include <QSet>
#include <limits>
namespace ade {
namespace {
QString pathKey(const QString &path){auto result=QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    result=result.toCaseFolded();
#endif
    return result;
}
bool validPath(const QString &path){return !path.isEmpty()&&path.size()<=32768&&!path.contains(QChar::Null)&&QDir::isAbsolutePath(path);}
bool match(const WorktreeUsageIdentity &a,const WorktreeUsageIdentity &b){return a.key()==b.key()||pathKey(a.path)==pathKey(b.path);}
}
QString WorktreeUsageIdentity::key()const{
    const auto repo=pathKey(commonGitDir).toUtf8(),token=instanceToken.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray::number(repo.size())+":"+repo+QByteArray::number(token.size())+":"+token,QCryptographicHash::Sha256).toHex());
}
bool WorktreeUsageIdentity::valid()const{return verified&&validPath(commonGitDir)&&validPath(path)&&!instanceToken.isEmpty()&&instanceToken.size()<=1024&&!instanceToken.contains(QChar::Null);}
bool WorktreeMountRegistry::replace(const QString &owner,const QList<WorktreeUsageIdentity> &mounts,QString *error){
    auto fail=[&](const QString &message){if(error)*error=message;return false;};
    if(owner.isEmpty()||owner.size()>4096||owner.contains(QChar::Null)||mounts.size()>128)return fail("Invalid mount owner or mount count.");
    if(!m_owners.contains(owner)&&m_owners.size()>=4096)return fail("Mount owner limit reached.");
    QSet<QString> keys,paths;
    for(const auto &mount:mounts){if(!mount.valid()||keys.contains(mount.key())||paths.contains(pathKey(mount.path)))return fail("Invalid or duplicate mounted worktree identity.");keys.insert(mount.key());paths.insert(pathKey(mount.path));}
    if(m_nextTicket==std::numeric_limits<quint64>::max())return fail("Mount update generation limit reached.");
    const auto ticket=++m_nextTicket;
    if(mounts.isEmpty())m_owners.remove(owner);else m_owners.insert(owner,{mounts,{},ticket,false});
    if(error)error->clear();
    return true;
}
quint64 WorktreeMountRegistry::beginUpdate(const QString &owner,const QStringList &paths,QString *error){
    auto fail=[&](const QString &message){if(error)*error=message;return quint64(0);};
    if(owner.isEmpty()||owner.size()>4096||owner.contains(QChar::Null)||paths.size()>128)return fail("Invalid mount owner or path count.");
    if(!m_owners.contains(owner)&&m_owners.size()>=4096)return fail("Mount owner limit reached.");
    if(m_nextTicket==std::numeric_limits<quint64>::max())return fail("Mount update generation limit reached.");
    QSet<QString> seen;for(const auto &path:paths){if(!validPath(path)||seen.contains(pathKey(path)))return fail("Invalid or duplicate pending mount path.");seen.insert(pathKey(path));}
    auto &state=m_owners[owner];state.pendingPaths=paths;state.pending=true;state.ticket=++m_nextTicket;
    if(error)error->clear();
    return state.ticket;
}
bool WorktreeMountRegistry::completeUpdate(const QString &owner,quint64 ticket,const QList<WorktreeUsageIdentity> &mounts,QString *error){
    auto fail=[&](const QString &message){if(error)*error=message;return false;};
    const auto found=m_owners.find(owner);if(found==m_owners.end()||!ticket||found->ticket!=ticket||!found->pending)return fail("Mount identity completion is stale.");
    QSet<QString> expected,actual,keys;for(const auto &path:found->pendingPaths)expected.insert(pathKey(path));
    if(mounts.size()!=expected.size())return fail("Mount identity completion does not match its requested paths.");
    for(const auto &mount:mounts){if(!mount.valid()||actual.contains(pathKey(mount.path))||keys.contains(mount.key()))return fail("Invalid or duplicate mount identity completion.");actual.insert(pathKey(mount.path));keys.insert(mount.key());}
    if(actual!=expected)return fail("Mount identity completion changed its requested paths.");
    found->mounts=mounts;found->pendingPaths.clear();found->pending=false;if(mounts.isEmpty())m_owners.erase(found);
    if(error)error->clear();
    return true;
}
void WorktreeMountRegistry::release(const QString &owner){m_owners.remove(owner);}
int WorktreeMountRegistry::ownerCount(const WorktreeUsageIdentity &identity)const{
    if(!identity.valid())return 0;
    int count=0;
    for(const auto &owner:m_owners){bool mounted=false;for(const auto &mount:owner.mounts)if(match(mount,identity)){mounted=true;break;}if(!mounted)for(const auto &path:owner.pendingPaths)if(pathKey(path)==pathKey(identity.path)){mounted=true;break;}if(mounted)++count;}
    return count;
}
bool WorktreeMountRegistry::isMounted(const WorktreeUsageIdentity &identity)const{return ownerCount(identity)>0;}
int WorktreeMountRegistry::pendingOwnerCount()const{int count=0;for(const auto &owner:m_owners)if(owner.pending)++count;return count;}
WorktreeCleanupDecision evaluateWorktreeCleanup(const WorktreeCleanupFacts &f,const WorktreeUsageRecord *usage,const WorktreeUsageHealth &health,qint64 now){
    WorktreeCleanupDecision result;auto deny=[&](const QString &reason){result.reason=reason;return result;};
    if(now<0)return deny("invalid-clock");
    if(!health.storageHealthy)return deny("usage-storage-unavailable");
    if(!health.trackingScopeComplete)return deny("usage-tracking-incomplete");
    if(!f.identity.valid())return deny("unverified-worktree-instance");
    if(!f.authorized||!f.managed||!f.registered)return deny("unmanaged-or-unregistered");
    if(f.main)return deny("main-worktree");
    if(f.mounted)return deny("mounted");
    if(f.busy)return deny("workspace-busy");
    if(f.detached||f.branch.isEmpty())return deny("detached-or-missing-branch");
    if(!f.statusKnown||f.dirty)return deny("dirty-or-unknown-status");
    if(!usage||!usage->identity.valid()||usage->identity.key()!=f.identity.key()||pathKey(usage->identity.path)!=pathKey(f.identity.path))return deny("usage-history-unknown");
    if(usage->evidence!=WorktreeUsageEvidence::VerifiedLiveUse&&usage->evidence!=WorktreeUsageEvidence::VerifiedNewInstance)return deny("usage-history-unknown");
    const qint64 use=usage->evidence==WorktreeUsageEvidence::VerifiedLiveUse?usage->lastUsedAt:qMax(usage->createdAt,usage->lastUsedAt);
    if(use<0)return deny("usage-history-unknown");
    result.lastUsedAt=usage->lastUsedAt;
    if(use>now||usage->firstObservedAt>now||usage->lastUsedAt>now||usage->createdAt>now||usage->dismissedAt>now||health.recoveryProtectionSince>now)return deny("future-clock-value");
    if(now-use<WorktreeUnusedMs)return deny("recent-use");
    if(health.recoveryProtectionSince>=0&&now-health.recoveryProtectionSince<WorktreeUnusedMs)return deny("recovery-protection");
    const QStringList targets{f.defaultBranch,"main","master","develop","dev"};
    if(!f.merged||f.mergedInto.isEmpty()||f.mergedInto==f.branch||!targets.contains(f.mergedInto))return deny("not-merged-into-cleanup-target");
    if(usage->dismissedAt>=0&&now-usage->dismissedAt<WorktreeUnusedMs)return deny("snoozed");
    result.eligible=true;return result;
}
}
