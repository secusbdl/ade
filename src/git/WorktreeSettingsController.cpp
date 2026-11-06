#include "WorktreeSettingsController.h"
#include <QFileInfo>
#include <QPointer>
#include <limits>
namespace ade {
namespace {
int index(GitSetupVariant v){return v==GitSetupVariant::PowerShell?1:0;}
const GitSetupScript &script(const GitSetupRepository &r,GitSetupVariant v){return v==GitSetupVariant::PowerShell?r.powershell:r.posix;}
WorktreeScriptDraft initial(const GitSetupRepository &r,GitSetupVariant v){
    const auto &s=script(r,v);const auto text=s.exists?s.content:WorktreeSettingsController::scriptTemplate(v);
    return {text,text,s.path,s.error,s.revision,false};
}
}
WorktreeSettingsController::WorktreeSettingsController(GitService *service,QObject *parent):QObject(parent),m_service(service){
    if(!service){m_message="Git settings service is unavailable.";return;}
    connect(service,&GitService::completed,this,&WorktreeSettingsController::complete);
    connect(service,&GitService::repositoryTopologyChanged,this,[this](const QString &common,quint64){
        if(m_refreshId){refresh();return;}
        for(const auto &id:m_order)if(GitService::pathKey(id)==GitService::pathKey(common)){refresh();return;}
    });
    connect(service,&QObject::destroyed,this,[this]{const QPointer<WorktreeSettingsController> self(this);m_service=nullptr;m_refreshId=0;m_message="Git settings service is unavailable.";finishMutation(false);if(self)emit changed();});
}
WorktreeSettingsController::~WorktreeSettingsController(){
    if(m_service){m_service->disconnect(this);if(m_refreshId)m_service->cancel(m_refreshId);if(m_mutationId)m_service->cancel(m_mutationId);}
}
QString WorktreeSettingsController::scriptTemplate(GitSetupVariant variant){
    return (variant==GitSetupVariant::PowerShell?QString("$ErrorActionPreference = 'Stop'\n\n"):QString("#!/bin/sh\nset -e\n\n"))+
        "# Runs after stagewise creates and mounts a new Git worktree.\n# CWD is the new worktree.\n\n# Example:\n# pnpm install\n";
}
void WorktreeSettingsController::setMutationGuard(std::function<bool(const QString &,GitOperation)> guard){m_guard=std::move(guard);emit changed();}
QStringList WorktreeSettingsController::repositoryIds() const{return m_order;}
GitSetupRepository WorktreeSettingsController::repository(const QString &id) const{return m_repositories.value(id).info;}
bool WorktreeSettingsController::repositoryAvailable(const QString &id) const{return m_repositories.value(id).available;}
WorktreeScriptDraft WorktreeSettingsController::draft(const QString &id,GitSetupVariant v) const{return m_repositories.value(id).drafts[index(v)];}
void WorktreeSettingsController::selectRepository(const QString &id){if(!m_repositories.contains(id)||m_selected==id)return;m_selected=id;emit changed();}
void WorktreeSettingsController::selectVariant(GitSetupVariant v){if(v!=GitSetupVariant::Posix&&v!=GitSetupVariant::PowerShell)return;if(m_variant==v)return;m_variant=v;emit changed();}
void WorktreeSettingsController::setDraft(const QString &id,GitSetupVariant v,const QString &text){
    auto found=m_repositories.find(id);if(found==m_repositories.end())return;auto &d=found->drafts[index(v)];if(d.text==text)return;d.text=text;emit changed();
}
void WorktreeSettingsController::resetDraft(const QString &id,GitSetupVariant v){auto found=m_repositories.find(id);if(found==m_repositories.end())return;found->drafts[index(v)]=initial(found->info,v);emit changed();}
bool WorktreeSettingsController::hasUnsavedDrafts() const{for(const auto &r:m_repositories)for(const auto &d:r.drafts)if(d.dirty())return true;return false;}
void WorktreeSettingsController::discardDrafts(){for(auto &r:m_repositories){r.drafts[0]=initial(r.info,GitSetupVariant::Posix);r.drafts[1]=initial(r.info,GitSetupVariant::PowerShell);}emit changed();}
bool WorktreeSettingsController::canSave(const QString &id,GitSetupVariant v) const{
    const auto found=m_repositories.constFind(id);if(found==m_repositories.cend())return false;const auto &d=found->drafts[index(v)];
    return m_service&&m_guard&&!isMutating()&&found->available&&!found->info.trustedWorkspaceAnchor.isEmpty()&&d.dirty()&&d.error.isEmpty()&&!d.conflict;
}
bool WorktreeSettingsController::canRemove(const QString &id,const QString &path) const{
    if(!m_service||!m_guard||isMutating()||!repositoryAvailable(id))return false;
    for(const auto &tree:repository(id).worktrees)if(GitService::pathKey(tree.worktree.path)==GitService::pathKey(path))return tree.removable;
    return false;
}
void WorktreeSettingsController::refresh(){
    if(!m_service)return;
    if(m_refreshId){m_refreshAgain=true;return;}
    m_refreshGeneration=m_generation;m_refreshId=m_service->setupRepositories();emit changed();
}
void WorktreeSettingsController::acceptRepositories(const QList<GitSetupRepository> &repositories){
    ++m_repositoryRevision;
    for(auto &r:m_repositories)r.available=false;
    m_order.clear();
    for(const auto &info:repositories){
        const bool known=m_repositories.contains(info.id);auto &r=m_repositories[info.id];
        for(const auto v:{GitSetupVariant::Posix,GitSetupVariant::PowerShell}){
            auto &d=r.drafts[index(v)];const auto fresh=initial(info,v);
            if(!known||!d.dirty())d=fresh;
            else{d.path=fresh.path;d.error=fresh.error;d.conflict=d.revision!=fresh.revision;}
        }
        r.info=info;r.available=true;m_order.append(info.id);
    }
    for(auto it=m_repositories.begin();it!=m_repositories.end();++it)if(!it->available&&(it->drafts[0].dirty()||it->drafts[1].dirty()))m_order.append(it.key());
    if(!m_order.contains(m_selected))m_selected=m_order.isEmpty()?QString():m_order.first();
}
bool WorktreeSettingsController::authorize(const QString &path,GitOperation operation){
    if(!m_service||!m_guard||isMutating())return false;
    const QPointer<WorktreeSettingsController> self(this);const auto guard=m_guard;m_authorizing=true;const bool approved=guard(path,operation);
    if(!self)return false;
    m_authorizing=false;
    if(!approved){m_message="Workspace operation was cancelled.";emit changed();return false;}return true;
}
quint64 WorktreeSettingsController::save(const QString &id,GitSetupVariant v){
    if(!canSave(id,v))return 0;
    const auto r=m_repositories.value(id);const auto d=r.drafts[index(v)];
    const QPointer<WorktreeSettingsController> self(this);if(!authorize(r.info.mainWorktreePath,GitOperation::SaveSetupScript)||!self)return 0;
    if(!canSave(id,v))return 0;
    m_mutation={id,r.info.mainWorktreePath,d.text,v,GitOperation::SaveSetupScript};
    // Reserve the controller before notifying a host whose callbacks may reenter.
    m_mutationId=std::numeric_limits<quint64>::max();emit mutationStarted(m_mutation.path,m_mutation.operation);if(!self||!m_service)return 0;
    m_mutationId=m_service->saveSetupScript(r.info.trustedWorkspaceAnchor,v,d.text,d.revision);const auto request=m_mutationId;m_message="Saving setup script…";emit changed();return request;
}
quint64 WorktreeSettingsController::remove(const QString &id,const QString &path){
    if(!canRemove(id,path))return 0;
    const auto canonical=GitService::canonicalPath(path);const QPointer<WorktreeSettingsController> self(this);
    if(!authorize(canonical,GitOperation::RemoveWorktree)||!self||!canRemove(id,path))return 0;
    m_mutation={id,canonical,{},GitSetupVariant::PowerShell,GitOperation::RemoveWorktree};m_mutationId=std::numeric_limits<quint64>::max();
    emit mutationStarted(canonical,GitOperation::RemoveWorktree);if(!self||!m_service)return 0;
    emit worktreeRemovalStarted(canonical);if(!self||!m_service)return 0;
    m_mutationId=m_service->removeWorktree(canonical,GitRemovePolicy::Settings);const auto request=m_mutationId;m_message="Deleting managed worktree…";emit changed();return request;
}
void WorktreeSettingsController::finishMutation(bool success){
    if(!m_mutationId)return;
    const auto completed=m_mutation;m_mutationId=0;const QPointer<WorktreeSettingsController> self(this);
    emit mutationFinished(completed.path,completed.operation,success);if(!self)return;
    if(completed.operation==GitOperation::RemoveWorktree)emit worktreeRemovalFinished(completed.path,success);
}
void WorktreeSettingsController::complete(quint64 id,const GitResult &result){
    const QPointer<WorktreeSettingsController> self(this);
    if(id==m_refreshId){
        m_refreshId=0;if(result.ok&&m_refreshGeneration==m_generation){acceptRepositories(result.setupRepositories);if(!result.warnings.isEmpty())m_message=result.warnings.join('\n');}
        else if(!result.ok)m_message=result.error.message;
        const bool again=m_refreshAgain||m_refreshGeneration!=m_generation;m_refreshAgain=false;emit changed();if(self&&again)refresh();return;
    }
    if(id!=m_mutationId)return;
    const auto completed=m_mutation;
    if(result.ok){
        ++m_generation;++m_repositoryRevision;m_message=completed.operation==GitOperation::SaveSetupScript?"Setup script saved.":"Worktree deleted. Its branch was retained.";
        if(completed.operation==GitOperation::SaveSetupScript){auto &r=m_repositories[completed.repository];auto &saved=completed.variant==GitSetupVariant::PowerShell?r.info.powershell:r.info.posix;saved=result.setupScript;
            auto &d=r.drafts[index(completed.variant)];const auto newerText=d.text;d=initial(r.info,completed.variant);d.text=newerText;}
    }else{m_message=result.error.message;if(result.error.code==GitErrorCode::Conflict)m_repositories[completed.repository].drafts[index(completed.variant)].conflict=true;}
    finishMutation(result.ok);if(!self)return;emit changed();if(self&&(result.ok||result.mayHaveMutated||result.error.code==GitErrorCode::Conflict))refresh();
}
}
