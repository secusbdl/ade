#include "MainWindow.h"
#include "ChevronToolButton.h"
#include "SidebarNavigation.h"
#include "Icons.h"
#include "SettingsPage.h"
#include "core/AgentEngine.h"
#include "core/ToolExecutor.h"
#include "runtime/AgentToolHost.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalWidget.h"
#include "git/GitPanel.h"
#include "git/WorktreeSetupRunner.h"
#include "git/WorktreeSettingsController.h"
#include "git/WorktreeSettingsPane.h"
#include <QApplication>
#include <QDir>
#include <QFileSystemModel>
#include <QLabel>
#include <QTabWidget>
#include <QTreeView>
#include <QToolButton>
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
    for(const auto& root:m_gitMutatingPaths)if(containsPath(root,path)||containsPath(path,root))return true;
    for(const auto& root:m_worktreesRemoving)if(containsPath(root,path)||containsPath(path,root))return true;
    for(const auto& root:m_worktreeSetups)if(containsPath(root,path)||containsPath(path,root))return true;
    return false;
}
void MainWindow::initializeGit(){
    GitServiceOptions options;options.managedWorktreeRoot=QDir(m_store.directory()).filePath("worktrees");
    m_git=new GitPanel(options,this);m_git->hide();m_worktreeSetup=new WorktreeSetupRunner(this);
    m_worktreeSettings=new WorktreeSettingsController(m_git->service(),this);
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
    connect(m_git,&GitPanel::mutationFinished,this,mutationFinished);
    connect(m_worktreeSettings,&WorktreeSettingsController::mutationFinished,this,mutationFinished);
    connect(m_git,&GitPanel::worktreeRemovalStarted,this,removalStarted);
    connect(m_worktreeSettings,&WorktreeSettingsController::worktreeRemovalStarted,this,removalStarted);
    connect(m_git,&GitPanel::worktreeRemovalFinished,this,removalFinished);
    connect(m_worktreeSettings,&WorktreeSettingsController::worktreeRemovalFinished,this,removalFinished);
    connect(m_git,&GitPanel::mountWorktreeRequested,this,[this](const QString& path,const WorktreeSetupMetadata& setup){
        connectWorkspace(path);if(!current()||GitService::pathKey(current()->workspace)!=GitService::pathKey(path))return;
        if(!setup.workspacePath.isEmpty()){m_worktreeSetups.insert(GitService::pathKey(path));m_worktreeSetup->startAfterMount(setup);}
    });
    connect(m_git,&GitPanel::openDiffRequested,this,[this](const QString& workspace,const QString& file,bool staged,const QString&){
        if(staged)return;
        ToolExecutor scope;QString error;if(!scope.setWorkspace(workspace))return;
        const auto path=scope.resolvePath(file,&error);if(!error.isEmpty()){m_status->setText(error);return;}openFile(path);
    });
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
void MainWindow::updateGitContext(){
    if(!m_git)return;
    // Chats retain only their current workspace. Cleanup needs a separate,
    // persistent last-use journal before it can safely classify old worktrees.
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
bool MainWindow::prepareGitMutation(const QString& path,GitOperation operation){
    if(gitPathBusy(path)){m_status->setText("Wait for this workspace operation to finish.");return false;}
    for(auto* engine:m_engines)if(engine->isBusy()&&(containsPath(path,engine->workspace())||containsPath(engine->workspace(),path))){m_status->setText("Stop agents using this workspace before changing its Git state.");return false;}
    if(operation==GitOperation::CreateWorktree)return true;
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
