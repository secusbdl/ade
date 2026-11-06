#pragma once
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
namespace ade {
class NetworkBroker;
// Explicit, read-only checks of saved plugin credentials. Results contain only
// fixed local messages; service responses and tokens never reach Settings.
class PluginConnectionService final : public QObject {
    Q_OBJECT
public:
    explicit PluginConnectionService(QObject* parent = nullptr);
    PluginConnectionService(QNetworkAccessManager* network, QObject* parent);
    ~PluginConnectionService() override;
    void checkSaved(quint64 requestId, const QString& pluginId, const QString& dataDirectory,
                    const QString& posthogRegion = QStringLiteral("us"));
    void cancel(quint64 requestId);
signals:
    void finished(quint64 requestId, QString pluginId, bool success, QString safeMessage);
private:
    void clear();
    QPointer<QNetworkAccessManager> m_network;
    QPointer<NetworkBroker> m_active;
    quint64 m_requestId = 0;
    quint64 m_epoch = 0;
    bool m_destroying = false;
};
}
