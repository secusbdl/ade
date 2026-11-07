#include "UtilityService.h"
#include "CustomModelOptions.h"
#include "ProviderProtocol.h"
#include "UtilityPrompts.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace ade {
namespace {
QString xml(QString text) { return text.toHtmlEscaped().replace('\'', "&apos;"); }
QString attachmentHints(const QJsonObject &message) {
    QString result;
    const auto attachments = message["metadata"].toObject()["attachments"].toArray();
    for (int i = 0; i < qMin(attachments.size(), qsizetype(32)); ++i) {
        const auto reference = attachments[i].toObject();
        result += " [attachment: " + reference["path"].toString().left(300);
        if (!reference["originalFileName"].toString().isEmpty()) result += " (" + reference["originalFileName"].toString().left(200) + ')';
        result += ']';
    }
    return result;
}
QString titleHistory(const QJsonArray &messages) {
    QStringList lines;
    for (const auto &value : messages) {
        const auto message = value.toObject(); const auto role = message["role"].toString();
        if (role != "user" && role != "assistant") continue;
        auto text = message["content"].toString() + attachmentHints(message); text.replace(QRegularExpression("[\\n\\r]+"), "  ");
        lines.append((role + ": " + text.left(200)).left(500));
    }
    while (lines.size() > 10) lines.removeFirst();
    return "<conversation>\n" + lines.join('\n') + "\n</conversation>";
}
QString compactHistory(const QJsonArray &messages, QString previous) {
    int boundary = 0;
    for (int i = messages.size() - 1; i >= 0; --i) {
        const auto summary = messages[i].toObject()["ade_compressedHistory"].toString();
        if (!summary.isEmpty()) { previous = summary; boundary = i; break; }
    }
    QStringList lines;
    if (!previous.isEmpty()) lines.append("<previous-chat-history>" + xml(previous) + "</previous-chat-history>");
    for (int i = boundary; i < messages.size(); ++i) {
        const auto message = messages[i].toObject(); const auto role = message["role"].toString();
        if (role != "user" && role != "assistant") continue;
        // Provider IDs may repeat in later assistant batches. Pair results only
        // with this assistant's contiguous result block, as the wire does.
        QMap<QString, QString> results, toolAttachments;
        if (role == "assistant") for (int j = i + 1; j < messages.size(); ++j) {
            const auto result = messages[j].toObject();
            if (result["role"] != "tool") break;
            const auto id = result["tool_call_id"].toString();
            results[id] = result["content"].toString();
            toolAttachments[id] = attachmentHints(result);
        }
        QString text = xml(message["content"].toString() + attachmentHints(message));
        for (const auto &value : message["tool_calls"].toArray()) {
            const auto call = value.toObject(); const auto function = call["function"].toObject();
            const auto args = QJsonDocument::fromJson(function["arguments"].toString().toUtf8()).object();
            const auto name = function["name"].toString();
            QString detail = args["path"].toString();
            if (detail.isEmpty()) detail = args["query"].toString(args["pattern"].toString());
            if (name == "runCommand") detail = args["command"].toString().left(500);
            QString outcome = results.value(call["id"].toString());
            if (outcome.startsWith("Error:")) outcome = " " + outcome.section('\n', 0, 0).left(120);
            else if (name == "runCommand") outcome = " " + outcome.left(500);
            else outcome.clear();
            text += "\n[" + xml(name + ": " + detail + outcome) + "]";
            text += xml(toolAttachments.value(call["id"].toString()));
        }
        lines.append("<" + role + ">" + text + "</" + role + ">");
    }
    QString hint = "Your target output size is approximately 30,000 characters.";
    if (!previous.isEmpty()) {
        hint += QString(" The previous briefing is %1 characters (%2% of target).").arg(previous.size()).arg(qRound(previous.size() * 100.0 / 30000));
        if (previous.size() < 18000) hint += " Incorporate it fully and add new content. Do NOT shorten the previous briefing.";
        else if (previous.size() < 25500) hint += " Apply only light condensation to the oldest sections if needed; preserve all recent detail.";
        else hint += " Condense the oldest resolved sections to make room; keep all file paths, decisions and outcomes.";
    }
    return "<chat-history>" + lines.join('\n') + "</chat-history>\n\n" + hint
        + "\n\nWrite the briefing. The agent will read this as its own memory and must be able to continue working without losing context.";
}
QString sanitizeTitle(QString title) {
    title = title.trimmed();
    if ((title.startsWith('"') && title.endsWith('"')) || (title.startsWith('\'') && title.endsWith('\''))) title = title.mid(1, title.size() - 2);
    title.replace(QRegularExpression("^#+\\s*", QRegularExpression::MultilineOption), "");
    title.replace(QRegularExpression("`([^`]*)`"), "\\1");
    title.replace(QRegularExpression("\\*+([^*]+)\\*+"), "\\1");
    title.replace(QRegularExpression("[.…:!?]+$"), "");
    return title.trimmed();
}
}

