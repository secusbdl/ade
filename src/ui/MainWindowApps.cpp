#include "MainWindow.h"
#include "Icons.h"
#include "runtime/AgentToolHost.h"
#include "apps/MiniAppHost.h"
#include "apps/MiniAppPage.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include <QDir>
#include <QApplication>
#include <QLabel>
#include <QMap>
#include <QPalette>
#include <QTabWidget>
#include "SettingsPage.h"
namespace ade {
QJsonObject MainWindow::appThemeVariables() const{
    // Keep the upstream iframe theme message and semantic CSS names. Native
    // widgets and app previews use the same current application palette.
    const auto palette=qApp->palette();
    const bool dark=palette.color(QPalette::Base).lightness()<128;
    const auto color=[&](QPalette::ColorRole role){return palette.color(role).name();};
    const QMap<QString,QString> accents={{"blue","#4778d7"},{"violet","#855ad2"},{"green","#348a67"},{"amber","#ac7a24"},{"neutral","#777f8c"}};
    const auto accent=accents.value(m_settings->preference("personalization.theme").toString(),"#4778d7");
    return {{"--color-app-background",color(QPalette::Window)},{"--color-background",color(QPalette::Base)},
        {"--color-foreground",color(QPalette::Text)},{"--color-muted-foreground",color(QPalette::PlaceholderText)},
        {"--color-subtle-foreground",dark?"#a6abb4":"#7b7f85"},{"--color-sidebar-foreground",color(QPalette::PlaceholderText)},
        {"--color-surface-1",color(QPalette::Button)},{"--color-surface-2",color(QPalette::AlternateBase)},
        {"--color-surface-3",dark?"#343d4b":"#e2e8ee"},{"--color-border",dark?"#3d424b":"#e4e6e8"},
        {"--color-border-subtle",dark?"#393d45":"#e6e6e6"},{"--color-primary-foreground",accent},
        {"--color-primary-solid",accent},{"--color-brand-foreground","#4778d7"},{"--color-brand-solid","#4778d7"},
        {"--color-solid-foreground","#fdfdfd"},{"--color-overlay","#16191e"},
        {"--color-surface-tinted",QString("color-mix(in srgb, %1 12%, %2)").arg(accent,color(QPalette::Base))}};
}
void MainWindow::updateAppThemes(){
    const auto variables=appThemeVariables();QList<QPointer<AgentToolHost>> hosts;
    for(auto* host:m_toolHosts)hosts.append(host);
    const QPointer<MainWindow> self(this);
    for(const auto& host:hosts){if(host)host->apps()->setTheme(variables);if(!self)return;}
}
void MainWindow::configureAppRoots(AgentToolHost* host,const PluginContext& context){
    QHash<QString,QString> roots;
    if(!context.pluginsDirectory.isEmpty())for(const auto& plugin:PluginCatalog::definitions())
        if(plugin.runtimeReady&&!context.disabledPluginIds.contains(plugin.id))roots.insert(plugin.id,QDir(context.pluginsDirectory).filePath(plugin.id));
    host->apps()->setPluginRoots(roots);
}
void MainWindow::connectAppTabs(AgentToolHost* host){
    auto* apps=host->apps();
    apps->setTheme(appThemeVariables());
    connect(apps,&MiniAppHost::pageOpened,this,[this,apps](const QString& instance,MiniAppPage* page,const QString& title,bool active){
        const QPointer<MainWindow> self(this);const QPointer<MiniAppHost> owner(apps);const QPointer<MiniAppPage> preview(page);const QPointer<BrowserToolBridge> bridge(m_browserBridge);
        const auto owned=[&]{return self&&owner&&preview&&owner->ownsPage(instance,preview);};
        auto* browser=page->browser();browser->setProperty("miniAppBrowser",true);
        const auto tabId=bridge->registerTab(browser);
        if(!owned()){if(bridge)bridge->unregisterTab(tabId);return;}
        if(tabId.isEmpty()){owner->closeTab(instance);return;}
        page->setProperty("browserTabId",tabId);
        page->setProperty("appInstanceId",instance);page->setProperty("appAgentId",apps->agentId());
        const int index=m_tabs->addTab(page,icon("plugin"),title);
        if(!owned()){if(bridge)bridge->unregisterTab(tabId);return;}
        m_tabs->setTabToolTip(index,page->appUrl().toDisplayString());
        connect(page,&MiniAppPage::failed,this,[this](const QString& error){m_status->setText("App: "+error);});
        if(active){m_tabs->setCurrentIndex(index);revealContent();}else refreshWorkbench();
        if(!owned()){if(bridge)bridge->unregisterTab(tabId);return;}
        apps->bindTab(instance,tabId);
    });
    connect(apps,&MiniAppHost::pageActivationRequested,this,[this](const QString& tabId,const QString& title,bool active){
        for(int i=0;i<m_tabs->count();++i)if(m_tabs->widget(i)->property("browserTabId").toString()==tabId){m_tabs->setTabText(i,title);if(active){m_tabs->setCurrentIndex(i);revealContent();}break;}
    });
    connect(apps,&MiniAppHost::pageCloseRequested,this,[this](const QString& instance,const QString& tabId){
        const QPointer<MainWindow> self(this);
        // A synchronous activation observer can close the app between Qt tab
        // insertion and bindTab(). In that window the host's tabId is empty.
        QString registered=tabId;QPointer<QWidget> closing;
        for(int i=0;i<m_tabs->count();++i){auto* page=m_tabs->widget(i);if(page->property("appInstanceId").toString()==instance){closing=page;if(registered.isEmpty())registered=page->property("browserTabId").toString();break;}}
        m_browserBridge->unregisterTab(registered);if(!self)return;
        if(closing){const int index=m_tabs->indexOf(closing);if(index>=0)m_tabs->removeTab(index);}
        if(!self)return;
        refreshWorkbench();
    });
}
}
