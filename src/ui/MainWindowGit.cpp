#include "MainWindow.h"
#include "ChevronToolButton.h"
#include "SidebarNavigation.h"
#include "Icons.h"
#include "GitDiffTab.h"
#include "SettingsPage.h"
#include "SideChatPane.h"
#include "core/AgentEngine.h"
#include "core/ToolExecutor.h"
#include "runtime/AgentToolHost.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalWidget.h"
#include "git/GitPanel.h"
#include "git/WorktreeSetupRunner.h"
#include "git/WorktreeSettingsController.h"
#include "git/WorktreeSettingsPane.h"
#include "git/WorktreeCleanupController.h"
#include <QApplication>
#include <QDir>
#include <QFileSystemModel>
#include <QLabel>
#include <QTabWidget>
#include <QTreeView>
#include <QToolButton>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace ade {
namespace {
bool containsPath(const QString& root,const QString& path){
    if(root.isEmpty()||path.isEmpty())return false;
    const auto a=GitService::pathKey(root),b=GitService::pathKey(path);
    return a==b||b.startsWith(a+'/');
}
}
bool MainWindow::gitPathBusy(const QString& path) const {
    if(path.isEmpty())return false;
    for(auto* host:m_toolHosts)if(host&&host->mutationBusy())
        for(const auto& chat:m_sessions)if(chat.id==host->agentId()&&(containsPath(chat.workspace,path)||containsPath(path,chat.workspace)))return true;
    for(const auto& root:m_gitMutatingPaths)if(containsPath(root,path)||containsPath(path,root))return true;
    for(const auto& root:m_worktreesRemoving)if(containsPath(root,path)||containsPath(path,root))return true;
    for(const auto& root:m_worktreeSetups)if(containsPath(root,path)||containsPath(path,root))return true;
    return false;
}
void MainWindow::initializeGit(){
    GitServiceOptions options;options.managedWorktreeRoot=QDir(m_store.directory()).filePath("worktrees");
    m_git=new GitPanel(options,this);m_git->hide();m_worktreeSetup=new WorktreeSetupRunner(this);
    m_worktreeSettings=new WorktreeSettingsController(m_git->service(),this);
    m_worktreeCleanup=new WorktreeCleanupController(m_git->service(),m_store.directory(),m_store.durableRevision(),this);
    m_worktreeCleanup->setObjectName("worktreeCleanupController");
    m_worktreeCleanup->setMutationGuard([this](const QString& path,GitOperation operation){return prepareGitMutation(path,operation);});
    connect(m_worktreeCleanup,&WorktreeCleanupController::errorOccurred,this,[this](const QString& error){m_status->setText(error);});
    connect(m_git,&GitPanel::cleanupReviewRequested,this,&MainWindow::reviewUnusedWorktrees);
    m_git->setMutationGuard([this](const QString& path,GitOperation operation){return prepareGitMutation(path,operation);});
    m_worktreeSettings->setMutationGuard([this](const QString& path,GitOperation operation){return prepareGitMutation(path,operation);});
    const auto mutationStarted=[this](const QString& path,GitOperation){m_gitMutatingPaths.insert(GitService::pathKey(path));};
    const auto mutationFinished=[this](const QString& path,GitOperation,bool){m_gitMutatingPaths.remove(GitService::pathKey(path));updateGitContext();};
    const auto removalStarted=[this](const QString& path){
        const auto key=GitService::pathKey(path);m_worktreesRemoving.insert(key);QStringList affected;
        for(const auto& session:m_sessions)if(containsPath(path,session.workspace))affected.append(session.id);
        m_removalAffectedSessions.insert(key,affected);
    };
    const auto removalFinished=[this](const QString& path,bool){const auto key=GitService::pathKey(path);m_worktreesRemoving.remove(key);m_removalAffectedSessions.remove(key);updateGitContext();};
    connect(m_git,&GitPanel::mutationStarted,this,mutationStarted);
    connect(m_worktreeSettings,&WorktreeSettingsController::mutationStarted,this,mutationStarted);
    connect(m_worktreeCleanup,&WorktreeCleanupController::mutationStarted,this,mutationStarted);
    connect(m_git,&GitPanel::mutationFinished,this,mutationFinished);
    connect(m_worktreeSettings,&WorktreeSettingsController::mutationFinished,this,mutationFinished);
    connect(m_worktreeCleanup,&WorktreeCleanupController::mutationFinished,this,mutationFinished);
    connect(m_git,&GitPanel::worktreeRemovalStarted,this,removalStarted);
    connect(m_worktreeSettings,&WorktreeSettingsController::worktreeRemovalStarted,this,removalStarted);
    connect(m_worktreeCleanup,&WorktreeCleanupController::worktreeRemovalStarted,this,removalStarted);
    connect(m_git,&GitPanel::worktreeRemovalFinished,this,removalFinished);
    connect(m_worktreeSettings,&WorktreeSettingsController::worktreeRemovalFinished,this,removalFinished);
    connect(m_worktreeCleanup,&WorktreeCleanupController::worktreeRemovalFinished,this,removalFinished);
    connect(m_git,&GitPanel::mountWorktreeRequested,this,[this](const QString& path,const WorktreeSetupMetadata& setup){
        connectWorkspace(path);if(!current()||GitService::pathKey(current()->workspace)!=GitService::pathKey(path))return;
        if(!setup.workspacePath.isEmpty()){m_worktreeSetups.insert(GitService::pathKey(path));m_worktreeSetup->startAfterMount(setup);}
    });
    connect(m_git,&GitPanel::openDiffRequested,this,&MainWindow::openWorkspaceDiff);
    connect(m_git,&GitPanel::worktreeRemoved,this,[this](const QString& path){
        m_worktreeSetup->cancel(path);
        const auto affected=m_removalAffectedSessions.value(GitService::pathKey(path));
        for(auto& session:m_sessions)if(affected.contains(session.id)||containsPath(path,session.workspace)){
            session.workspace.clear();if(auto* engine=m_engines.value(session.id))engine->setWorkspace({});
            if(auto* host=m_toolHosts.value(session.id))host->setWorkspace({});
        }
        saveSessions();
        if(current()&&current()->workspace.isEmpty()){m_workspace->setText("Connect workspace  +");static_cast<ChevronToolButton*>(m_workspace)->setChevronVisible(false);m_workspace->setToolTip({});m_files->setModel(nullptr);}
        const QPointer<MainWindow> self(this);updateGitContext();if(self)refreshWorkbench();
    });
    connect(m_git,&GitPanel::repositoryChanged,this,[this](const QString&){updateGitContext();});
    connect(m_worktreeSetup,&WorktreeSetupRunner::updated,this,[this](const WorktreeSetupRun& run){
        const auto key=GitService::pathKey(run.metadata.workspacePath);
        if(run.status==WorktreeSetupRun::Status::Running)m_worktreeSetups.insert(key);else m_worktreeSetups.remove(key);
        if(!current()||GitService::pathKey(current()->workspace)!=GitService::pathKey(run.metadata.workspacePath))return;
        if(run.status==WorktreeSetupRun::Status::Running)m_status->setText("Running worktree setup…");
        else if(run.status==WorktreeSetupRun::Status::Failed)m_status->setText("Worktree setup failed: "+run.message+"\n"+run.stderrTail.right(1500));
        else if(run.status==WorktreeSetupRun::Status::Succeeded)m_status->setText("Worktree setup completed.");
    });
}
void MainWindow::configureWorktreeSettingsPage(){
    m_settings->setWorktreePageFactory([this](QWidget* parent){
        updateGitContext();
        auto* pane=new WorktreeSettingsPane(m_worktreeSettings,parent);
        pane->setDarkTheme(qApp->palette().color(QPalette::Base).lightness()<128);
        return pane;
    });
}
void MainWindow::showGit(){
    updateGitContext();int index=m_tabs->indexOf(m_git);if(index<0)index=m_tabs->addTab(m_git,icon("branch"),"Source control");m_tabs->setCurrentIndex(index);revealContent();m_git->refresh();
}
void MainWindow::openWorkspaceDiff(const QString& workspace,const QString& path,bool staged,const QString& oldPath){
    if(!m_git||workspace.isEmpty()||path.isEmpty())return;
    const auto canonical=GitService::canonicalPath(workspace);if(canonical.isEmpty())return;
    bool authorized=false;
    for(const auto& session:m_sessions)if(!session.archived&&!session.workspace.isEmpty()
        &&GitService::pathKey(GitService::canonicalPath(session.workspace))==GitService::pathKey(canonical)){authorized=true;break;}
    if(!authorized){m_status->setText("The comparison workspace is no longer connected.");return;}
    const auto key=GitDiffTab::tabKey(canonical,path,staged,oldPath);
    const QPointer<MainWindow> self(this);
    for(int i=0;i<m_tabs->count();++i)if(auto* tab=qobject_cast<GitDiffTab*>(m_tabs->widget(i));tab&&tab->tabKey()==key){
        const QPointer<GitDiffTab> guarded(tab);m_tabs->setCurrentIndex(i);if(!self||!guarded)return;
        revealContent();if(!self||!guarded)return;guarded->refresh();return;
    }
    auto* tab=new GitDiffTab(m_git->service());const QPointer<GitDiffTab> guarded(tab);
    tab->load(canonical,path,staged,oldPath);if(!self||!guarded)return;
    const int index=m_tabs->addTab(tab,icon("document"),QFileInfo(path).fileName()+(staged?" · Staged":" · Diff"));
    if(!self||!guarded||m_tabs->indexOf(tab)<0)return;
    m_tabs->setTabToolTip(index,(oldPath.isEmpty()?path:oldPath+" → "+path)+"\n"+canonical);
    m_tabs->setCurrentWidget(tab);if(!self||!guarded||m_tabs->indexOf(tab)<0)return;revealContent();
}
void MainWindow::openWorkspaceChangedFile(const QString& workspace,const QString& relativePath,std::function<bool()> stillValid,const QString& ownerChat){
    if(!m_git||workspace.isEmpty()||relativePath.isEmpty())return;
    const auto canonical=GitService::canonicalPath(workspace);
    const auto owner=ownerChat.isEmpty()?m_currentId:ownerChat;const auto primary=m_currentId;
    if(canonical.isEmpty()||!find(owner)||find(owner)->archived||(owner!=primary&&!m_sideChats.value(owner))||GitService::canonicalPath(find(owner)->workspace).compare(canonical,Qt::CaseInsensitive)!=0)return;
    const QPointer<MainWindow> self(this);const auto epoch=++m_changedFileOpenEpoch;
    const auto valid=[self,owner,primary,epoch,canonical,stillValid]{
        const auto validOwner=[&]{return self&&self->m_currentId==primary&&self->m_changedFileOpenEpoch==epoch&&self->find(owner)&&!self->find(owner)->archived&&(owner==primary||self->m_sideChats.value(owner))&&GitService::canonicalPath(self->find(owner)->workspace).compare(canonical,Qt::CaseInsensitive)==0;};
        if(!validOwner())return false;
        if(stillValid&&!stillValid())return false;
        return validOwner();
    };
    if(!valid())return;
    showWorkspaceChanges(canonical,owner);if(!valid())return;
    const QPointer<GitService> service=m_git->service();
    const auto request=service->diff(canonical);
    auto connection=std::make_shared<QMetaObject::Connection>();
    *connection=connect(service,&GitService::completed,this,[this,self,service,connection,valid,request,canonical,relativePath](quint64 id,const GitResult& result){
        if(id!=request)return;
        QObject::disconnect(*connection);
        if(!valid()||!service)return;
        if(!result.ok&&result.error.code!=GitErrorCode::NotRepository){m_status->setText(result.error.message);return;}
        for(const auto& entry:result.diff.entries)if(entry.path==relativePath||entry.oldPath==relativePath){
            if(entry.status!='D')openWorkspaceDiff(canonical,entry.path,entry.staged,entry.oldPath);
            return;
        }
        ToolExecutor scope;QString error;if(!scope.setWorkspace(canonical))return;
        const auto path=scope.resolvePath(relativePath,&error);
        if(!error.isEmpty()){m_status->setText(error);return;}
        if(valid())openFile(path);
    });
}
void MainWindow::updateGitContext(){
    if(!m_git)return;
    updateGitUsageTracking();
    GitWorkspaceContext context;context.usageKnown=false;
    for(const auto& session:m_sessions)if(!session.workspace.isEmpty()){
        context.trustedRoots.append(session.workspace);
        if(!session.archived&&m_liveConversationOwners.contains(session.id))context.mountedPaths.append(session.workspace);
        const auto key=GitService::pathKey(session.workspace);context.lastUsedAt[key]=qMax(context.lastUsedAt.value(key),session.updated);
    }
    context.trustedRoots.removeDuplicates();context.mountedPaths.removeDuplicates();m_git->setWorkspaceContext(context);
    refreshAgents();if(m_sidebarNavigation)m_sidebarNavigation->workspaceContextChanged();
    QSet<QString> watchOwners;
    for(const auto& session:m_sessions)if(!session.archived&&m_liveConversationOwners.contains(session.id)&&!session.workspace.isEmpty()){
        const auto owner=QStringLiteral("chat/")+session.id;watchOwners.insert(owner);
        m_git->service()->watchWorkspace(owner,session.workspace);
    }
    for(const auto& owner:m_repositoryWatchOwners)if(!watchOwners.contains(owner))m_git->service()->unwatchWorkspace(owner);
    m_repositoryWatchOwners=watchOwners;
    m_git->setWorkspace(current()&&!current()->archived?current()->workspace:QString(),m_currentId);
    if(m_worktreeSettings&&!m_worktreeSettings->isMutating()&&m_settings
        &&(m_settings->currentSection()=="worktree-setup"||m_worktreeSettings->hasUnsavedDrafts()))m_worktreeSettings->refresh();
}
void MainWindow::updateGitUsageTracking(){
    if(!m_git||!m_worktreeCleanup)return;
    GitWorkspaceContext context;QMap<QString,WorktreeLiveOwner> owners;
    for(const auto& session:m_sessions)if(!session.workspace.isEmpty()){
        context.trustedRoots.append(session.workspace);
        if(!session.archived&&m_liveConversationOwners.contains(session.id)){
            context.mountedPaths.append(session.workspace);owners.insert(session.id,{session.workspace,session.lastMessageAt});
        }
        context.lastUsedAt[GitService::pathKey(session.workspace)]=qMax(context.lastUsedAt.value(GitService::pathKey(session.workspace)),session.lastMessageAt);
    }
    context.trustedRoots.removeDuplicates();context.mountedPaths.removeDuplicates();m_git->setWorkspaceContext(context);
    m_worktreeCleanup->updateOwners(owners);
}
void MainWindow::closeGitUsageTracking(){
    if(!m_worktreeCleanup)return;
    QString error;if(!m_worktreeCleanup->closeClean(m_store.durableRevision(),&error)&&m_status)m_status->setText(error);
}
void MainWindow::reviewUnusedWorktrees(){
    if(!m_worktreeCleanup)return;
    auto* dialog=new QDialog(this);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("worktreeCleanupDialog");
    dialog->setWindowTitle("Review unused worktrees");dialog->resize(820,440);
    auto* layout=new QVBoxLayout(dialog);auto* description=new QLabel("Only clean, unmounted, merged worktrees with verified activity older than seven days are eligible. Branches are retained. Nothing is removed automatically.");description->setWordWrap(true);layout->addWidget(description);
    auto* status=new QLabel;status->setObjectName("worktreeCleanupStatus");status->setWordWrap(true);layout->addWidget(status);
    auto* rows=new QTreeWidget;rows->setObjectName("worktreeCleanupRows");rows->setHeaderLabels({"Worktree / branch","Last used","Status"});rows->setRootIsDecorated(false);rows->header()->setSectionResizeMode(0,QHeaderView::Stretch);layout->addWidget(rows,1);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close);auto* refresh=buttons->addButton("Refresh",QDialogButtonBox::ActionRole);auto* snooze=buttons->addButton("Snooze selected 7 days",QDialogButtonBox::ActionRole);auto* remove=buttons->addButton("Remove selected…",QDialogButtonBox::ActionRole);
    refresh->setObjectName("worktreeCleanupRefresh");snooze->setObjectName("worktreeCleanupSnooze");remove->setObjectName("worktreeCleanupRemove");layout->addWidget(buttons);
    const QPointer<WorktreeCleanupController> controller=m_worktreeCleanup;
    auto selected=[rows]{QStringList keys;for(int i=0;i<rows->topLevelItemCount();++i){auto* row=rows->topLevelItem(i);if(row->checkState(0)==Qt::Checked)keys.append(row->data(0,Qt::UserRole).toString());}return keys;};
    auto render=[controller,rows,status,refresh,snooze,remove]{
        rows->clear();if(!controller)return;
        const bool busy=controller->busy();status->setText(busy?"Checking worktrees…":controller->trackingError());
        for(const auto& row:controller->rows()){
            auto* item=new QTreeWidgetItem(rows);item->setText(0,row.facts.identity.path+"\n"+row.facts.branch);item->setData(0,Qt::UserRole,row.facts.identity.key());
            item->setText(1,row.decision.lastUsedAt>=0?QDateTime::fromMSecsSinceEpoch(row.decision.lastUsedAt).toString("yyyy-MM-dd HH:mm"):"Unknown");
            item->setText(2,row.decision.eligible?"Ready":QString(row.decision.reason).replace('-',' '));
            if(row.decision.eligible){item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(0,Qt::Unchecked);}
        }
        if(!busy&&controller->trackingReady()&&controller->rows().isEmpty())status->setText("No managed worktrees were found, or this review expired. Refresh to check again.");
        refresh->setEnabled(!busy&&controller->trackingReady());snooze->setEnabled(!busy&&controller->trackingReady());remove->setEnabled(!busy&&controller->trackingReady());
    };
    connect(controller,&WorktreeCleanupController::changed,dialog,render);
    connect(controller,&WorktreeCleanupController::errorOccurred,dialog,[status](const QString& message){status->setText(message);});
    connect(refresh,&QPushButton::clicked,dialog,[controller]{if(controller)controller->refresh();});
    connect(snooze,&QPushButton::clicked,dialog,[controller,selected,status]{QString error;if(controller&&!controller->snooze(selected(),&error))status->setText(error);});
    connect(remove,&QPushButton::clicked,dialog,[controller,selected,status,dialog]{
        const auto keys=selected();if(keys.isEmpty()){status->setText("Select at least one eligible worktree.");return;}
        const QPointer<QDialog> safe(dialog);
        if(QMessageBox::question(dialog,"Remove worktrees",QString("Remove %1 selected worktree directories? Git branches will be retained. Fresh safety checks run before every removal.").arg(keys.size()),QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
        if(safe&&controller)controller->removeReviewed(keys);
    });
    connect(buttons,&QDialogButtonBox::rejected,dialog,&QDialog::close);render();dialog->show();controller->refresh();
}
bool MainWindow::prepareGitMutation(const QString& path,GitOperation operation){
    if(gitPathBusy(path)){m_status->setText("Wait for this workspace operation to finish.");return false;}
    for(auto* engine:m_engines)if(engine->isBusy()&&(containsPath(path,engine->workspace())||containsPath(engine->workspace(),path))){m_status->setText("Stop agents using this workspace before changing its Git state.");return false;}
    if(operation==GitOperation::CreateWorktree)return true;
    for(auto* host:m_toolHosts)for(const auto& value:host->shells()->snapshot()["sessions"].toArray()){
        const auto shell=value.toObject();const auto cwd=shell["cwd"].toString();
        if(shell["running"].toBool()&&(containsPath(path,cwd)||containsPath(cwd,path))&&(operation==GitOperation::RemoveWorktree||shell["commandActive"].toBool())){m_status->setText("Stop the agent shell using this workspace before changing its Git state.");return false;}
    }
    if(operation==GitOperation::RemoveWorktree)for(auto* terminal:findChildren<TerminalWidget*>())
        if(terminal->isRunning()&&containsPath(path,terminal->workingDirectory())){m_status->setText("Close the terminal using this worktree before removing it.");return false;}
    QList<EditorWidget*> editors;
    for(auto* editor:findChildren<EditorWidget*>())if(containsPath(path,editor->filePath())){
        if(operation==GitOperation::SaveSetupScript){
            const auto editorPath=GitService::pathKey(editor->filePath());
            if(editorPath!=GitService::pathKey(QDir(path).filePath(".stagewise/worktree-setup.sh"))
                &&editorPath!=GitService::pathKey(QDir(path).filePath(".stagewise/worktree-setup.ps1")))continue;
        }
        if(!editor->requestClose())return false;
        editors.append(editor);
    }
    for(auto* editor:editors){const int index=m_tabs->indexOf(editor);if(index>=0)m_tabs->removeTab(index);editor->deleteLater();}
    return true;
}
}