UtilityService::UtilityService(QObject *parent) : QObject(parent), m_network(this) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (!m_reply) return;
        if (m_timedOut) { cancel(); emit failed("Utility request did not settle after abort; fallback stopped."); return; }
        m_timedOut = true; m_timeout.start(2000); m_reply->abort();
    });
}
UtilityService::~UtilityService() { cancel(); }
void UtilityService::setMainModels(const QList<ModelConfig> &models) { if (!m_busy) m_mainModels = models; }
void UtilityService::setTitleModels(const QList<ModelConfig> &models) { if (!m_busy) m_titleModels = models; }
void UtilityService::setCompressionModels(const QList<ModelConfig> &models) { if (!m_busy) m_compressionModels = models; }
void UtilityService::setBusy(bool busy) { if (m_busy != busy) { m_busy = busy; emit busyChanged(busy); } }
void UtilityService::cancel() {
    ++m_generation; m_timeout.stop();
    if (m_reply) { auto reply = m_reply; m_reply = nullptr; reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    setBusy(false);
}
void UtilityService::generateTitle(const QJsonArray &messages) { start(true, titleHistory(messages)); }
void UtilityService::compressHistory(const QJsonArray &messages, const QString &previous) { start(false, compactHistory(messages, previous)); }
QString UtilityService::redact(QString text) const {
    for (const auto &entry : m_candidates) text = CustomModelOptions::redactDiagnostic(entry.provider, text);
    return text.left(2000);
}
void UtilityService::start(bool title, const QString &prompt) {
    if (m_busy) { emit failed("A utility request is already running."); return; }
    ++m_generation; m_title = title; m_prompt = prompt; m_index = -1; m_error.clear(); m_candidates.clear();
    QSet<QString> seen;
    auto append = [&](const QList<ModelConfig> &models) {
        for (const auto &entry : models) {
            const auto &p = entry.provider;
            const auto key = QString::fromUtf8(QJsonDocument(QJsonArray{p.id, p.model}).toJson(QJsonDocument::Compact));
            if (p.isExternalHarness() || p.model.isEmpty() || seen.contains(key)) continue;
            seen.insert(key); m_candidates.append(entry);
        }
    };
    append(title ? m_titleModels : m_compressionModels); append(m_mainModels);
    if (m_candidates.isEmpty()) { emit failed("No available utility or chat model is configured."); return; }
    if (prompt.size() > 4 * 1024 * 1024) { emit failed("Utility input exceeds the 4 MiB character limit."); return; }
    const QPointer<UtilityService> self(this);const auto generation=m_generation;
    setBusy(true);
    if(self&&m_busy&&generation==m_generation)attempt();
}
void UtilityService::attemptFailed(const QString &error) {
    m_error = redact(error); const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] { if (m_busy && generation == m_generation) attempt(); });
}
void UtilityService::attempt() {
    if (!m_busy) return;
    if (++m_index >= m_candidates.size()) {
        const auto error=m_error.isEmpty()?QString("All utility models failed."):m_error;
        const QPointer<UtilityService> self(this);const auto generation=m_generation;
        setBusy(false);if(self&&generation==m_generation)emit failed(error);return;
    }
    const auto entry = m_candidates[m_index]; auto p = entry.provider;
    const QUrl endpoint(p.baseUrl);
    if (!endpoint.isValid() || endpoint.host().isEmpty() || !endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment()
        || (endpoint.scheme() != "https" && endpoint.scheme() != "http") || !ProviderProtocol::supports(p)) {
        attemptFailed("Utility model has an invalid endpoint or protocol."); return;
    }
    p.maxOutputTokens = m_title ? 100 : 20000;
    if (m_title && !entry.hasThinkingOverride && ProviderProtocol::kind(p) == "anthropic") p.thinkingMode = "disabled";
    if (!p.customRequest.isEmpty()) {
        p.requestOverrides["maxOutputTokens"] = p.maxOutputTokens;
        p.requestOverrides["temperature"] = m_title ? 0.15 : 0.1;
        if (m_title && !entry.hasThinkingOverride && ProviderProtocol::kind(p) == "anthropic")
            p.requestOverrides["thinkingMode"] = "disabled";
    }
    const auto prompt = QString::fromUtf8(m_title ? utilityPrompts::title : utilityPrompts::compression);
    auto prepared = ProviderProtocol::prepare(p, QJsonArray{QJsonObject{{"role", "user"}, {"content", m_prompt}}}, prompt, {});
    if (!prepared.error.isEmpty()) { attemptFailed(prepared.error); return; }
    prepared.request.setRawHeader("Accept", "application/json");
    if (ProviderProtocol::kind(p) == "google") {
        auto url = prepared.request.url(); auto path = url.path(); path.replace(":streamGenerateContent", ":generateContent");
        url.setPath(path); url.setQuery(QString{}); prepared.request.setUrl(url);
        auto generation = prepared.body["generationConfig"].toObject(); if (p.customRequest.isEmpty()) generation["temperature"] = m_title ? 0.15 : 0.1;
        prepared.body["generationConfig"] = generation;
    } else { prepared.body["stream"] = false; prepared.body.remove("stream_options"); if (p.customRequest.isEmpty()) prepared.body["temperature"] = m_title ? 0.15 : 0.1; }
    prepared.request.setTransferTimeout(m_title ? 15000 : 30000);
    m_body.clear(); m_timedOut = false; m_tooLarge = false;
    m_reply = m_network.post(prepared.request, QJsonDocument(prepared.body).toJson(QJsonDocument::Compact));
    const QPointer<QNetworkReply> reply=m_reply;const auto generation=m_generation;
    connect(reply, &QNetworkReply::readyRead, this, [this,reply,generation] {
        if (!reply || m_reply!=reply || !m_busy || generation!=m_generation) return;
        const auto bytes = m_reply->readAll();
        if (m_body.size() + bytes.size() > 4 * 1024 * 1024) { m_tooLarge = true; m_reply->abort(); }
        else m_body += bytes;
    },Qt::QueuedConnection);
    connect(reply, &QNetworkReply::finished, this, [this,reply,generation] {
        if(reply&&m_reply==reply&&m_busy&&generation==m_generation)settleReply();
    },Qt::QueuedConnection);
    m_timeout.start(m_title ? 15000 : 30000);
    emit modelAttempted(p.id, p.model);
}
void UtilityService::settleReply() {
    if (!m_reply || !m_busy) return;
    auto reply = m_reply; m_reply = nullptr; m_timeout.stop();
    m_body += reply->readAll(); const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto networkError = reply->error(); reply->deleteLater();
    if (m_tooLarge || m_body.size() > 4 * 1024 * 1024) { attemptFailed("Utility output exceeded the 4 MiB limit."); return; }
    if (m_timedOut) { attemptFailed("Utility request timed out."); return; }
    QJsonParseError parse; const auto document = QJsonDocument::fromJson(m_body, &parse); const auto object = document.object();
    if (status < 200 || status >= 300 || networkError != QNetworkReply::NoError) {
        attemptFailed(QString("Utility request failed (HTTP %1): %2").arg(status).arg(object["error"].toObject()["message"].toString())); return;
    }
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { attemptFailed("Utility provider returned malformed JSON."); return; }
    if (object["error"].isObject()) { attemptFailed(object["error"].toObject()["message"].toString("Utility provider reported an error.")); return; }
    QString text;
    const auto protocol = ProviderProtocol::kind(m_candidates[m_index].provider);
    if (protocol == "responses") {
        if (object["status"] != "completed") { attemptFailed("Utility response was not completed."); return; }
        for (const auto &item : object["output"].toArray()) for (const auto &part : item.toObject()["content"].toArray())
            if (part.toObject()["type"] == "output_text") text += part.toObject()["text"].toString();
    } else if (protocol == "anthropic") {
        if (object["stop_reason"] == "max_tokens") { attemptFailed("Utility response reached its output limit."); return; }
        for (const auto &part : object["content"].toArray()) if (part.toObject()["type"] == "text") text += part.toObject()["text"].toString();
    } else if (protocol == "google") {
        const auto candidates = object["candidates"].toArray();
        if (candidates.isEmpty()) { attemptFailed("Utility Google response contained no candidate."); return; }
        const auto candidate = candidates.first().toObject();
        if (!candidate["finishReason"].toString().isEmpty() && candidate["finishReason"] != "STOP") { attemptFailed("Utility Google response did not complete normally."); return; }
        for (const auto &part : candidate["content"].toObject()["parts"].toArray()) if (!part.toObject()["thought"].toBool()) text += part.toObject()["text"].toString();
    } else {
        const auto choices = object["choices"].toArray();
        if (choices.isEmpty()) { attemptFailed("Utility response contained no choice."); return; }
        const auto choice = choices.first().toObject();
        if (choice["finish_reason"] == "length" || choice["finish_reason"] == "content_filter") { attemptFailed("Utility response stopped before completion."); return; }
        text = choice["message"].toObject()["content"].toString();
    }
    text = m_title ? sanitizeTitle(text) : text.trimmed();
    if ((m_title && (text.size() < 6 || text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size() < 2))
        || (!m_title && text.size() < 30)) { attemptFailed("Utility response was too short."); return; }
    const bool title = m_title;
    const QPointer<UtilityService> self(this);const auto generation=m_generation;
    setBusy(false);
    if(!self||generation!=m_generation)return;
    if (title) emit titleReady(text); else emit compressionReady(text);
}
int UtilityService::compressionBoundary(const QJsonArray &messages, int contextWindowTokens, int minKeptMessages) {
    const int budget = qMin(qMax(1, contextWindowTokens) / 5, 40000), floor = qMax(5, minKeptMessages);
    int tokens = 0, boundary = messages.size();
    for (int i = messages.size() - 1; i >= 0; --i) {
        const int estimate = (QJsonDocument(messages[i].toObject()).toJson(QJsonDocument::Compact).size() + 403) / 4;
        if (tokens + estimate > budget) { boundary = qMin(i + 1, int(messages.size()) - 1); break; }
        tokens += estimate;
        if (messages.size() - i >= floor) { boundary = i; break; }
        if (i == 0) return -1;
    }
    // Canonical messages separate tool results. Keep a whole user turn so calls/results stay paired.
    while (boundary > 0 && boundary < messages.size() && messages[boundary].toObject()["role"] != "user") --boundary;
    return boundary > 0 && boundary < messages.size() ? boundary : -1;
}
}
