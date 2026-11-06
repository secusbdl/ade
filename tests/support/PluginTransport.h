#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <functional>

namespace ade::test {
// Synthetic transport only: exact original URLs and substituted headers are
// recorded, but no createRequest call delegates to a real network backend.
struct ReplyPlan {
    int status = 200;
    QByteArray body = R"({"fixture":"plugin-runtime-ok"})";
    QByteArray reason = "OK";
    QList<QPair<QByteArray, QByteArray>> headers{{"Content-Type", "application/json"}};
    QUrl redirect;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    bool hold = false;
};

class PluginReply final : public QNetworkReply {
public:
    PluginReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation,
                const ReplyPlan& plan, QObject* parent) : QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); setOperation(operation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        if (!plan.hold) QTimer::singleShot(0, this, [this, plan] { complete(plan); });
    }
    void complete(const ReplyPlan& plan) {
        if (m_done) return;
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, plan.status);
        setAttribute(QNetworkRequest::HttpReasonPhraseAttribute, QString::fromLatin1(plan.reason));
        if (!plan.redirect.isEmpty()) setAttribute(QNetworkRequest::RedirectionTargetAttribute, plan.redirect);
        for (const auto& header : plan.headers) setRawHeader(header.first, header.second);
        m_body = plan.body;
        const QPointer<PluginReply> alive(this);
        if (plan.error != NoError) {
            setError(plan.error, "Synthetic transport error"); emit errorOccurred(plan.error);
            if (!alive || m_done) return;
        }
        if (!m_body.isEmpty()) {
            emit readyRead();
            if (!alive || m_done) return;
        }
        m_done = true; setFinished(true); emit finished();
    }
    void abort() override {
        if (m_done) return;
        m_done = true; setFinished(true); setError(OperationCanceledError, "Synthetic request aborted");
        const QPointer<PluginReply> alive(this);
        emit errorOccurred(OperationCanceledError);
        if (alive) emit finished();
    }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QNetworkReply::bytesAvailable(); }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* destination, qint64 maximum) override {
        const auto count = std::min<qint64>(maximum, m_body.size() - m_offset);
        if (count <= 0) return m_done ? -1 : 0;
        std::memcpy(destination, m_body.constData() + m_offset, size_t(count)); m_offset += count; return count;
    }
private:
    QByteArray m_body;
    qint64 m_offset = 0;
    bool m_done = false;
};

class PluginTransport final : public QNetworkAccessManager {
public:
    struct Request { Operation operation; QNetworkRequest request; QByteArray body; };
    using QNetworkAccessManager::QNetworkAccessManager;
    QList<Request> requests;
    QList<QPointer<PluginReply>> replies;
    ReplyPlan defaultPlan;
    QList<ReplyPlan> plans;
    std::function<ReplyPlan(const Request&)> responder;
protected:
    QNetworkReply* createRequest(Operation operation, const QNetworkRequest& request, QIODevice* outgoingData) override {
        const Request captured{operation, request, outgoingData ? outgoingData->readAll() : QByteArray{}};
        requests.append(captured);
        const auto plan = responder ? responder(captured) : plans.isEmpty() ? defaultPlan : plans.takeFirst();
        auto* reply = new PluginReply(request, operation, plan, this); replies.append(reply); return reply;
    }
};
} // namespace ade::test
