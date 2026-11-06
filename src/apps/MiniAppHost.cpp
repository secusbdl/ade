#include "MiniAppHost.h"
#include "MiniAppPage.h"
#include "browser/browserwidget.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
#include <utility>

namespace ade {
namespace {
QString digest(const QStringList& parts){return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray::fromStringList(parts)).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());}
}
MiniAppHost::MiniAppHost(const QString& agentId,const QString& dataDirectory,QObject* parent):QObject(parent),m_agentId(agentId),m_dataDirectory(QDir(dataDirectory).absolutePath()){
    m_agentAppsDirectory=QDir(m_dataDirectory).filePath(QStringLiteral("agent-apps/")+digest({m_agentId}));
}
MiniAppHost::~MiniAppHost(){
    m_destroying=true;const auto instances=std::exchange(m_instances,{});
    for(const auto& instance:instances)if(instance.page){instance.page->disconnect(this);delete instance.page;}
}
MiniAppPage* MiniAppHost::activePage() const{return m_instances.value(m_activeInstance).page;}
bool MiniAppHost::ownsPage(const QString& instanceId,const MiniAppPage* page) const{return page&&m_instances.value(instanceId).page==page;}
QString MiniAppHost::activeTabId() const{return m_instances.value(m_activeInstance).tabId;}
void MiniAppHost::setPluginRoots(const QHash<QString,QString>& roots){
    QHash<QString,QString> safe;for(auto it=roots.cbegin();it!=roots.cend();++it)if(AppResourceStore::safeSegment(it.key())&&!it.value().isEmpty())safe.insert(it.key(),QDir(it.value()).absolutePath());
    const auto previous=m_pluginRoots;m_pluginRoots=safe;const auto ids=m_instances.keys();const QPointer<MiniAppHost> self(this);
    for(const auto& id:ids){const auto plugin=m_instances.value(id).pluginId;if(!plugin.isEmpty()&&(!safe.contains(plugin)||previous.value(plugin)!=safe.value(plugin))){closeInstance(id,tr("App plugin authorization changed."));if(!self)return;}}
}
bool MiniAppHost::setAgentAppsDirectory(const QString& directory){
    if(m_appsDirectoryLocked||!m_instances.isEmpty()||directory.isEmpty())return false;
    m_agentAppsDirectory=QDir(directory).absolutePath();return true;
}
void MiniAppHost::setTheme(const QJsonObject& variables){m_theme=variables;const auto instances=m_instances;const QPointer<MiniAppHost> self(this);for(const auto& instance:instances){if(instance.page)instance.page->setTheme(variables);if(!self)return;}}
void MiniAppHost::execute(const QString& id,const QString& operation,const QJsonObject& args){
    if(id.isEmpty()||id.size()>512){emit completed(id,QJsonValue(),tr("Invalid app request ID."));return;}
    if(m_pendingOpen.contains(id)){cancel(id);return;}
    if(operation=="open"){open(id,args);return;}
    if(operation=="close"){
        const auto active=m_activeInstance;const QPointer<MiniAppHost> self(this);if(!active.isEmpty())closeInstance(active,tr("App was closed."));if(self)emit completed(id,QJsonValue(QJsonValue::Null),{});return;
    }
    if(operation=="send"){
        const auto instance=m_instances.value(m_activeInstance);
        if(!instance.page||instance.appId!=args["appId"].toString()||instance.pluginId!=args["pluginId"].toString()){emit completed(id,QJsonValue(),tr("Target app is not active."));return;}
        QString error;const QPointer<MiniAppHost> self(this);const bool success=instance.page->postMessage(args["data"],&error);
        if(self)emit completed(id,QJsonValue(QJsonValue::Null),success?QString():error);
        return;
    }
    emit completed(id,QJsonValue(),tr("Unknown app operation."));
}
void MiniAppHost::open(const QString& id,const QJsonObject& args){
    const QString appId=args["appId"].toString(),pluginId=args["pluginId"].toString();
    if(!AppResourceStore::safeSegment(appId)||(!pluginId.isEmpty()&&!AppResourceStore::safeSegment(pluginId))
        ||(!args["target"].isUndefined()&&args["target"]!="tab")
        ||(!args["setActive"].isUndefined()&&!args["setActive"].isBool())
        ||(!args["title"].isUndefined()&&!args["title"].isString())){emit completed(id,QJsonValue(),tr("Invalid app options."));return;}
    const bool setActive=args["setActive"].toBool(true);const QString title=args["title"].toString(appId).left(200);
    if(!pluginId.isEmpty()&&!m_pluginRoots.contains(pluginId)){emit completed(id,QJsonValue(),tr("The app plugin is not enabled for this agent."));return;}
    if(pluginId.isEmpty()&&!AppResourceStore::safeSegment(m_agentId)){emit completed(id,QJsonValue(),tr("Invalid agent app identity."));return;}
    const auto active=m_instances.value(m_activeInstance);
    if(active.page&&active.appId==appId&&active.pluginId==pluginId&&!active.tabId.isEmpty()){
        const QPointer<MiniAppHost> self(this);active.page->reloadApp();if(!self)return;
        emit pageActivationRequested(active.tabId,title,setActive);if(self)emit completed(id,QJsonObject{{"tabId",active.tabId}},{});return;
    }
    if(m_instances.size()>=16||m_pendingOpen.size()>=16){emit completed(id,QJsonValue(),tr("Too many open app previews."));return;}
    const QString root=pluginId.isEmpty()?QDir(m_agentAppsDirectory).filePath(appId):QDir(m_pluginRoots.value(pluginId)).filePath(QStringLiteral("apps/")+appId);
    const QString owner=pluginId.isEmpty()?m_agentId:pluginId;
    const QUrl base(QStringLiteral("app://")+(pluginId.isEmpty()?QStringLiteral("agents/"):QStringLiteral("plugins/"))+QString::fromLatin1(QUrl::toPercentEncoding(owner))+'/'+QString::fromLatin1(QUrl::toPercentEncoding(appId)));
    const QString profile=QDir(m_dataDirectory).filePath(QStringLiteral("mini-app-profiles/")+digest({m_agentId,pluginId,appId}));
    auto* page=new MiniAppPage(base,root,profile);if(!page->configured()){const QString error=page->configurationError();delete page;emit completed(id,QJsonValue(),error);return;}
    page->browser()->setProperty("adeOwnerAgentId",m_agentId);
    const QString instanceId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    Instance instance;instance.page=page;instance.appId=appId;instance.pluginId=pluginId;instance.opening.insert(id);
    m_instances.insert(instanceId,instance);m_pendingOpen.insert(id,instanceId);m_activeInstance=instanceId;
    page->setTheme(m_theme);
    connect(page,&MiniAppPage::messageReceived,this,[this,instanceId](const QJsonValue& data){if(m_activeInstance!=instanceId)return;const auto instance=m_instances.value(instanceId);if(instance.page)emit messageReceived(instance.appId,instance.pluginId,data);});
    connect(page,&QObject::destroyed,this,[this,instanceId]{pageDestroyed(instanceId);});
    connect(page,&MiniAppPage::failed,this,[this,instanceId](const QString& error){if(!m_instances.contains(instanceId))return;const auto requests=m_instances.value(instanceId).opening;const QPointer<MiniAppHost> self(this);for(const auto& request:requests){m_pendingOpen.remove(request);m_instances[instanceId].opening.remove(request);emit completed(request,QJsonValue(),error);if(!self)return;}});
    QTimer::singleShot(10000,this,[this,instanceId]{const auto instance=m_instances.value(instanceId);if(instance.page&&instance.tabId.isEmpty())closeInstance(instanceId,tr("The native app tab was not attached."));});
    const QPointer<MiniAppHost> self(this);emit activeAppChanged();if(!self)return;
    emit pageOpened(instanceId,page,title,setActive);
}
void MiniAppHost::bindTab(const QString& instanceId,const QString& tabId){
    auto it=m_instances.find(instanceId);if(it==m_instances.end()||!it->page||tabId.isEmpty()||tabId.size()>256)return;
    it->tabId=tabId;const auto requests=std::exchange(it->opening,{});const QPointer<MiniAppHost> self(this);
    for(const auto& id:requests){m_pendingOpen.remove(id);emit completed(id,QJsonObject{{"tabId",tabId}},{});if(!self)return;}
}
void MiniAppHost::cancel(const QString& id){
    const auto instanceId=m_pendingOpen.take(id);if(instanceId.isEmpty())return;
    auto it=m_instances.find(instanceId);if(it!=m_instances.end())it->opening.remove(id);
    const bool orphan=it!=m_instances.end()&&it->tabId.isEmpty()&&it->opening.isEmpty();const QPointer<MiniAppHost> self(this);
    emit completed(id,QJsonValue(),tr("App request canceled."));if(self&&orphan)closeInstance(instanceId,tr("App request canceled."));
}
void MiniAppHost::closeInstance(const QString& id,const QString& reason){
    if(!m_instances.contains(id))return;
    const auto instance=m_instances.take(id);const bool active=m_activeInstance==id;if(active)m_activeInstance.clear();
    const QPointer<MiniAppHost> self(this);
    for(const auto& request:instance.opening){m_pendingOpen.remove(request);emit completed(request,QJsonValue(),reason);if(!self)return;}
    if(instance.page)instance.page->disconnect(this);
    emit pageCloseRequested(id,instance.tabId);if(instance.page)instance.page->deleteLater();if(!self)return;
    if(active)emit activeAppChanged();
}
void MiniAppHost::pageDestroyed(const QString& id){
    if(m_destroying||!m_instances.contains(id))return;
    const auto instance=m_instances.take(id);const bool active=m_activeInstance==id;if(active)m_activeInstance.clear();
    const QPointer<MiniAppHost> self(this);for(const auto& request:instance.opening){m_pendingOpen.remove(request);emit completed(request,QJsonValue(),tr("App tab was closed."));if(!self)return;}if(active)emit activeAppChanged();
}
void MiniAppHost::closeTab(const QString& instanceId){closeInstance(instanceId,tr("App tab was closed."));}
void MiniAppHost::reset(){const auto ids=m_instances.keys();const QPointer<MiniAppHost> self(this);for(const auto& id:ids){closeInstance(id,tr("Agent app context was reset."));if(!self)return;}}
}
