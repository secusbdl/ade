#include "ProviderService.h"
#include "ProviderProtocol.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrlQuery>
#include <memory>

namespace ade {
ProviderService::ProviderService(QObject *parent) : QObject(parent), m_network(this) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        const bool testing = !m_test.isNull();
        cancel();
        if (testing) emit validationFinished(false, "Provider validation timed out.");
        else emit discoveryFailed("Model discovery timed out.");
    });
}
ProviderService::~ProviderService() { cancel(); }
void ProviderService::setBusy(bool busy) { if (m_busy == busy) return; m_busy = busy; emit busyChanged(busy); }
void ProviderService::cancel() {
    m_timeout.stop();
    if (m_reply) { auto reply = m_reply; m_reply = nullptr; reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    if (m_test) { auto test = m_test; m_test = nullptr; test->disconnect(this); test->cancel(); test->deleteLater(); }
    setBusy(false);
}
void ProviderService::discoveryError(QString error) {
    if (!m_provider.apiKey.isEmpty()) error.replace(m_provider.apiKey, "[REDACTED]");
    cancel();
    emit discoveryFailed(error.left(2000));
}
void ProviderService::discover(const ProviderConfig &provider) {
    cancel();
    m_provider = provider;
    const auto url = QUrl(provider.baseUrl);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() || !ProviderProtocol::supports(provider)) {
        emit discoveryFailed("Configure a valid endpoint and supported protocol before discovering models."); return;
    }
    m_models = {}; m_ids.clear(); m_cursors.clear(); m_pages = 0;
    setBusy(true); m_timeout.start(30000); requestPage();
}
void ProviderService::requestPage(const QString &cursor) {
    if (++m_pages > 50) { discoveryError("Model discovery exceeded its 50-page limit."); return; }
    QString base = m_provider.baseUrl;
    while (base.endsWith('/')) base.chop(1);
    for (const auto &suffix : {QString("/chat/completions"), QString("/messages"), QString("/responses"), QString("/agent")})
        if (base.endsWith(suffix)) base.chop(suffix.size());
    QUrl url(base + "/models"); QUrlQuery query;
    const auto protocol = ProviderProtocol::kind(m_provider);
    if (!cursor.isEmpty()) query.addQueryItem(protocol == "google" ? "pageToken" : "after_id", cursor);
    if (protocol == "google") query.addQueryItem("pageSize", "1000");
    if (!query.isEmpty()) url.setQuery(query);
    QNetworkRequest request(url);
    ProviderProtocol::authorize(request, m_provider);
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(10000);
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (m_reply && m_reply->bytesAvailable() > 8 * 1024 * 1024) discoveryError("Model catalog exceeded the 8 MiB limit.");
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, protocol] {
        if (!m_reply || !m_busy) return;
        auto reply = m_reply; m_reply = nullptr;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->readAll(); const auto networkError = reply->error(); reply->deleteLater();
        if (status < 200 || status >= 300 || networkError != QNetworkReply::NoError) {
            discoveryError(QStringLiteral("Model discovery failed (HTTP %1).").arg(status)); return;
        }
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) { discoveryError("Model catalog is not valid JSON."); return; }
        const auto object = document.object(); const auto values = object[protocol == "google" ? "models" : "data"];
        if (!values.isArray()) { discoveryError("Model catalog has an unexpected schema."); return; }
        for (const auto &value : values.toArray()) {
            const auto model = value.toObject();
            QString id = model[protocol == "google" ? "name" : "id"].toString();
            if (id.startsWith("models/")) id.remove(0, 7);
            if (id.isEmpty() || m_ids.contains(id)) continue;
            if (protocol == "google" && model["supportedGenerationMethods"].isArray()
                && !model["supportedGenerationMethods"].toArray().contains("generateContent")) continue;
            static const QRegularExpression nonChat("\\b(embedding|tts|whisper|realtime|moderation|dall-e)\\b", QRegularExpression::CaseInsensitiveOption);
            if ((protocol == "openai" || protocol == "responses") && nonChat.match(id).hasMatch()) continue;
            m_ids.insert(id);
            m_models.append(QJsonObject{{"id", id}, {"name", model[protocol == "google" ? "displayName" : "display_name"].toString(id)},
                {"inputTokenLimit", model["inputTokenLimit"].toInt()}, {"outputTokenLimit", model["outputTokenLimit"].toInt()}, {"metadata", model}});
            if (m_models.size() > 10000) { discoveryError("Model catalog exceeded its entry limit."); return; }
        }
        QString next = protocol == "google" ? object["nextPageToken"].toString()
            : object["has_more"].toBool() ? object["last_id"].toString() : QString();
        if (!next.isEmpty()) {
            if (m_cursors.contains(next)) { discoveryError("Model catalog repeated a pagination cursor."); return; }
            m_cursors.insert(next); requestPage(next); return;
        }
        m_timeout.stop(); setBusy(false); emit modelsReady(m_models);
    });
}
void ProviderService::test(const ProviderConfig &provider) {
    cancel(); m_provider = provider; setBusy(true);
    auto bounded = provider; bounded.maxOutputTokens = 64;
    m_test = new AgentEngine(this); m_test->setProvider(bounded);
    auto failure = std::make_shared<QString>(); auto output = std::make_shared<QString>();
    connect(m_test, &AgentEngine::textDelta, this, [output](const QString &text) { *output += text; });
    connect(m_test, &AgentEngine::failed, this, [failure](const QString &error) { *failure = error; });
    connect(m_test, &AgentEngine::finished, this, [this, failure, output] {
        const bool success = failure->isEmpty() && !output->trimmed().isEmpty();
        const auto detail = success ? QString("Provider returned a model response.") : failure->isEmpty() ? QString("Provider returned no response.") : *failure;
        auto test = m_test; m_test = nullptr; if (test) test->deleteLater();
        m_timeout.stop(); setBusy(false); emit validationFinished(success, detail);
    });
    m_timeout.start(30000);
    m_test->submit("Reply with the single word OK.");
}
}
