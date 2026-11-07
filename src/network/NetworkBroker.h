#pragma once
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QStringList>

namespace ade {
struct CredentialType {
    QString id;
    QStringList secretFields;
    QStringList allowedOrigins;
    QStringList publicFields;
};
struct NetworkLimits {
    int timeoutMs = 30000;
    qint64 maxRequestBytes = 1024 * 1024;
    qint64 maxResponseBytes = 2 * 1024 * 1024;
    int maxConcurrent = 8;
    int maxRedirects = 5;
};
// Host-only credential values never enter worker IPC. agentId and credential configuration
// must come from the host, not fields supplied by sandbox code.
class NetworkBroker final : public QObject {
    Q_OBJECT
public:
    explicit NetworkBroker(QObject *parent = nullptr);
    // Host-only dependency injection. A supplied manager is borrowed and must
    // live on this object's thread; nullptr selects the ordinary owned manager.
    NetworkBroker(QNetworkAccessManager *manager, QObject *parent);
    ~NetworkBroker() override;
    bool registerCredentialType(const CredentialType &definition, QString *error = nullptr);
    // Host-owned store reconciliation. Empty fields remove this type's saved
    // credential; identical definition/data leave existing requests and handles intact.
    bool synchronizeCredential(const CredentialType &definition, const QJsonObject &fields, QString *error = nullptr);
    bool setCredential(const QString &typeId, const QJsonObject &fields, QString *error = nullptr);
    void removeCredential(const QString &typeId);
    void setAllowedCredentialTypes(const QString &agentId, const QStringList &typeIds);
    QJsonValue credential(const QString &agentId, const QString &typeId, QString *error = nullptr);
    bool setLimits(const NetworkLimits &limits);
    static bool originMatches(const QString &url, const QString &pattern);
    static QList<CredentialType> builtinCredentialTypes();

public slots:
    void fetch(const QString &requestId, const QString &agentId, const QJsonObject &request);
    void cancel(const QString &requestId);
    void cancelAgent(const QString &agentId);
    void resetAgent(const QString &agentId);

signals:
    // Fetch-style HTTP failures (4xx/5xx) return a response with ok=false and no transport error.
    void completed(QString requestId, QJsonObject response, QString error);

private:
    struct Secret { QString agentId, typeId, value; QStringList origins; };
    struct Request;
    void send(const QSharedPointer<Request> &state);
    void read(const QSharedPointer<Request> &state);
    void received(const QSharedPointer<Request> &state);
    void finish(const QSharedPointer<Request> &state, const QJsonObject &response, const QString &error);
    void invalidateType(const QString &typeId);
    bool containsKnownSecret(const QByteArray &value) const;
    QString substitute(const QString &value, const QHash<QString, Secret> &used, bool url, qsizetype maxChars, bool *overflow) const;

    QPointer<QNetworkAccessManager> m_network;
    NetworkLimits m_limits;
    QHash<QString, CredentialType> m_types;
    QHash<QString, QJsonObject> m_credentials;
    QHash<QString, QStringList> m_allowedTypes;
    QHash<QString, Secret> m_secrets;
    QHash<QString, QHash<QString, QJsonObject>> m_resolved;
    QHash<QString, QSharedPointer<Request>> m_pending;
};
}
