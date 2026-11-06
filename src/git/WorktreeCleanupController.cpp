#include "WorktreeCleanupController.h"
#include <QDateTime>
#include <QTimer>
namespace ade {
WorktreeCleanupController::WorktreeCleanupController(GitService *service,QString directory,
    const QByteArray &revision,QObject *parent):QObject(parent),m_service(service),m_journal(std::move(directory)) {
    m_lease=std::make_shared<std::atomic_bool>(true);
    QString error;m_journal.open(QDateTime::currentMSecsSinceEpoch(),revision,&error);
    if(service)connect(service,&GitService::completed,this,&WorktreeCleanupController::finished);
}
WorktreeCleanupController::~WorktreeCleanupController() {
    m_lease->store(false);
    if(m_service){for(auto id:m_pending.keys())m_service->cancel(id);if(m_scan)m_service->cancel(m_scan);if(m_remove)m_service->cancel(m_remove);}
}
void WorktreeCleanupController::invalidate(){m_lease->store(false);m_lease=std::make_shared<std::atomic_bool>(true);m_rows.clear();}
void WorktreeCleanupController::setMutationGuard(std::function<bool(const QString &,GitOperation)> guard){m_guard=std::move(guard);}
bool WorktreeCleanupController::trackingReady()const{return !m_closed&&m_service&&m_journal.health().storageHealthy&&m_pending.isEmpty()&&m_failedOwners.isEmpty();}
QString WorktreeCleanupController::trackingError()const{
    if(!m_journal.health().storageHealthy)return m_journal.lastError();
    if(!m_pending.isEmpty())return "Verifying mounted worktree identities…";
    if(!m_failedOwners.isEmpty())return "A mounted workspace identity could not be verified. Cleanup is disabled.";
    return {};
}
void WorktreeCleanupController::updateOwners(const QMap<QString,WorktreeLiveOwner> &owners){
    if(m_closed||!m_service)return;
    bool different=m_owners.size()!=owners.size();
    for(auto it=owners.begin();!different&&it!=owners.end();++it){const auto old=m_owners.constFind(it.key());different=old==m_owners.end()||GitService::pathKey(old->path)!=GitService::pathKey(it->path)||old->activity!=it->activity;}
    if(!different)return;
    invalidate();const auto now=QDateTime::currentMSecsSinceEpoch();QString error;
    // Ending a live mount counts as use; saved/archived history alone never mounts it.
    for(auto it=m_owners.begin();it!=m_owners.end();++it){const auto next=owners.constFind(it.key());
        if(next==owners.end()||GitService::pathKey(next->path)!=GitService::pathKey(it->path)){
            const auto identity=m_identities.take(it.key());if(identity.valid())m_journal.recordLiveUse(identity,now,&error);
            m_failedOwners.remove(it.key());
        }
    }
    const auto previous=m_owners;m_owners=owners;
    for(auto it=owners.begin();it!=owners.end();++it){const auto old=previous.constFind(it.key());
        if(old!=previous.end()&&GitService::pathKey(old->path)==GitService::pathKey(it->path)&&old->activity==it->activity)continue;
        for(auto p=m_pending.begin();p!=m_pending.end();){if(p->owner==it.key()){m_service->cancel(p.key());p=m_pending.erase(p);}else ++p;}
        const auto id=m_service->inspectUsage(it->path);m_pending.insert(id,{it.key(),it->path,it->activity});
    }
    // Released owners cannot leave an unresolved inspection blocking tracking.
    for(auto it=m_pending.begin();it!=m_pending.end();){if(!it->owner.isEmpty()&&!owners.contains(it->owner)){m_service->cancel(it.key());it=m_pending.erase(it);}else ++it;}
    const QPointer<WorktreeCleanupController> self(this);if(!error.isEmpty())emit errorOccurred(error);if(self)emit changed();
}
bool WorktreeCleanupController::closeClean(const QByteArray &revision,QString *error){
    if(m_closed)return true;
    if(busy()||!m_pending.isEmpty()||!m_failedOwners.isEmpty()){if(error)*error="Worktree tracking was still active or incomplete; recovery protection is retained.";return false;}
    invalidate();
    // Live background mounts can outlast the retention interval without a new
    // message. Their final use is shutdown, not their activation timestamp.
    const auto now=QDateTime::currentMSecsSinceEpoch();
    for(auto it=m_owners.begin();it!=m_owners.end();++it){const auto identity=m_identities.value(it.key());
        if(identity.valid()&&!m_journal.recordLiveUse(identity,now,error))return false;
    }
    m_closed=true;return m_journal.closeClean(revision,error);
}
void WorktreeCleanupController::refresh(){
    if(busy()||m_closed)return;
    if(!trackingReady()){emit errorOccurred(trackingError());return;}
    invalidate();m_scanLease=m_lease;m_scan=m_service->scanUsage();emit changed();
}
bool WorktreeCleanupController::snooze(const QStringList &keys,QString *error){
    if(busy()||!trackingReady()){if(error)*error="Wait for worktree tracking to finish.";return false;}
    QList<WorktreeUsageIdentity> identities;
    for(const auto &key:keys){bool found=false;for(const auto &row:m_rows)if(row.facts.identity.key()==key&&row.decision.eligible){identities.append(row.facts.identity);found=true;break;}if(!found){if(error)*error="The review changed. Refresh before snoozing.";return false;}}
    if(identities.isEmpty()){if(error)*error="Select at least one eligible worktree.";return false;}
    if(!m_journal.snooze(identities,QDateTime::currentMSecsSinceEpoch(),error))return false;
    invalidate();emit changed();return true;
}
void WorktreeCleanupController::removeReviewed(const QStringList &keys){
    if(busy()||!trackingReady()||keys.isEmpty()){emit errorOccurred("Wait for a complete worktree review and select eligible rows.");return;}
    QSet<QString> seen;for(const auto &key:keys){bool found=false;for(const auto &row:m_rows)if(row.facts.identity.key()==key&&row.decision.eligible){found=true;break;}if(!found||seen.contains(key)){emit errorOccurred("Review changed or contains duplicate selections. Refresh first.");return;}seen.insert(key);}
    m_queue=keys;removeNext();
}
void WorktreeCleanupController::removeNext(){
    if(m_queue.isEmpty()||m_closed||!m_service){emit changed();return;}
    const auto key=m_queue.takeFirst();WorktreeCleanupFacts facts;
    for(const auto &row:m_rows)if(row.facts.identity.key()==key&&row.decision.eligible){facts=row.facts;break;}
    QString error;auto health=m_journal.validatedHealth(trackingReady(),&error);const auto record=m_journal.record(facts.identity);
    if(!evaluateWorktreeCleanup(facts,&record,health,QDateTime::currentMSecsSinceEpoch()).eligible){m_queue.clear();emit errorOccurred(error.isEmpty()?"Worktree review expired. Refresh before removal.":error);return;}
    const auto lease=m_lease;const auto guard=m_guard;const QPointer<WorktreeCleanupController> self(this);
    m_removing=facts.identity.path;
    if(guard&&!guard(facts.identity.path,GitOperation::RemoveWorktree)){if(self){m_queue.clear();m_removing.clear();}return;}
    if(!self)return;
    if(lease!=m_lease||!lease->load()){m_queue.clear();m_removing.clear();emit errorOccurred("Workspace changed during removal review.");return;}
    m_removing=facts.identity.path;
    // Reserve the pending state before host callbacks; they may revoke/delete us.
    emit mutationStarted(m_removing,GitOperation::RemoveWorktree);if(!self)return;
    emit worktreeRemovalStarted(m_removing);if(!self)return;
    if(lease!=m_lease||!lease->load()||!m_service){m_queue.clear();const auto path=m_removing;m_removing.clear();emit worktreeRemovalFinished(path,false);if(self)emit mutationFinished(path,GitOperation::RemoveWorktree,false);return;}
    WorktreeCleanupPermit permit{record,health,m_journal.path(),m_journal.revision(),lease,facts.branch,facts.headSha};
    m_remove=m_service->removeReviewedWorktree(permit);emit changed();
}
void WorktreeCleanupController::finished(quint64 id,const GitResult &result){
    if(m_closed)return;
    if(result.ok&&result.operation==GitOperation::CreateWorktree&&m_service){
        invalidate();m_pending.insert(m_service->inspectUsage(result.path),{{},result.path,0});
    }
    const auto pending=m_pending.find(id);
    if(pending!=m_pending.end()){
        const auto request=pending.value();m_pending.erase(pending);
        if(request.owner.isEmpty()){
            QString error;if(result.ok&&result.usage.size()==1)m_journal.recordNewInstance(result.usage.first().identity,QDateTime::currentMSecsSinceEpoch(),&error);
            const QPointer<WorktreeCleanupController> self(this);if(!error.isEmpty())emit errorOccurred(error);if(self)emit changed();return;
        }
        const auto owner=m_owners.constFind(request.owner);
        if(owner==m_owners.end()||GitService::pathKey(owner->path)!=GitService::pathKey(request.path)||owner->activity!=request.activity)return;
        QString error;
        if(result.ok&&result.usage.size()==1&&result.usage.first().identity.valid()){
            const auto identity=result.usage.first().identity;m_identities[request.owner]=identity;m_failedOwners.remove(request.owner);
            if(!m_journal.recordLiveUse(identity,QDateTime::currentMSecsSinceEpoch(),&error))m_failedOwners.insert(request.owner);
        }else if(result.error.code!=GitErrorCode::NotRepository){m_failedOwners.insert(request.owner);error=result.error.message;}
        const QPointer<WorktreeCleanupController> self(this);if(!error.isEmpty())emit errorOccurred(error);if(self)emit changed();return;
    }
    if(id==m_scan){
        m_scan=0;if(m_scanLease!=m_lease||!m_scanLease->load()){emit changed();return;}
        QString error;
        if(!result.ok||!result.warnings.isEmpty()){emit errorOccurred(result.ok?result.warnings.join('\n'):result.error.message);return;}
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(const auto &facts:result.usage)if(!m_journal.record(facts.identity).identity.valid()&&!m_journal.observeImported(facts.identity,now,-1,&error))break;
        const auto health=m_journal.validatedHealth(trackingReady(),&error);m_rows.clear();
        for(const auto &facts:result.usage){const auto record=m_journal.record(facts.identity);m_rows.append({facts,evaluateWorktreeCleanup(facts,&record,health,now)});}
        const QPointer<WorktreeCleanupController> self(this);if(!error.isEmpty())emit errorOccurred(error);if(self)emit changed();return;
    }
    if(id==m_remove){
        const auto path=m_removing;m_remove=0;m_removing.clear();if(!result.ok)m_queue.clear();
        if(result.ok){for(auto it=m_rows.begin();it!=m_rows.end();){if(GitService::pathKey(it->facts.identity.path)==GitService::pathKey(path))it=m_rows.erase(it);else ++it;}}
        const QPointer<WorktreeCleanupController> self(this);emit worktreeRemovalFinished(path,result.ok);if(!self)return;
        emit mutationFinished(path,GitOperation::RemoveWorktree,result.ok);if(!self)return;
        if(!result.ok){emit errorOccurred(result.error.message);if(!self)return;}
        emit changed();if(self&&!m_queue.isEmpty())QTimer::singleShot(0,this,&WorktreeCleanupController::removeNext);
    }
}
}
