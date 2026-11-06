#pragma once

#include "BrowserContext.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>

class QTimer;

namespace ade {
class BrowserWidget;

// Native exposure of Stagewise's API.sendCDP and readConsoleLogs capabilities.
// This class does not implement Stagewise's persistent Node.js sandbox.
class BrowserToolBridge final : public QObject {
    Q_OBJECT
public:
    explicit BrowserToolBridge(QObject* parent = nullptr);
    ~BrowserToolBridge() override;
    QString registerTab(BrowserWidget* browser, const QString& requestedId = QString());
    void unregisterTab(const QString& tabId);
    // Only the host selects focus; empty clears it. Registration does not focus.
    bool setActiveTab(const QString& tabId);
    QString activeTabId() const { return m_activeTabId; }
    QJsonArray definitions() const;
    bool handles(const QString& name) const;
    void setRequestTimeoutMs(int milliseconds);
    // Owner-thread value copy. Empty/invalid identity never means unscoped.
    BrowserContextSnapshot snapshotForAgent(const QString& agentId) const;
    bool canAccessTab(const QString& tabId,const QString& agentId) const;

public slots:
    void execute(const QString& id, const QString& name, const QJsonObject& arguments);
    void executeForAgent(const QString& id,const QString& agentId,const QString& name,const QJsonObject& arguments);
    void cancel(const QString& id);
    void cancelAll();
    void subscribeEvent(const QString& subscriptionId,const QString& tabId,const QString& eventName);
    void unsubscribeEvent(const QString& subscriptionId);

signals:
    void completed(const QString& id, const QString& output, bool error);
    void tabsChanged();
    void eventReceived(const QString& subscriptionId,const QJsonObject& parameters);
    void subscriptionEnded(const QString& subscriptionId,const QString& error);

private:
    struct Tab {
        QPointer<BrowserWidget> browser;
        QString title;
        QString url;
        qint64 lastFocusedAt = 0;
        quint64 focusOrder = 0;
        QList<QJsonObject> logs;
        QList<QMetaObject::Connection> connections;
    };
    struct Pending {
        QString tabId;
        QString correlation;
        QPointer<QTimer> timeout;
    };
    void finish(const QString& id, const QString& output, bool error);
    void finishJson(const QString& id, const QJsonObject& output, bool error = false);
    void prepareConsole(const QString& tabId);
    void receiveConsole(const QString& tabId, const QString& event, const QString& json);
    void readConsole(const QString& id, const QString& tabId, const QJsonObject& arguments);
    QJsonObject tabsSnapshot(const QString& agentId = {}) const;
    void executeScoped(const QString& id,const QString& agentId,const QString& name,const QJsonObject& arguments);
    bool stillPending(const QString& id, const QString& correlation) const;
    void receiveEvent(const QString& tabId,const QString& event,const QString& json);

    QHash<QString, Tab> m_tabs;
    QStringList m_order;
    QSet<QString> m_usedIds;
    QString m_activeTabId;
    quint64 m_nextTabId = 1;
    int m_timeoutMs = 20000;
    QHash<QString, Pending> m_pending;
    QHash<QString, QString> m_correlations;
    struct Subscription {QString tabId,eventName;};
    QHash<QString,Subscription> m_subscriptions;
};
} // namespace ade
