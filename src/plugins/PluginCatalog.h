#pragma once
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <memory>

namespace ade {
class NetworkBroker;
struct PluginDefinition {
    QString id, displayName, description, credentialType, secretField;
    bool runtimeReady = false;
    QString pendingReason, upstreamPath;
};
struct PluginContext {
    QString pluginsDirectory;
    QStringList disabledPluginIds, enabledCredentialTypes;
};
// Local bundled metadata, context extraction and the separate encrypted plugin store.
// No service calls. Host-owned preferences/agent identity only; never model-provider keys.
class PluginCatalog {
public:
    static QList<PluginDefinition> definitions();
    static QString materialize(const QString &dataDir, QString *error = nullptr);
    static PluginContext context(const QString &dataDir, const QJsonObject &preferences, QString *error = nullptr);
    bool configureAgent(NetworkBroker &broker, const QString &agentId, const QString &dataDir,
                        const QJsonObject &preferences, QString *error = nullptr);
private:
    struct BrokerRevision { QPointer<NetworkBroker> broker; QByteArray revision; quint64 operation = 0; };
    // A broker cancellation callback may destroy the catalog's owning window.
    // An active configure call keeps only this non-secret cache state alive.
    std::shared_ptr<QList<BrokerRevision>> m_revisions = std::make_shared<QList<BrokerRevision>>();
};
}
