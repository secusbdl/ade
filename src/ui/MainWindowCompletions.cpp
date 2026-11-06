#include "MainWindow.h"
#include "Composer.h"
#include "SideChatPane.h"
#include "ComposerWorkspaceSearch.h"
#include "ContextPreferences.h"
#include "SettingsPage.h"
#include "attachments/AttachmentStore.h"
#include "browser/BrowserToolBridge.h"
#include "browser/BrowserWidget.h"
#include "core/AgentEngine.h"
#include "core/ContextLoader.h"
#include "git/GitService.h"
#include "runtime/AgentToolHost.h"
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLabel>
#include <QRegularExpression>
#include <QTabWidget>
#include <QtConcurrent>
#include <functional>
#include <optional>

namespace ade {
namespace {
struct CompletionQuery {QString chat,workspace,query;quint64 id=0;QChar trigger;ContextOptions options;QJsonArray base;};
struct CompletionResult {CompletionQuery query;QJsonArray items;QString error;};
QString slashLink(const ContextSkill &skill){auto label=skill.name;label.replace(']',' ');return "[/"+label+"](slash:"+QString::fromLatin1(QUrl::toPercentEncoding(skill.id))+')';}
CompletionResult runCompletionQuery(const CompletionQuery &query,const std::shared_ptr<std::atomic_bool> &canceled){
    CompletionResult result;result.query=query;result.items=query.base;
    if(query.trigger=='/'){
        ContextLoader loader;const auto snapshot=loader.load(query.options);const auto filter=query.query.toCaseFolded();
        for(const auto &skill:snapshot.skills){if(canceled->load())return {};if(!skill.userInvocable||!(skill.id+' '+skill.name+' '+skill.description).toCaseFolded().contains(filter))continue;
            result.items.append(QJsonObject{{"id",skill.id},{"kind","skill"},{"label",skill.name},{"description",skill.description.left(400)},{"group",skill.source},{"insertText",slashLink(skill)}});
            if(result.items.size()>=128)break;
        }
    }else if(!query.workspace.isEmpty()){
        auto search=ComposerWorkspaceSearch::search(query.workspace,query.query,canceled);result.error=search.error;
        for(int index=result.items.size()-1;index>=0;--index)if(result.items[index].toObject()["kind"]=="workspace"){
            if(search.rootIdentity.isEmpty())result.items.removeAt(index);
            else {auto item=result.items[index].toObject();item["workspaceIdentity"]=search.rootIdentity;result.items[index]=item;}
        }
        for(const auto &entry:search.items)result.items.append(entry);
    }
    return result;
}
}
class ComposerCompletionController final:public QObject {
public:
    explicit ComposerCompletionController(QObject *parent):QObject(parent){}
    ~ComposerCompletionController()override{if(canceled)canceled->store(true);}
    void cancel(){if(canceled)canceled->store(true);pending.reset();offered.clear();++epoch;}
    void enqueue(CompletionQuery query){if(canceled)canceled->store(true);pending=std::move(query);if(!running)launch();}
    void launch(){
        if(!pending)return;
        const auto query=*pending;pending.reset();running=true;canceled=std::make_shared<std::atomic_bool>(false);const auto cancellation=canceled;
        const auto generation=epoch;auto *watcher=new QFutureWatcher<CompletionResult>(this);
        connect(watcher,&QFutureWatcher<CompletionResult>::finished,this,[this,watcher,generation,cancellation]{
            auto result=watcher->result();watcher->deleteLater();running=false;
            const QPointer<ComposerCompletionController> self(this);
            if(!cancellation->load()&&generation==epoch&&ready)ready(result);
            if(self&&pending)launch();
        });
        watcher->setFuture(QtConcurrent::run([query,cancellation]{return runCompletionQuery(query,cancellation);}));
    }
    QString chat,workspace;quint64 request=0,epoch=0;
    QHash<QString,QJsonObject> offered;
    std::function<void(const CompletionResult&)> ready;
private:
    std::optional<CompletionQuery> pending;bool running=false;
    std::shared_ptr<std::atomic_bool> canceled;
};

void MainWindow::initializeCompletions(){
    if(m_completionController||!m_composer)return;
    initializeComposerCompletions(m_composer,{});
}
void MainWindow::initializeComposerCompletions(Composer* composer,const QString& fixedChat){
    if(!composer)return;
    auto* controller=new ComposerCompletionController(composer);
    const QPointer<ComposerCompletionController> stateGuard(controller);
    const QPointer<Composer> target(composer);
    if(fixedChat.isEmpty())m_completionController=controller;else m_sideCompletionControllers[fixedChat]=controller;
    auto sessionFor=[this,target,fixedChat]()->Conversation*{
        if(!target)return nullptr;
        if(fixedChat.isEmpty())return current();
        auto* session=find(fixedChat);const auto pane=m_sideChats.value(fixedChat);
        const auto* parent=session?find(session->sideChatParentId):nullptr;
        return session&&pane&&pane->composer()==target&&parent&&!parent->archived&&!session->sideChatParentId.isEmpty()?session:nullptr;
    };
    auto report=[this,fixedChat](const QString& error){if(fixedChat.isEmpty())m_status->setText(error);else if(const auto pane=m_sideChats.value(fixedChat))pane->setError(error);};
    controller->ready=[this,stateGuard,target,sessionFor](const CompletionResult &result){
        auto *state=stateGuard.data();const auto *session=sessionFor();
        if(!state||!session||session->archived||session->id!=result.query.chat||state->request!=result.query.id||
           GitService::canonicalPath(session->workspace)!=result.query.workspace)return;
        state->offered.clear();for(const auto &value:result.items){const auto item=value.toObject();state->offered.insert(item["id"].toString(),item);}
        target->setCompletionResults(result.query.id,result.items,result.error.isEmpty()?QString("No matches"):result.error);
    };
    connect(target,&Composer::completionRequested,this,[this,stateGuard,target,sessionFor](const QString &chat,quint64 request,QChar trigger,const QString &query){
        const auto *session=sessionFor();auto *state=stateGuard.data();if(!session||!state||session->archived||session->id!=chat)return;
        CompletionQuery input;input.chat=chat;input.id=request;input.trigger=trigger;input.query=query.left(200);input.workspace=GitService::canonicalPath(session->workspace);
        state->chat=chat;state->workspace=input.workspace;state->request=request;state->offered.clear();
        if(trigger=='/'){
            input.options=contextOptionsFor(m_settings->preferences(),session->workspace,QDir::homePath());QString error;
            const auto plugins=PluginCatalog::context(m_store.directory(),m_settings->preferences(),&error);
            if(!error.isEmpty()){target->setCompletionResults(request,{},error);return;}
            input.options.pluginsDirectory=plugins.pluginsDirectory;input.options.disabledPluginIds=plugins.disabledPluginIds;
        }else{
            const auto filter=query.toCaseFolded();
            if(!input.workspace.isEmpty()&&(filter.isEmpty()||(QString("workspace ")+QFileInfo(input.workspace).fileName()).toCaseFolded().contains(filter))){
                input.base.append(QJsonObject{{"id","workspace:ws1"},{"kind","workspace"},{"label",QFileInfo(input.workspace).fileName()},{"description",input.workspace},{"group","Workspaces"},{"insertText","[](path:ws1/)"},{"workspace",input.workspace}});
            }
            for(int index=0;index<m_tabs->count();++index){auto *page=m_tabs->widget(index);const auto tab=page->property("browserTabId").toString();auto *browser=page->findChild<BrowserWidget*>();if(tab.isEmpty()||!browser||!m_browserBridge->canAccessTab(tab,chat))continue;
                const auto title=m_tabs->tabText(index);const auto url=browser->currentUrl().toString();if(!(title+' '+url).toCaseFolded().contains(filter))continue;
                input.base.append(QJsonObject{{"id","tab:"+tab},{"kind","tab"},{"tabId",tab},{"label",title},{"description",url},{"group","Tabs"},{"insertText","[](tab:"+QString::fromLatin1(QUrl::toPercentEncoding(tab))+")"}});
            }
        }
        state->enqueue(std::move(input));
    });
    connect(target,&Composer::completionActivated,this,[this,stateGuard,target,sessionFor,report,fixedChat](const QString &chat,quint64 request,const QJsonObject &item){
        auto *state=stateGuard.data();auto *session=sessionFor();const auto itemId=item["id"].toString();
        if(!state||!session||session->archived||session->id!=chat||state->request!=request||state->offered.value(itemId)!=item||GitService::canonicalPath(session->workspace)!=state->workspace)return;
        const auto nextDraft=target->completionDraft(request,itemId);if(!nextDraft)return;
        auto refs=session->draftAttachments;const auto kind=item["kind"].toString();ComposerPathPin pin;QString error;
        if(kind=="file"||kind=="directory"){
            pin=ComposerWorkspaceSearch::pin(state->workspace,item["relativePath"].toString(),item["workspaceIdentity"].toString(),&error);
            if(!pin||pin.directory!=(kind=="directory")){report(error.isEmpty()?QString("The selected entry changed. Search again."):error);return;}
            if(kind=="file"){
                AttachmentStore store(m_store.directory());const auto ref=store.importFile(chat,pin.absolutePath,{{"ws1",state->workspace}},&error);if(ref.isEmpty()){report(error);return;}
                bool exists=false;for(const auto &value:refs)exists|=value.toObject()["path"]==ref.path;
                if(!exists){if(refs.size()>=32){report("A message can contain at most 32 attachments.");return;}refs.append(ref.toJson());}
            }
        }else if(kind=="tab"){
            bool present=false;for(int index=0;index<m_tabs->count();++index)present|=m_tabs->widget(index)->property("browserTabId")==item["tabId"].toString();
            if(!present||!m_browserBridge->canAccessTab(item["tabId"].toString(),chat)){report("This browser tab is no longer available to this chat.");return;}
        }else if(kind=="workspace"){
            pin=ComposerWorkspaceSearch::pin(state->workspace,{},item["workspaceIdentity"].toString(),&error);if(!pin){report(error);return;}
        }else if(kind!="skill")return;
        auto next=m_sessions;for(auto &row:next)if(row.id==chat){row.draft=*nextDraft;row.draftAttachments=refs;}
        const QPointer<MainWindow> self(this);const auto previous=m_sessions;
        if(!commitConversationState(next,m_store.pendingCleanup())){if(self)report(m_status->text());return;}
        if(!self||!target)return;
        const bool inserted=target->commitCompletion(request,itemId);
        if(!self)return;
        if(!inserted){
            // There is no modal/event loop between validation and commit. If an
            // observer still revoked the choice, preserve its original draft.
            if(const auto* active=sessionFor();active&&active->id==chat)commitConversationState(previous,m_store.pendingCleanup());
            return;
        }
        if(self){if(fixedChat.isEmpty())refreshAttachments();else refreshSideChat(fixedChat);}
    });
    if(fixedChat.isEmpty())refreshCompletionContext();else target->setCompletionContext(fixedChat);
}
void MainWindow::refreshCompletionContext(){
    for(auto it=m_sideCompletionControllers.begin();it!=m_sideCompletionControllers.end();){
        const auto pane=m_sideChats.value(it.key());
        if(!it.value()||!pane){it=m_sideCompletionControllers.erase(it);continue;}
        it.value()->cancel();pane->composer()->setCompletionContext(it.key());pane->composer()->invalidateCompletions();++it;
    }
    if(m_completionController)m_completionController->cancel();
    if(m_composer){m_composer->setCompletionContext(current()&&!current()->archived?m_currentId:QString());m_composer->invalidateCompletions();}
}
bool MainWindow::configureTurnContext(const QString &id,const QString &userMessage,QString *error){
    if(error)error->clear();
    const auto *session=find(id);const QPointer<AgentToolHost> host=m_toolHosts.value(id);
    if(!session||session->archived||!host){if(error)*error="The chat context is unavailable.";return false;}
    const auto preferences=m_settings->preferences();const auto directory=m_store.directory();
    auto options=contextOptionsFor(preferences,session->workspace,QDir::homePath());QString failure;
    const QPointer<MainWindow> self(this);
    const bool configured=m_pluginCatalog.configureAgent(*host->network(),id,directory,preferences,&failure);
    if(!self||!host)return false;
    if(!configured){if(error)*error=failure;return false;}
    const auto plugins=PluginCatalog::context(directory,preferences,&failure);
    if(!failure.isEmpty()){if(error)*error=failure;return false;}
    configureAppRoots(host,plugins);
    if(!self||!host)return false;
    options.pluginsDirectory=plugins.pluginsDirectory;options.disabledPluginIds=plugins.disabledPluginIds;
    ContextLoader loader;const auto snapshot=loader.load(options);QSet<QString> requested;
    const QRegularExpression links("\\[[^\\]]*\\]\\(slash:([^\\)]+)\\)");auto matches=links.globalMatch(userMessage);
    while(matches.hasNext())requested.insert(QUrl::fromPercentEncoding(matches.next().captured(1).toUtf8()));
    const auto command=userMessage.trimmed().section(QRegularExpression("\\s+"),0,0);
    for(const auto &skill:snapshot.skills)if(skill.userInvocable&&(requested.contains(skill.id)||(command.startsWith('/')&&(skill.id==command.mid(1)||skill.name.compare(command.mid(1),Qt::CaseInsensitive)==0))))options.invokedSkillIds.append(skill.id);
    host->configureContext(options);return true;
}
}
