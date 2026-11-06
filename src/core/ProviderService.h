#pragma once
#include "AgentEngine.h"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>

namespace ade {
class ProviderService final : public QObject {
    Q_OBJECT
public:
    explicit ProviderService(QObject *parent = nullptr);
    ~ProviderService() override;
    void discover(const ProviderConfig &provider);
    void test(const ProviderConfig &provider);
    void cancel();
    bool isBusy() const { return m_busy; }
signals:
    void modelsReady(QJsonArray models);
    void discoveryFailed(QString error);
    void validationFinished(bool success, QString detail);
    void busyChanged(bool busy);
private:
    void requestPage(const QString &cursor = {});
    void discoveryError(QString error);
    void setBusy(bool busy);
    ProviderConfig m_provider;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<AgentEngine> m_test;
    QTimer m_timeout;
    QJsonArray m_models;
    QSet<QString> m_ids, m_cursors;
    int m_pages = 0;
    bool m_busy = false;
};
}
