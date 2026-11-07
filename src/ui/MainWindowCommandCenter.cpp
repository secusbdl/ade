#include "MainWindow.h"
#include "CommandCenter.h"
#include "CommandFileSearch.h"
#include "ComposerWorkspaceSearch.h"
#include "SettingsPage.h"
#include "SidebarNavigation.h"
#include "WorkbenchSplitter.h"
#include "browser/BrowserWidget.h"
#include "core/AgentEngine.h"
#include "editor/EditorWidget.h"
#include "editor/CodeEditor.h"
#include "git/GitService.h"
#include "terminal/TerminalWidget.h"
#include "runtime/AgentToolHost.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include "WorkspaceFileTreeModel.h"
#include <QFutureWatcher>
#include <QLabel>
#include <QShortcut>
#include <QTabWidget>
#include <QTextBlock>
#include <QTreeView>
#include <QtConcurrent>
#include <optional>

namespace ade {
namespace {
struct SearchJob{quint64 id=0;QStringList roots;QString query;bool content=false,ignored=false;QJsonArray base;};
struct SearchReply{quint64 id=0;QJsonArray results;QString status;};
SearchReply searchFiles(const SearchJob& job,const std::shared_ptr<std::atomic_bool>& cancel){
    SearchReply reply;reply.id=job.id;reply.results=job.base;bool limited=false;QStringList errors;QElapsedTimer time;time.start();
    for(const auto& root:job.roots){if(cancel->load())return {};if(time.elapsed()>5000){limited=true;break;}const auto found=CommandFileSearch::search({root,job.query,job.content,job.ignored},cancel);for(const auto& entry:found.items){if(reply.results.size()>=256){limited=true;break;}reply.results.append(entry);}limited=limited||found.limited;if(!found.error.isEmpty())errors.append(found.error);}
    if(limited)errors.prepend("Search limits reached; narrow the query or workspace.");
    errors.removeDuplicates();reply.status=errors.join(' ');return reply;
}
QJsonArray settingsItems(){
    const QList<QStringList> definitions{
        {"models-providers","Models & Providers","Manage providers, models, API keys, and presets","api keys models providers custom"},
        {"agent-general","General Agent Settings","Configure agent behavior","general agent"},
        {"skills-context","Skills & Context files","Manage skills and AGENTS.md","skills context instructions"},
        {"worktree-setup","Worktrees","Configure repository setup scripts and worktrees","git worktree setup"},
        {"plugins","Plugins","Configure bundled and enabled plugins","plugins extensions tools"},
        {"personalization","Personalization","Configure UI size and theme","theme colors zoom"},
        {"browsing","Browsing Settings","Configure browser behavior and permissions","browser permissions search engine"},
        {"history","History","Open browsing history","history visited pages"},
        {"archived-agents","Archived chats","Search, restore, or delete archived chats","archive chats"},
        {"clear-data","Clear browsing data","Remove selected browser data","cookies cache clear data"},
        {"account","Account","Provider credentials and account options","account login"},
        {"about","About","Application version and licenses","about licenses version"}};
    QJsonArray result;for(const auto& item:definitions){QJsonArray keywords;for(const auto& keyword:item[3].split(' '))keywords.append(keyword);result.append(QJsonObject{{"id","setting:"+item[0]},{"kind","setting"},{"route",item[0]},{"title",item[1]},{"subtitle",item[2]},{"keywords",keywords}});}return result;
}
}
class CommandCenterController final:public QObject {
public:
    explicit CommandCenterController(QWidget* parent):QObject(parent),view(new CommandCenter(parent)){}
    ~CommandCenterController()override{if(cancel)cancel->store(true);}
    void enqueue(SearchJob job){if(cancel)cancel->store(true);pending=std::move(job);if(!running)launch();}
    void invalidate(){if(cancel)cancel->store(true);pending.reset();offered.clear();}
    void launch(){if(!pending)return;const auto job=*pending;pending.reset();running=true;cancel=std::make_shared<std::atomic_bool>(false);const auto flag=cancel;auto* watcher=new QFutureWatcher<SearchReply>(this);
        connect(watcher,&QFutureWatcher<SearchReply>::finished,this,[this,watcher,flag]{const auto reply=watcher->result();watcher->deleteLater();running=false;const QPointer<CommandCenterController> self(this);if(!flag->load()&&view&&view->requestId()==reply.id&&view->isVisible()){publish(reply.id,reply.results,reply.status);}if(self&&pending)launch();});
        watcher->setFuture(QtConcurrent::run([job,flag]{return searchFiles(job,flag);}));}
    void publish(quint64 request,const QJsonArray& items,const QString& status={}){if(!view||view->requestId()!=request)return;offered.clear();for(const auto& value:items){const auto item=value.toObject();offered.insert(item["id"].toString(),item);}view->setResults(request,items,status);}
    QPointer<CommandCenter> view;QHash<QString,QJsonObject> offered;QHash<QString,QPointer<QWidget>> pages;
private:
    bool running=false;std::optional<SearchJob> pending;std::shared_ptr<std::atomic_bool> cancel;
};
void MainWindow::initializeCommandCenter(){
    if(m_commandCenterController)return;
    m_commandCenterController=new CommandCenterController(this);auto* state=m_commandCenterController.data();
    connect(state->view,&CommandCenter::includeIgnoredChanged,this,[this](bool value){if(!m_settings->setPreference("general.fileSearchIncludeGitignored",value)&&m_commandCenterController&&m_commandCenterController->view)m_commandCenterController->view->setIncludeIgnored(m_settings->preference("general.fileSearchIncludeGitignored").toBool());});
    connect(state->view,&CommandCenter::queryRequested,this,[this](quint64 request,const QString& mode,const QString& query,const QString& workspace,bool content,bool ignored){
        auto* controller=m_commandCenterController.data();if(!controller||!controller->view)return;controller->invalidate();controller->pages.clear();QJsonArray base;
        if(mode=="global"||mode=="agents")for(const auto& session:m_sessions){if(session.archived||!session.sideChatParentId.isEmpty())continue;const auto* engine=m_engines.value(session.id);base.append(QJsonObject{{"id","agent:"+session.id},{"kind","agent"},{"agentId",session.id},{"title",session.title.isEmpty()?"New chat":session.title},{"subtitle",session.workspace},{"recency",double(session.lastMessageAt?session.lastMessageAt:session.updated)},{"busy",engine&&engine->isBusy()},{"pinned",m_sidebarNavigation&&m_sidebarNavigation->isPinned(session.id)},{"keywords",QJsonArray{session.workspace}}});}
        if(mode=="global"||mode=="browser"||mode=="terminals")for(int i=0;i<m_tabs->count();++i){auto* page=m_tabs->widget(i);const auto tab=page->property("browserTabId").toString();const bool terminal=qobject_cast<TerminalWidget*>(page);if((tab.isEmpty()&&!terminal)||(mode=="browser"&&tab.isEmpty())||(mode=="terminals"&&!terminal))continue;QString key=page->property("commandCenterId").toString();if(key.isEmpty()){key=QUuid::createUuid().toString(QUuid::WithoutBraces);page->setProperty("commandCenterId",key);}controller->pages.insert(key,page);auto* browser=page->findChild<BrowserWidget*>();const auto url=browser?browser->currentUrl().toString():QString();const auto subtitle=terminal?static_cast<TerminalWidget*>(page)->workingDirectory():url;base.append(QJsonObject{{"id","page:"+key},{"kind",terminal?"terminal":"tab"},{"pageId",key},{"title",m_tabs->tabText(i)},{"subtitle",subtitle},{"url",url},{"recency",double(page->property("commandLastFocused").toLongLong())}});}
        if(mode=="global"||mode=="terminals")for(auto* host:m_toolHosts){const auto* owner=find(host->agentId());if(!owner||owner->archived)continue;for(const auto& value:host->shells()->snapshot()["sessions"].toArray()){const auto shell=value.toObject();if(!shell["running"].toBool())continue;const auto shellId=shell["id"].toString();base.append(QJsonObject{{"id","shell:"+owner->id+":"+shellId},{"kind","shell"},{"agentId",owner->id},{"shellId",shellId},{"title","Shell · "+owner->title},{"subtitle",shell["cwd"]},{"recency",shell["createdAt"]},{"keywords",QJsonArray{shellId,"powershell"}}});}}
        if(mode=="global"||mode=="settings")for(const auto& item:settingsItems())base.append(item);
        base=CommandCenter::rank(base,query,mode=="global"?30:128);
        SearchJob job;job.id=request;job.query=query;job.content=content;job.ignored=ignored;job.base=base;
        if(mode=="files"||(mode=="global"&&!query.trimmed().isEmpty())){for(const auto& session:m_sessions){if(session.archived||session.workspace.isEmpty())continue;const auto root=GitService::canonicalPath(session.workspace);if(root.isEmpty()||(!workspace.isEmpty()&&root!=workspace)||job.roots.contains(root))continue;if(job.roots.size()>=16)break;job.roots.append(root);}}
        controller->publish(request,base,job.roots.isEmpty()?QString():QString("Searching files…"));if(!job.roots.isEmpty())controller->enqueue(std::move(job));
    });
    connect(state->view,&CommandCenter::actionRequested,this,[this](quint64 request,const QJsonObject& proposed,const QString& action){
        auto* controller=m_commandCenterController.data();if(!controller||!controller->view||request!=controller->view->requestId())return;
        const auto item=controller->offered.value(proposed["id"].toString());if(item.isEmpty()||item!=proposed)return;const auto kind=item["kind"].toString();const QPointer<MainWindow> self(this);
        if(kind=="agent"){const auto id=item["agentId"].toString();const auto* session=find(id);if(!session||session->archived)return;if(action=="rename")requestRenameConversation(id);else if(action=="delete")requestDeleteConversation(id);else if(action=="pin"){if(m_sidebarNavigation)m_sidebarNavigation->setPinned(id,!m_sidebarNavigation->isPinned(id));if(self&&m_commandCenterController&&m_commandCenterController->view)m_commandCenterController->view->refresh();}else selectConversation(id);return;}
        if(kind=="setting"){showSettings();if(self)m_settings->selectSection(item["route"].toString());return;}
        if(kind=="shell"){showAgentShell(item["agentId"].toString(),item["shellId"].toString());return;}
        if(kind=="tab"||kind=="terminal"){const QPointer<QWidget> page=controller->pages.value(item["pageId"].toString());if(!page||m_tabs->indexOf(page)<0)return;if(action=="copy"){auto* browser=page->findChild<BrowserWidget*>();if(browser)QApplication::clipboard()->setText(browser->currentUrl().toString());return;}if(action=="delete"){emit m_tabs->tabCloseRequested(m_tabs->indexOf(page));return;}m_tabs->setCurrentWidget(page);if(self&&page){revealContent();if(self&&page)page->setFocus();}return;}
        if(kind=="file"){
            const auto root=item["workspace"].toString();QString owner;for(const auto& session:m_sessions)if(!session.archived&&GitService::canonicalPath(session.workspace)==root){owner=session.id;if(owner==m_currentId)break;}if(owner.isEmpty()){conversationError("The file's workspace is no longer connected.");return;}
            QString error;const auto pin=ComposerWorkspaceSearch::pin(root,item["relativePath"].toString(),item["workspaceIdentity"].toString(),&error);if(!pin){conversationError(error);return;}
            if(pin.directory){if(owner!=m_currentId)selectConversation(owner);if(!self||m_currentId!=owner)return;m_splitter->setFileTreeVisible(true);if(!self||m_currentId!=owner)return;const auto generation=m_fileModel->generation();const auto index=m_fileModel->index(pin.absolutePath);if(!self||m_currentId!=owner||m_fileModel->generation()!=generation||!index.isValid())return;auto ancestor=index;while(ancestor.isValid()){m_files->expand(ancestor);if(!self||m_currentId!=owner||m_fileModel->generation()!=generation)return;ancestor=ancestor.parent();}m_files->setCurrentIndex(index);if(!self||m_currentId!=owner||m_fileModel->generation()!=generation)return;m_files->scrollTo(index);m_files->setFocus();}
            else {openFile(pin.absolutePath);if(!self)return;if(auto* editor=qobject_cast<EditorWidget*>(m_tabs->currentWidget());editor&&GitService::canonicalPath(editor->filePath())==GitService::canonicalPath(pin.absolutePath)){const int line=item["line"].toInt();if(line>0){auto cursor=editor->editor()->textCursor();const auto block=editor->editor()->document()->findBlockByNumber(line-1);if(block.isValid()){cursor.setPosition(block.position());editor->editor()->setTextCursor(cursor);editor->editor()->centerCursor();}}editor->editor()->setFocus();}}
        }
    });
    connect(m_tabs,&QTabWidget::currentChanged,state,[this](int index){if(auto* page=m_tabs->widget(index))page->setProperty("commandLastFocused",QDateTime::currentMSecsSinceEpoch());});
    new QShortcut(QKeySequence("Ctrl+K"),this,[this]{openCommandCenter();});new QShortcut(QKeySequence("Ctrl+P"),this,[this]{openCommandCenter("files");});new QShortcut(QKeySequence("Ctrl+Shift+F"),this,[this]{openCommandCenter("files",true);});
}
void MainWindow::openCommandCenter(const QString& mode,bool searchContents){
    initializeCommandCenter();auto* controller=m_commandCenterController.data();if(!controller||!controller->view)return;if(controller->view->isVisible()&&mode=="global"){controller->view->reject();return;}
    QJsonArray workspaces;QSet<QString> seen;for(const auto& session:m_sessions){if(session.archived||session.workspace.isEmpty())continue;const auto root=GitService::canonicalPath(session.workspace);if(root.isEmpty()||seen.contains(root))continue;seen.insert(root);workspaces.append(QJsonObject{{"id",root},{"title",QFileInfo(root).fileName()+" — "+root}});}
    controller->view->setWorkspaces(workspaces);controller->view->setIncludeIgnored(m_settings->preference("general.fileSearchIncludeGitignored").toBool());controller->view->present(mode,searchContents);
}
}
