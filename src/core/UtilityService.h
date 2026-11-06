#pragma once
#include "ModelConfig.h"
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>

namespace ade {
class UtilityService final : public QObject {
    Q_OBJECT
public:
    explicit UtilityService(QObject *parent = nullptr);
    ~UtilityService() override;
    void setMainModels(const QList<ModelConfig> &models);
    void setTitleModels(const QList<ModelConfig> &models);
    void setCompressionModels(const QList<ModelConfig> &models);
    void generateTitle(const QJsonArray &messages);
    void compressHistory(const QJsonArray &messages, const QString &previousBriefing = {});
    void cancel();
    bool isBusy() const { return m_busy; }
    static int compressionBoundary(const QJsonArray &messages, int contextWindowTokens = 100000, int minKeptMessages = 10);
signals:
    void titleReady(QString title);
    void compressionReady(QString briefing);
    void failed(QString error);
    void busyChanged(bool busy);
    void modelAttempted(QString providerId, QString modelId);
private:
    void start(bool title, const QString &prompt);
    void attempt();
    void settleReply();
    void attemptFailed(const QString &error);
    void setBusy(bool busy);
    QString redact(QString value) const;
    QList<ModelConfig> m_mainModels, m_titleModels, m_compressionModels, m_candidates;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_timeout;
    QByteArray m_body;
    QString m_prompt, m_error;
    int m_index = -1;
    bool m_busy = false, m_title = true, m_timedOut = false, m_tooLarge = false;
    quint64 m_generation = 0;
};
}
