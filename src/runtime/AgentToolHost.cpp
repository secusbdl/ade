#include "AgentToolHost.h"
#include "core/AgentEngine.h"
#include "browser/BrowserToolBridge.h"
#include "attachments/ModelMediaCatalog.h"
#include <QDir>
#include <QFileInfo>
namespace ade {
namespace {
bool prepareAppDirectory(const QString& directory){
    const auto linked=[](QString path){while(true){const QFileInfo info(path);if(info.isSymLink()||info.isJunction())return true;const auto parent=info.absolutePath();if(parent==path)return false;path=parent;}};
    return !linked(directory)&&QDir().mkpath(directory)&&!linked(directory);
}
}

AgentToolHost::AgentToolHost(AgentEngine* engine,BrowserToolBridge* browser,const QString& agentId,
    const QString& dataDirectory,QObject* parent):QObject(parent),m_engine(engine),m_browser(browser),m_agentId(agentId),
    m_attachmentStore(dataDirectory),m_attachmentPipeline(m_attachmentStore),m_readSnapshots(dataDirectory),m_readLease(m_readSnapshots.acquire(agentId)),m_attachmentFiles(agentId,dataDirectory,this),m_apps(agentId,dataDirectory,this) {
    m_attachmentPipeline.setFileReadLease(m_readLease);
    engine->setAttachmentReadSource(m_attachmentFiles.formattedSource());
    engine->setFileReadCommitter([lease=m_readLease](const FileReadCapture& capture,const std::atomic_bool* canceled,QString* error){return FileReadSnapshotStore::commit(lease,capture,canceled,error);});
    engine->setMessagePreparer([guard=QPointer<AgentToolHost>(this)](const QJsonArray& history,const ProviderConfig& provider,QString* error){
        if(!guard||guard->m_fileContextRemoved){if(error)*error="The file context host is unavailable or was removed.";return QJsonArray{};}
        return guard->m_attachmentPipeline.prepare(history,guard->m_agentId,guard->m_attachmentMounts,ModelMediaCatalog::resolve(provider.model),{},error);
    });
    const auto appsDirectory=m_apps.agentAppsDirectory();
    if(prepareAppDirectory(appsDirectory)&&m_files.setAppsDirectory(appsDirectory)){
        m_sandbox.setAppsDirectory(m_files.appsDirectory());engine->setAppsDirectory(m_files.appsDirectory());
        m_attachmentMounts.insert("apps",m_files.appsDirectory());
    }
    // The browser, file brokers and attachment capture share one immutable root.
    // Standalone MiniAppHost fixtures may configure a root before this binding.
    m_apps.lockAgentAppsDirectory();
    m_sandbox.setAgentId(agentId);
    m_sandbox.setArtifactDirectory(m_attachmentFiles.rootDirectory());
    // AgentEngine bounds external calls to 120 seconds. Leave time for broker cleanup.
    m_sandbox.setExecutionLimits(30000,45000,110000);
    auto definitions=m_sandbox.definitions();
    for(const auto& definition:ContextLoader::toolDefinitions())definitions.append(definition);
    if(browser)for(const auto& definition:browser->definitions())definitions.append(definition);
    engine->setExternalTools(definitions);
    connect(engine,&AgentEngine::externalToolRequested,this,[this](const QString& id,const QString& name,const QJsonObject& args){
        if(name=="readContext"){QString error;const auto output=m_context.readFile(args["path"].toString(),&error);if(m_engine)m_engine->completeExternalTool(id,error.isEmpty()?output:error,!error.isEmpty());return;}
        if(m_sandbox.handles(name)){m_toolAttachments.remove(id);m_attachmentErrors.remove(id);m_sandbox.execute(id,name,args);return;}
        if(m_browser&&m_browser->handles(name)){m_directBrowser.insert(id);m_browser->executeForAgent(id,m_agentId,name,args);return;}
        if(m_engine)m_engine->completeExternalTool(id,"Error: External tool is unavailable.",true);
    });
    connect(engine,&AgentEngine::externalToolCancellationRequested,this,[this](const QString& id){
        const QPointer<AgentToolHost> self(this);m_sandbox.cancel(id);if(!self)return;
        if(m_directBrowser.remove(id)&&m_browser)m_browser->cancel(id);
    });
    connect(&m_sandbox,&SandboxHost::completed,this,[this](const QString& id,const QString& output,bool error){
        const auto references=m_toolAttachments.take(id);auto attachmentError=m_attachmentErrors.take(id);
        QJsonObject metadata;
        if(attachmentError.isEmpty()&&!references.isEmpty())metadata=captureAttachments(references,&attachmentError);
        const auto result=attachmentError.isEmpty()?output:output+"\nAttachment processing failed: "+attachmentError;
        if(m_engine)m_engine->completeExternalToolWithAttachments(id,result,error||!attachmentError.isEmpty(),metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject());
    });
    connect(&m_sandbox,&SandboxHost::output,this,&AgentToolHost::output);
    connect(&m_sandbox,&SandboxHost::attachmentCreated,this,[this](const QString& id,const QString& path,const QString& mime){
        QString error;const auto ref=m_attachmentStore.existingBlob(m_agentId,QFileInfo(path).fileName(),{},&error);
        if(error.isEmpty()) {
            const auto actual=m_attachmentStore.resolve(m_agentId,ref,{},&error);
            if(QFileInfo(actual).canonicalFilePath().compare(QFileInfo(path).canonicalFilePath(),Qt::CaseInsensitive)!=0)error="The generated attachment path is outside its chat.";
        }
        // Upstream persists idle-created files, but only an active tool collects
        // attachment metadata or streams it into the conversation.
        if(id.isEmpty())return;
        if(!error.isEmpty()){m_attachmentErrors.insert(id,error);return;}
        if(m_toolAttachments.value(id).size()>=32){m_attachmentErrors.insert(id,"A tool can return at most 32 attachments.");return;}
        m_toolAttachments[id].append(ref.toJson());emit attachmentCreated(id,path,mime);
    });
    connect(&m_sandbox,&SandboxHost::browserRequest,this,[this](const QString& id,const QString& tab,const QString& method,const QJsonObject& params){
        if(!m_browser){m_sandbox.completeBrowserRequest(id,"Browser registry is unavailable.",true);return;}
        m_sandboxBrowser.insert(id);m_browser->executeForAgent(id,m_agentId,"sendCDP",{{"tabId",tab},{"method",method},{"params",params}});
    });
    if(browser)connect(browser,&BrowserToolBridge::completed,this,[this](const QString& id,const QString& output,bool error){
        if(m_sandboxBrowser.remove(id))m_sandbox.completeBrowserRequest(id,output,error);
        else if(m_directBrowser.remove(id)&&m_engine)m_engine->completeExternalTool(id,output,error);
    });
    connect(&m_sandbox,&SandboxHost::fileRequest,this,[this](const QString& id,const QString& name,const QJsonObject& arguments){
        if(m_workspaceChanging){m_sandbox.completeFileRequest(id,"The workspace is changing; retry after it is connected.",true);return;}
        m_sandboxFiles.insert(id);m_files.execute(id,name,arguments);
    });
    connect(&m_files,&ToolExecutor::completed,this,[this](const QString& id,const QString& output,bool error){
        if(m_sandboxFiles.remove(id))m_sandbox.completeFileRequest(id,output,error);
    });
    connect(&m_files,&ToolExecutor::fileChanged,this,&AgentToolHost::fileChanged);
    connect(&m_sandbox,&SandboxHost::attachmentFileRequest,this,[this](const QString& id,const QString& agent,const QString& operation,const QJsonObject& arguments){
        m_sandboxAttachmentFiles.insert(id);m_attachmentFiles.execute(id,agent,operation,arguments);
    });
    connect(&m_attachmentFiles,&AttachmentFileBroker::completed,this,[this](const QString& id,const QString& output,bool error){
        if(m_sandboxAttachmentFiles.remove(id))m_sandbox.completeFileRequest(id,output,error);
    });
    connect(&m_sandbox,&SandboxHost::networkRequest,this,[this](const QString& id,const QString& agent,const QJsonObject& request){
        if(agent!=m_agentId){m_sandbox.completeNetworkRequest(id,{},"Agent identity does not match the native host.");return;}
        m_sandboxNetwork.insert(id);m_network.fetch(id,m_agentId,request);
    });
    connect(&m_sandbox,&SandboxHost::credentialRequest,this,[this](const QString& id,const QString& agent,const QString& type){
        if(agent!=m_agentId){m_sandbox.completeCredentialRequest(id,{},"Agent identity does not match the native host.");return;}
        QString error;const auto value=m_network.credential(m_agentId,type,&error);m_sandbox.completeCredentialRequest(id,value,error);
    });
    connect(&m_network,&NetworkBroker::completed,this,[this](const QString& id,const QJsonObject& response,const QString& error){
        if(m_sandboxNetwork.remove(id))m_sandbox.completeNetworkRequest(id,response,error);
    });
    connect(&m_sandbox,&SandboxHost::contextReset,this,[this](const QString& agent){
        if(agent!=m_agentId)return;
        const QPointer<AgentToolHost> self(this);
        m_sandboxNetwork.clear();m_network.resetAgent(m_agentId);if(!self)return;
        resetEvents();if(!self)return;
        m_sandboxApps.clear();m_apps.reset();if(!self)return;
        m_sandboxFiles.clear();m_files.cancel();if(!self)return;
        m_sandboxAttachmentFiles.clear();m_attachmentFiles.cancelAll();
    });
    connect(&m_sandbox,&SandboxHost::brokerCancellationRequested,this,[this](const QString& id){
        const QPointer<AgentToolHost> self(this);
        if(m_sandboxBrowser.remove(id)&&m_browser)m_browser->cancel(id);
        if(!self)return;
        if(m_sandboxNetwork.remove(id))m_network.cancel(id);
        if(!self)return;
        if(m_sandboxApps.remove(id))m_apps.cancel(id);
        if(!self)return;
        if(m_sandboxFiles.remove(id))m_files.cancel();
        if(!self)return;
        if(m_sandboxAttachmentFiles.remove(id))m_attachmentFiles.cancel(id);
    });
    connect(&m_sandbox,&SandboxHost::appRequest,this,[this](const QString& id,const QString& agent,const QString& operation,const QJsonObject& args){
        if(agent!=m_agentId){m_sandbox.completeAppRequest(id,{},"Agent identity does not match the native host.");return;}
        m_sandboxApps.insert(id);m_apps.execute(id,operation,args);
    });
    connect(&m_apps,&MiniAppHost::completed,this,[this](const QString& id,const QJsonValue& value,const QString& error){
        if(m_sandboxApps.remove(id))m_sandbox.completeAppRequest(id,value,error);
    });
    connect(&m_sandbox,&SandboxHost::eventSubscriptionRequested,this,[this](const QString& id,const QString& agent,const QString& kind,const QJsonObject& args){
        if(agent!=m_agentId||m_eventSubscriptions.contains(id)||m_eventSubscriptions.size()>=128){m_sandbox.endEventSubscription(id,"Invalid or excessive event subscription.");return;}
        if(kind!="cdp"&&kind!="appMessage"){m_sandbox.endEventSubscription(id,"Unknown event subscription kind.");return;}
        auto details=args;details["kind"]=kind;m_eventSubscriptions.insert(id,details);
        if(kind=="cdp") {
            if(m_browser&&m_browser->canAccessTab(args["tabId"].toString(),m_agentId))m_browser->subscribeEvent(id,args["tabId"].toString(),args["event"].toString());
            else {m_eventSubscriptions.remove(id);m_sandbox.endEventSubscription(id,"Browser tab is unavailable to this agent.");}
        }
    });
    connect(&m_sandbox,&SandboxHost::eventUnsubscriptionRequested,this,&AgentToolHost::removeEventSubscription);
    if(browser){
        connect(browser,&BrowserToolBridge::eventReceived,this,[this](const QString& id,const QJsonObject& parameters){
            if(m_eventSubscriptions.value(id)["kind"]=="cdp")m_sandbox.deliverEvent(id,parameters);
        });
        connect(browser,&BrowserToolBridge::subscriptionEnded,this,[this](const QString& id,const QString& error){
            if(m_eventSubscriptions.remove(id))m_sandbox.endEventSubscription(id,error);
        });
    }
    connect(&m_apps,&MiniAppHost::messageReceived,this,[this](const QString& appId,const QString& pluginId,const QJsonValue& data){
        const auto subscriptions=m_eventSubscriptions;const QPointer<AgentToolHost> self(this);
        for(auto it=subscriptions.cbegin();it!=subscriptions.cend();++it)if(it.value()["kind"]=="appMessage"&&it.value()["appId"]==appId&&it.value()["pluginId"].toString()==pluginId&&m_eventSubscriptions.contains(it.key())){
            m_sandbox.deliverEvent(it.key(),data);if(!self)return;
        }
    });
}
AgentToolHost::~AgentToolHost(){
    FileReadSnapshotStore::revoke(m_readLease);
    if(m_engine){disconnect(m_engine,nullptr,this,nullptr);disconnect(&m_sandbox,nullptr,m_engine,nullptr);}
    m_sandbox.cancelAll();m_files.cancel();m_sandboxAttachmentFiles.clear();m_attachmentFiles.cancelAll();
    m_sandboxNetwork.clear();m_network.resetAgent(m_agentId);
    resetEvents();m_sandboxApps.clear();m_apps.reset();
    disconnect(&m_sandbox,nullptr,this,nullptr);disconnect(&m_network,nullptr,this,nullptr);
    if(m_browser){
        // Remove IDs before cancellation: completion can be emitted synchronously.
        const auto direct=m_directBrowser;const auto sandbox=m_sandboxBrowser;m_directBrowser.clear();m_sandboxBrowser.clear();
        for(const auto& id:direct)m_browser->cancel(id);
        for(const auto& id:sandbox)m_browser->cancel(id);
    }
}
void AgentToolHost::removeEventSubscription(const QString& id){const auto details=m_eventSubscriptions.take(id);if(details["kind"]=="cdp"&&m_browser)m_browser->unsubscribeEvent(id);}
void AgentToolHost::resetEvents(){const auto ids=m_eventSubscriptions.keys();const QPointer<AgentToolHost> self(this);for(const auto& id:ids){removeEventSubscription(id);if(!self)return;}}
bool AgentToolHost::setWorkspace(const QString& path,QString* error){
    if(error)error->clear();
    if(m_workspaceChanging){if(error)*error="A workspace change is already in progress.";return false;}
    ToolExecutor candidate;
    if(!path.isEmpty()&&!candidate.setWorkspace(path)){if(error)*error="The requested workspace is unavailable.";return false;}
    const auto accepted=candidate.workspace();
    if(accepted.compare(m_files.workspace(),Qt::CaseInsensitive)==0)return true;
    const auto previous=m_files.workspace();m_workspaceChanging=true;
    const QPointer<AgentToolHost> self(this);
    // ws1 currently represents one root. Revoke old callbacks before rebinding it.
    m_sandbox.resetAgent(m_agentId);if(!self)return false;
    m_sandboxFiles.clear();m_files.cancel();if(!self)return false;
    const bool connected=m_files.setWorkspace(accepted);
    bool success=accepted.isEmpty()||connected;
    if(!success)m_files.setWorkspace(previous);
    const auto actual=m_files.workspace();m_sandbox.setWorkspace(actual);m_attachmentMounts.clear();
    if(!m_files.appsDirectory().isEmpty())m_attachmentMounts.insert("apps",m_files.appsDirectory());
    if(!actual.isEmpty()){m_attachmentMounts.insert("ws1",actual);m_attachmentMounts.insert("w1",actual);}
    m_workspaceChanging=false;
    if(!success&&error)*error="The workspace disappeared while connecting; the previous root was retained when available.";
    return success;
}
QJsonObject AgentToolHost::captureAttachments(const QJsonArray& references,QString* error){
    if(m_fileContextRemoved){if(error)*error="This chat's file context was removed.";return {};}
    return m_attachmentPipeline.capture(m_agentId,references,m_attachmentMounts,error);
}
bool AgentToolHost::cloneFileContext(const QString& targetAgentId,QString* error){
    if(error)error->clear();
    if(m_fileContextRemoved){if(error)*error="This chat's file context was removed.";return false;}
    if(!m_readSnapshots.cloneAgent(m_agentId,targetAgentId,error))return false;
    if(!m_attachmentStore.cloneAgent(m_agentId,targetAgentId,error)){
        QString ignored;m_readSnapshots.removeAgent(targetAgentId,&ignored);return false;
    }
    return true;
}
bool AgentToolHost::removeFileContext(QString* error){
    if(error)error->clear();
    m_fileContextRemoved=true;
    const QPointer<AgentToolHost> self(this);
    if(m_engine&&m_engine->isBusy())m_engine->cancel();
    if(!self){if(error)*error="The file context owner closed during cancellation.";return false;}
    FileReadSnapshotStore::revoke(m_readLease);
    m_sandbox.setArtifactDirectory({});m_sandbox.resetAgent(m_agentId);
    if(!self){if(error)*error="The file context owner closed during reset.";return false;}
    QString readError,attachmentError;
    const bool removedReads=m_readSnapshots.removeAgent(m_agentId,&readError);
    const bool removedAttachments=m_attachmentStore.removeAgent(m_agentId,&attachmentError);
    if(error)*error=readError.isEmpty()?attachmentError:readError;
    return removedReads&&removedAttachments;
}
ContextSnapshot AgentToolHost::configureContext(const ContextOptions& options){auto snapshot=m_context.load(options);if(m_engine)m_engine->setSystemContext(snapshot.prompt);return snapshot;}
}
