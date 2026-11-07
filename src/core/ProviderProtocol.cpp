#include "ProviderProtocol.h"
#include "CustomModelOptions.h"
#include "AgentPresentation.h"
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace ade {
namespace {
QJsonArray nativeContent(const QJsonObject &message, const ProviderConfig &provider) {
    const auto meta = message["ade_native"].toObject();
    if (meta["protocol"].toString() == ProviderProtocol::kind(provider)
        && meta["providerId"].toString() == provider.id && meta["model"].toString() == provider.model)
        return meta["content"].toArray();
    return {};
}
void appendGrouped(QJsonArray &messages, const QString &role, const QJsonArray &content, const char *field) {
    if (!messages.isEmpty() && messages.last().toObject()["role"].toString() == role) {
        auto last = messages.last().toObject();
        auto existing = last[field].toArray();
        for (const auto &part : content) existing.append(part);
        last[field] = existing;
        messages[messages.size() - 1] = last;
    } else messages.append(QJsonObject{{"role", role}, {field, content}});
}
QJsonArray modelContent(const QJsonValue &content, const QString &protocol, const QString &role) {
    QJsonArray source;
    if (content.isString()) { if (!content.toString().isEmpty()) source.append(QJsonObject{{"type", "text"}, {"text", content}}); }
    else source = content.toArray();
    QJsonArray result;
    auto addText = [&](const QString &text) {
        if (protocol == "google") result.append(QJsonObject{{"text", text}});
        else result.append(QJsonObject{{"type", protocol == "responses" ? (role == "assistant" ? "output_text" : "input_text") : "text"}, {"text", text}});
    };
    for (const auto &value : source) {
        const auto part = value.toObject();
        if (part["type"] == "text" && part["text"].isString()) { addText(part["text"].toString()); continue; }
        if (part["type"] != "image") { addText("[Unsupported content part]"); continue; }
        const auto mime = part["mediaType"].toString(); const auto encoded = part["dataBase64"].toString();
        if (role == "assistant" || !QStringList{"image/png", "image/jpeg", "image/webp", "image/gif"}.contains(mime)
            || encoded.isEmpty() || encoded.size() > 44 * 1024 * 1024
            || !QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors)) {
            addText("[Image content could not be serialized for this message]"); continue;
        }
        if (protocol == "anthropic") result.append(QJsonObject{{"type", "image"},
            {"source", QJsonObject{{"type", "base64"}, {"media_type", mime}, {"data", encoded}}}});
        else if (protocol == "google") result.append(QJsonObject{{"inlineData", QJsonObject{{"mimeType", mime}, {"data", encoded}}}});
        else if (protocol == "responses") result.append(QJsonObject{{"type", "input_image"}, {"image_url", "data:" + mime + ";base64," + encoded}});
        else result.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", "data:" + mime + ";base64," + encoded}}}});
    }
    return result;
}
}

