#pragma once
#include "MiniAppPage.h"
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QSet>

namespace ade {

// One host belongs to one conversation. Only host-configured roots are routable.
class MiniAppHost final: public QObject {
    Q_OBJECT
public:
    MiniAppHost(const QString& agentId,const QString& dataDirectory,QObject* parent=nullptr);
    ~MiniAppHost() override;
    QString agentId() const {return m_agentId;}
    void setPluginRoots(const QHash<QString,QString>& roots);
    bool setAgentAppsDirectory(const QString& directory);
    void lockAgentAppsDirectory() {m_appsDirectoryLocked=true;}
    QString agentAppsDirectory() const {return m_agentAppsDirectory;}
    void setTheme(const QJsonObject& variables);
    QString activeInstanceId() const {return m_activeInstance;}
    MiniAppPage* activePage() const;
    bool ownsPage(const QString& instanceId,const MiniAppPage* page) const;
    QString activeTabId() const;
    // Forget sandbox-owned activity without closing already bound preview tabs.
    void resetContext();

public slots:
    void execute(const QString& requestId,const QString& operation,const QJsonObject& arguments);
    void cancel(const QString& requestId);
    void bindTab(const QString& instanceId,const QString& tabId);
    void reset();
    void closeTab(const QString& instanceId);

signals:
    void pageOpened(const QString& instanceId,ade::MiniAppPage* page,const QString& title,bool setActive);
    void pageActivationRequested(const QString& tabId,const QString& title,bool setActive);
    void pageCloseRequested(const QString& instanceId,const QString& tabId);
    void completed(const QString& requestId,const QJsonValue& value,const QString& error);
    void messageReceived(const QString& appId,const QString& pluginId,const QJsonValue& data);
    void activeAppChanged();

private:
    struct Instance {QPointer<MiniAppPage> page;QString appId,pluginId,tabId;QSet<QString> opening;};
    void open(const QString& requestId,const QJsonObject& arguments);
    void closeInstance(const QString& instanceId,const QString& reason);
    void pageDestroyed(const QString& instanceId);
    QString m_agentId,m_dataDirectory,m_agentAppsDirectory,m_activeInstance;
    QHash<QString,QString> m_pluginRoots;
    QHash<QString,Instance> m_instances;
    QHash<QString,QString> m_pendingOpen;
    QJsonObject m_theme;
    bool m_destroying=false,m_appsDirectoryLocked=false;
};
}
