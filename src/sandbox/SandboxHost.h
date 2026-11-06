#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>

class QTimer;

namespace ade {

class SandboxHost final : public QObject {
    Q_OBJECT
public:
    explicit SandboxHost(QObject* parent = nullptr);
    ~SandboxHost() override;
    void setAgentId(const QString& agentId);
    QString agentId() const { return m_agentId; }
    void setWorkspace(const QString& workspace);
    void setAppsDirectory(const QString& directory);
    void setArtifactDirectory(const QString& directory);
    void setWorkerExecutable(const QString& executable);
    void setExecutionLimits(int synchronousMs, int inactivityMs, int hardMs,
                            quint64 memoryBytes = 64ull * 1024 * 1024);
    QJsonArray definitions() const;
    bool handles(const QString& name) const;
    bool isRunning() const;
    QString requestOwnerId(const QString& rpcRequestId)const;

public slots:
    void execute(const QString& id, const QString& name, const QJsonObject& arguments);
    void cancel(const QString& id);
    void cancelAll();
    void resetAgent(const QString& agentId);
    void completeBrowserRequest(const QString& requestId, const QString& output, bool error);
    void completeFileRequest(const QString& requestId, const QString& output, bool error);
    void completeNetworkRequest(const QString& requestId, const QJsonObject& response, const QString& error);
    void completeCredentialRequest(const QString& requestId, const QJsonValue& value, const QString& error);
    void completeAppRequest(const QString& requestId,const QJsonValue& value,const QString& error);
    void deliverEvent(const QString& listenerId,const QJsonValue& value);
    void endEventSubscription(const QString& listenerId,const QString& error);

signals:
    void completed(const QString& id, const QString& output, bool error);
    void output(const QString& id, const QString& text);
    void consoleMessage(const QString& agentId, const QString& level, const QString& text);
    void browserRequest(const QString& requestId, const QString& tabId,
                        const QString& method, const QJsonObject& parameters);
    void fileRequest(const QString& requestId, const QString& toolName, const QJsonObject& arguments);
    void attachmentFileRequest(const QString& requestId,const QString& agentId,const QString& operation,const QJsonObject& arguments);
    void networkRequest(const QString& requestId, const QString& agentId, const QJsonObject& request);
    void credentialRequest(const QString& requestId, const QString& agentId, const QString& typeId);
    void contextReset(const QString& agentId);
    void appRequest(const QString& requestId,const QString& agentId,const QString& operation,const QJsonObject& arguments);
    void eventSubscriptionRequested(const QString& listenerId,const QString& agentId,const QString& kind,const QJsonObject& arguments);
    void eventUnsubscriptionRequested(const QString& listenerId);
    void brokerCancellationRequested(const QString& requestId);
    void attachmentCreated(const QString& id, const QString& path, const QString& mimeType);

private:
    struct Pending {
        QString wireId;
        QString agentId;
        QString contextId;
        QPointer<QTimer> watchdog;
        bool cancelRequested = false;
    };
    struct Rpc { QString wireId,kind,agentId,contextId; QPointer<QTimer> deadline; };
    struct Subscription {QString agentId,contextId;QList<QJsonValue> queue;qsizetype bytes=0;bool inFlight=false;};
    void ensureProcess();
    void send(const QJsonObject& message);
    void readOutput();
    void receive(const QJsonObject& message);
    void settle(const QString& id, const QString& value, bool error);
    void failAll(const QString& reason);
    void completeRpc(const QString& requestId, const QJsonValue& value, const QString& error);
    void createAttachment(const QString& requestId, const QJsonObject& arguments);
    void invalidateContext(const QString& agentId,const QString& contextId);
    void flushEvent(const QString& listenerId);
    QString workspacePath(const QString& path, QString* error) const;

    QProcess m_process;
    QByteArray m_buffer;
    QList<QJsonObject> m_outgoing;
    QHash<QString, Pending> m_pending;
    QHash<QString, QString> m_wireToId;
    QHash<QString, Rpc> m_rpc;
    QSet<QString> m_contexts;
    QHash<QString,QString> m_contextIds;
    QHash<QString,Subscription> m_subscriptions;
    QString m_agentId = QStringLiteral("default");
    QString m_workspace;
    QString m_appsDirectory;
    QString m_artifactDirectory;
    QString m_executable;
    int m_synchronousMs = 30000;
    int m_inactivityMs = 45000;
    int m_hardMs = 180000;
    quint64 m_memoryBytes = 64ull * 1024 * 1024;
    bool m_destroying = false;
};
} // namespace ade