QString ProviderProtocol::kind(const ProviderConfig &provider) {
    if (provider.protocol == "responses" || provider.protocol == "openai-responses") return "responses";
    if (provider.protocol == "anthropic") return "anthropic";
    if (provider.protocol == "google" || provider.protocol == "gemini") return "google";
    return "openai";
}
bool ProviderProtocol::supports(const ProviderConfig &provider) {
    return QStringList{"openai", "openai-compatible", "chat-completions", "openai-chat-completions", "responses", "openai-responses", "anthropic", "google", "gemini"}.contains(provider.protocol);
}
void ProviderProtocol::authorize(QNetworkRequest &request, const ProviderConfig &provider) {
    const auto protocol = kind(provider);
    if (protocol == "anthropic") {
        request.setRawHeader("x-api-key", provider.apiKey.toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01"); // Pinned upstream version.
    } else if (protocol == "google") request.setRawHeader("x-goog-api-key", provider.apiKey.toUtf8());
    else if (!provider.apiKey.isEmpty()) request.setRawHeader("Authorization", "Bearer " + provider.apiKey.toUtf8());
}
ProviderRequest ProviderProtocol::prepare(const ProviderConfig &p, const QJsonArray &sourceHistory,
                                           const QString &system, const QJsonArray &sourceTools) {
    const auto tools = CustomModelOptions::allowsTools(p) ? sourceTools : QJsonArray{};
    if (!p.customRequest.isEmpty() && !p.customRequest.value("capabilities").toObject()
            .value("inputModalities").toObject().value("image").toBool()) {
        for (const auto& row : sourceHistory) for (const auto& part : row.toObject().value("content").toArray())
            if (part.toObject().value("type") == "image")
                return {{}, {}, "capabilities.inputModalities.image: this custom model does not declare image input."};
    }
    QJsonArray history;for(const auto& value:sourceHistory)if(!AgentPresentation::isUiOnly(value.toObject()))history.append(value);
    QString base = p.baseUrl;
    while (base.endsWith('/')) base.chop(1);
    const auto protocol = kind(p);
    QJsonObject body;
    QUrl url;
    if (protocol == "responses") {
        url = QUrl(base.endsWith("/responses") || base.endsWith("/agent") ? base : base + "/responses");
        QJsonArray input;
        for (const auto &value : history) {
            const auto message = value.toObject(); const auto role = message["role"].toString();
            if (role == "system") continue;
            if (role == "tool") {
                input.append(QJsonObject{{"type", "function_call_output"}, {"call_id", message["tool_call_id"]}, {"output", message["content"]}});
                continue;
            }
            const auto native = role == "assistant" ? nativeContent(message, p) : QJsonArray{};
            if (!native.isEmpty()) { for (const auto &item : native) input.append(item); continue; }
            const auto content = modelContent(message["content"], protocol, role);
            if (!content.isEmpty()) input.append(QJsonObject{{"role", role}, {"content", content}});
            for (const auto &callValue : message["tool_calls"].toArray()) {
                const auto call = callValue.toObject(); const auto function = call["function"].toObject();
                input.append(QJsonObject{{"type", "function_call"}, {"call_id", call["id"]}, {"name", function["name"]}, {"arguments", function["arguments"]}});
            }
        }
        QJsonArray functions;
        for (const auto &value : tools) {
            auto function = value.toObject()["function"].toObject();
            function["type"] = "function"; function["strict"] = false; functions.append(function);
        }
        body = {{"model", p.model}, {"instructions", system}, {"input", input}, {"stream", true},
                {"store", false}, {"tools", functions}, {"tool_choice", tools.isEmpty() ? "none" : "auto"}};
        if (p.maxOutputTokens > 0) body["max_output_tokens"] = p.maxOutputTokens;
        if (!p.reasoningEffort.isEmpty()) body["reasoning"] = QJsonObject{{"effort", p.reasoningEffort}, {"summary", "auto"}};
        if (url.host() == "api.openai.com") body["include"] = QJsonArray{"reasoning.encrypted_content"};
    } else if (protocol == "anthropic") {
        url = QUrl(base.endsWith("/messages") ? base : base + "/messages");
        QJsonArray messages;
        for (const auto &value : history) {
            const auto message = value.toObject();
            const auto role = message["role"].toString();
            if (role == "system") continue;
            QJsonArray content;
            if (role == "assistant") content = nativeContent(message, p);
            if (content.isEmpty()) {
                if (role == "tool") {
                    content.append(QJsonObject{{"type", "tool_result"}, {"tool_use_id", message["tool_call_id"]},
                        {"content", message["content"]}, {"is_error", message["content"].toString().startsWith("Error:")}});
                } else {
                    content = modelContent(message["content"], protocol, role);
                    for (const auto &callValue : message["tool_calls"].toArray()) {
                        const auto call = callValue.toObject(); const auto function = call["function"].toObject();
                        content.append(QJsonObject{{"type", "tool_use"}, {"id", call["id"]}, {"name", function["name"]},
                            {"input", QJsonDocument::fromJson(function["arguments"].toString().toUtf8()).object()}});
                    }
                }
            }
            appendGrouped(messages, role == "assistant" ? "assistant" : "user", content, "content");
        }
        body = {{"model", p.model}, {"messages", messages}, {"system", system}, {"stream", true},
                {"max_tokens", p.maxOutputTokens > 0 ? p.maxOutputTokens : 8192}};
        if (!tools.isEmpty()) {
            QJsonArray nativeTools;
            for (const auto &value : tools) { const auto fn = value.toObject()["function"].toObject();
                nativeTools.append(QJsonObject{{"name", fn["name"]}, {"description", fn["description"]}, {"input_schema", fn["parameters"]}}); }
            body["tools"] = nativeTools;
        }
        if (!p.thinkingMode.isEmpty()) {
            QJsonObject thinking{{"type", p.thinkingMode}};
            if (p.thinkingMode == "enabled" && p.thinkingBudget > 0) thinking["budget_tokens"] = p.thinkingBudget;
            body["thinking"] = thinking;
        }
        if (!p.reasoningEffort.isEmpty()) body["output_config"] = QJsonObject{{"effort", p.reasoningEffort}};
    } else if (protocol == "google") {
        QString model = p.model;
        if (model.startsWith("models/")) model.remove(0, 7);
        url = QUrl(base + "/models/" + QString::fromLatin1(QUrl::toPercentEncoding(model)) + ":streamGenerateContent");
        QUrlQuery query; query.addQueryItem("alt", "sse"); url.setQuery(query);
        QJsonArray contents;
        QMap<QString, QString> toolNames;
        QSet<QString> nativeCallIds;
        for (const auto &value : history) {
            const auto message = value.toObject(); const auto role = message["role"].toString();
            if (role == "system") continue;
            QJsonArray parts;
            if (role == "assistant") parts = nativeContent(message, p);
            for (const auto &part : parts) {
                const auto id = part.toObject()["functionCall"].toObject()["id"].toString();
                if (!id.isEmpty()) nativeCallIds.insert(id);
            }
            for (const auto &callValue : message["tool_calls"].toArray()) {
                const auto call = callValue.toObject(); toolNames[call["id"].toString()] = call["function"].toObject()["name"].toString();
            }
            if (parts.isEmpty()) {
                if (role == "tool") {
                    QJsonObject response{
                        {"name", toolNames.value(message["tool_call_id"].toString())},
                        {"response", QJsonObject{{"output", message["content"]}}}};
                    if (nativeCallIds.contains(message["tool_call_id"].toString())) response["id"] = message["tool_call_id"];
                    parts.append(QJsonObject{{"functionResponse", response}});
                } else {
                    parts = modelContent(message["content"], protocol, role);
                    for (const auto &callValue : message["tool_calls"].toArray()) {
                        const auto function = callValue.toObject()["function"].toObject();
                        parts.append(QJsonObject{{"functionCall", QJsonObject{{"name", function["name"]},
                            {"args", QJsonDocument::fromJson(function["arguments"].toString().toUtf8()).object()}}}});
                    }
                }
            }
            appendGrouped(contents, role == "assistant" ? "model" : "user", parts, "parts");
        }
        const QJsonObject systemInstruction{{"parts", QJsonArray{QJsonObject{{"text", system}}}}};
        body = {{"contents", contents}, {"systemInstruction", systemInstruction}};
        if (!tools.isEmpty()) {
            QJsonArray declarations;
            for (const auto &value : tools) { const auto fn = value.toObject()["function"].toObject();
                declarations.append(QJsonObject{{"name", fn["name"]}, {"description", fn["description"]}, {"parametersJsonSchema", fn["parameters"]}}); }
            body["tools"] = QJsonArray{QJsonObject{{"functionDeclarations", declarations}}};
        }
        QJsonObject generation;
        if (p.maxOutputTokens > 0) generation["maxOutputTokens"] = p.maxOutputTokens;
        QJsonObject thinking;
        if (!p.reasoningEffort.isEmpty()) thinking["thinkingLevel"] = p.reasoningEffort;
        else if (p.thinkingBudget > 0 || p.thinkingMode == "disabled") thinking["thinkingBudget"] = p.thinkingMode == "disabled" ? 0 : p.thinkingBudget;
        if (!thinking.isEmpty()) { thinking["includeThoughts"] = true; generation["thinkingConfig"] = thinking; }
        if (!generation.isEmpty()) body["generationConfig"] = generation;
    } else {
        url = QUrl(base.endsWith("/chat/completions") ? base : base + "/chat/completions");
        QJsonArray messages{QJsonObject{{"role", "system"}, {"content", system}}};
        for (const auto &value : history) {
            auto message = value.toObject(); message.remove("ade_native"); message.remove("metadata"); message.remove("ade_compressedHistory");
            if (message["content"].isArray()) message["content"] = modelContent(message["content"], protocol, message["role"].toString());
            const auto source = message.take("ade_reasoning_source").toObject();
            if (source["providerId"].toString() != p.id || source["model"].toString() != p.model)
                message.remove("reasoning_content");
            messages.append(message);
        }
        body = {{"model", p.model}, {"messages", messages}, {"stream", true},
                {"stream_options", QJsonObject{{"include_usage", true}}}};
        if (!tools.isEmpty()) { body["tools"] = tools; body["tool_choice"] = "auto"; }
        if (!p.reasoningEffort.isEmpty()) body["reasoning_effort"] = p.reasoningEffort;
        if (!p.thinkingMode.isEmpty()) body["thinking"] = QJsonObject{{"type", p.thinkingMode}};
        if (p.maxOutputTokens > 0) {
            const bool openAiReasoner = QUrl(p.baseUrl).host() == "api.openai.com"
                && (p.model.startsWith("o1") || p.model.startsWith("o3") || p.model.startsWith("o4")
                    || p.model.startsWith("gpt-5") || p.model.startsWith("gpt-6"));
            body[openAiReasoner ? "max_completion_tokens" : "max_tokens"] = p.maxOutputTokens;
        }
    }
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream, application/json");
    request.setRawHeader("User-Agent", "ADE-Native/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setTransferTimeout(90000);
    authorize(request, p);
    QString error;
    if (!CustomModelOptions::apply(p, &body, &request, &error)) return {{}, {}, error};
    return {request, body, {}};
}
}
