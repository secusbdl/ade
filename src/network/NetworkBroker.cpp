#include "NetworkBroker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QUrl>

namespace ade {
namespace {
struct Origin { QString scheme, host; int port = -1; bool valid = false; };
Origin origin(const QUrl &url) {
    Origin result{url.scheme().toLower(), url.host().toLower(), url.port()};
    result.valid = url.isValid() && !result.host.isEmpty() && url.userInfo().isEmpty()
        && (result.scheme == "https" || result.scheme == "http") && (result.port == -1 || (result.port > 0 && result.port <= 65535));
    if (result.port == -1) result.port = result.scheme == "https" ? 443 : 80;
    return result;
}
bool sameOrigin(const QUrl &a, const QUrl &b) {
    const auto left = origin(a), right = origin(b);
    return left.valid && right.valid && left.scheme == right.scheme && left.host == right.host && left.port == right.port;
}
bool validPattern(const QString &pattern) {
    if (!pattern.contains('*')) {
        const QUrl url(pattern, QUrl::StrictMode);
        return origin(url).valid && (url.path().isEmpty() || url.path() == "/") && !url.hasQuery() && !url.hasFragment();
    }
    static const QRegularExpression regex("^(https?)://([^/?#:@]+)(?::([0-9]{1,5}))?$");
    const auto match = regex.match(pattern);
    if (!match.hasMatch()) return false;
    const auto parts = match.captured(2).split('.');
    if (parts.size() < 2 || parts.contains(QString())) return false;
    static const QRegularExpression label("^[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$");
    for (const auto &part : parts) if (part != "*" && !label.match(QString::fromLatin1(QUrl::toAce(part))).hasMatch()) return false;
    if (!match.captured(3).isEmpty() && (match.captured(3).toInt() < 1 || match.captured(3).toInt() > 65535)) return false;
    return true;
}
bool controlBytes(const QByteArray &value) {
    for (const auto c : value) if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f) return true;
    return false;
}
QString definitionError(const CredentialType &definition) {
    if (!QRegularExpression("^[a-z0-9][a-z0-9-]{0,63}$").match(definition.id).hasMatch()
        || definition.secretFields.isEmpty() || definition.secretFields.size() > 16 || definition.publicFields.size() > 16
        || definition.allowedOrigins.isEmpty() || definition.allowedOrigins.size() > 16)
        return "Invalid credential type definition.";
    QSet<QString> fields;
    for (const auto &field : definition.secretFields + definition.publicFields) {
        if (!QRegularExpression("^[A-Za-z_][A-Za-z0-9_]{0,63}$").match(field).hasMatch() || fields.contains(field))
            return "Credential fields must be valid and unique.";
        fields.insert(field);
    }
    for (const auto &pattern : definition.allowedOrigins) if (!validPattern(pattern)) return "Invalid credential origin pattern.";
    return {};
}
QString credentialError(const CredentialType &type, const QJsonObject &fields) {
    const auto names = type.secretFields + type.publicFields;
    if (fields.size() != names.size()) return "Credential fields do not match the registered type.";
    for (const auto &name : names) {
        const auto value = fields[name];
        if (!value.isString() || value.toString().contains(QChar::Null) || value.toString().size() > 65536
            || (type.secretFields.contains(name) && value.toString().isEmpty())) return "Invalid credential field value.";
    }
    if (QJsonDocument(fields).toJson(QJsonDocument::Compact).size() > 256 * 1024) return "Credential record exceeds its size limit.";
    return {};
}
bool sameDefinition(const CredentialType &a, const CredentialType &b) {
    return a.id == b.id && a.secretFields == b.secretFields && a.publicFields == b.publicFields && a.allowedOrigins == b.allowedOrigins;
}
const QRegularExpression &placeholderRegex() {
    static const QRegularExpression regex("\\{\\{CRED:[^{}]+\\}\\}"); return regex;
}
QList<QByteArray> secretForms(const QString &secret) {
    const auto bytes = secret.toUtf8();
    if (bytes.isEmpty()) return {};
    auto json = QJsonDocument(QJsonArray{secret}).toJson(QJsonDocument::Compact);
    json = json.mid(2, json.size() - 4); // JSON string contents, without [" and "].
    const auto encoded = QUrl::toPercentEncoding(secret);
    return {bytes, encoded, encoded.toLower(), json, bytes.toBase64(), bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)};
}
}

struct NetworkBroker::Request {
    QString id, agent, originalUrl, redirectMode;
    QUrl url;
    QByteArray method, body, response;
    QList<QPair<QByteArray, QByteArray>> headers;
    QStringList credentialTypes;
    QHash<QString, Secret> used;
    QPointer<QNetworkReply> reply;
    QPointer<QTimer> timer;
    int redirects = 0;
    bool hasCredentials = false, redirected = false;
};

NetworkBroker::NetworkBroker(QObject *parent) : NetworkBroker(nullptr, parent) {}
NetworkBroker::NetworkBroker(QNetworkAccessManager *manager, QObject *parent)
    : QObject(parent), m_network(manager ? manager : new QNetworkAccessManager(this)) {
    for (const auto &definition : builtinCredentialTypes()) registerCredentialType(definition);
    connect(m_network, &QObject::destroyed, this, [this] {
        m_network = nullptr;
        const QPointer<NetworkBroker> alive(this);
        for (const auto &state : m_pending.values()) {
            finish(state, {}, "HTTP transport is no longer available.");
            if (!alive) return;
        }
    });
}
NetworkBroker::~NetworkBroker() {
    for (const auto &state : m_pending) {
        if (state->timer) { state->timer->stop(); state->timer->disconnect(this); }
        if (state->reply) { state->reply->disconnect(this); state->reply->abort(); }
    }
}
QList<CredentialType> NetworkBroker::builtinCredentialTypes() {
    // Stagewise 1.31.1 apps/browser/src/shared/credential-types. No model-provider keys.
    return {{"figma-pat", {"token"}, {"https://api.figma.com"}, {}},
            {"github-pat", {"token"}, {"https://api.github.com"}, {}},
            {"posthog-pat", {"personalApiKey"}, {"https://us.posthog.com", "https://eu.posthog.com"}, {}},
            {"supabase-pat", {"accessToken"}, {"https://api.supabase.com"}, {}},
            {"vercel-pat", {"token"}, {"https://api.vercel.com"}, {}}};
}
bool NetworkBroker::originMatches(const QString &urlString, const QString &pattern) {
    if (!validPattern(pattern)) return false;
    const QUrl url(urlString); const auto target = origin(url);
    if (!target.valid) return false;
    if (!pattern.contains('*')) return sameOrigin(url, QUrl(pattern));
    static const QRegularExpression regex("^(https?)://([^/?#:@]+)(?::([0-9]{1,5}))?$");
    const auto match = regex.match(pattern);
    const int port = match.captured(3).isEmpty() ? (match.captured(1) == "https" ? 443 : 80) : match.captured(3).toInt();
    if (target.scheme != match.captured(1) || target.port != port) return false;
    const auto wanted = match.captured(2).split('.'), actual = target.host.split('.');
    if (wanted.size() != actual.size()) return false;
    for (int i = 0; i < wanted.size(); ++i)
        if (wanted[i] != "*" && QString::fromLatin1(QUrl::toAce(wanted[i])).compare(QString::fromLatin1(QUrl::toAce(actual[i])), Qt::CaseInsensitive) != 0) return false;
    return true;
}
bool NetworkBroker::registerCredentialType(const CredentialType &definition, QString *error) {
    if (error) error->clear();
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    const auto invalid = definitionError(definition);
    if (!invalid.isEmpty()) return reject(invalid);
    if (!m_types.contains(definition.id) && m_types.size() >= 64) return reject("Credential type limit reached.");
    const QPointer<NetworkBroker> alive(this);
    removeCredential(definition.id);
    if (!alive) return false;
    m_types[definition.id] = definition; return true;
}
bool NetworkBroker::synchronizeCredential(const CredentialType &definition, const QJsonObject &fields, QString *error) {
    if (error) error->clear();
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    const auto invalidDefinition = definitionError(definition);
    if (!invalidDefinition.isEmpty()) return reject(invalidDefinition);
    if (!m_types.contains(definition.id) && m_types.size() >= 64) return reject("Credential type limit reached.");
    if (!fields.isEmpty()) {
        const auto invalidFields = credentialError(definition, fields);
        if (!invalidFields.isEmpty()) return reject(invalidFields);
    }
    if (m_types.contains(definition.id) && sameDefinition(m_types.value(definition.id), definition)
        && (fields.isEmpty() ? !m_credentials.contains(definition.id) : m_credentials.value(definition.id) == fields)) return true;
    // Publish the complete new type/value before cancellation callbacks. Reentrant
    // synchronization can supersede it; this frame never writes configuration afterward.
    const QString typeId = definition.id;
    m_types[typeId] = definition;
    if (fields.isEmpty()) m_credentials.remove(typeId);
    else m_credentials[typeId] = fields;
    const QPointer<NetworkBroker> alive(this);
    invalidateType(typeId);
    return !alive.isNull();
}
bool NetworkBroker::setCredential(const QString &typeId, const QJsonObject &fields, QString *error) {
    if (error) error->clear();
    auto reject = [&](const QString &message) { if (error) *error = message; return false; };
    if (!m_types.contains(typeId)) return reject("Unknown credential type.");
    const auto invalid = credentialError(m_types[typeId], fields);
    if (!invalid.isEmpty()) return reject(invalid);
    m_credentials[typeId] = fields;
    const QPointer<NetworkBroker> alive(this);
    invalidateType(typeId); return !alive.isNull();
}
void NetworkBroker::removeCredential(const QString &typeId) { m_credentials.remove(typeId); invalidateType(typeId); }
void NetworkBroker::invalidateType(const QString &typeId) {
    for (auto it = m_secrets.begin(); it != m_secrets.end();) { if (it->typeId == typeId) it = m_secrets.erase(it); else ++it; }
    for (auto &resolved : m_resolved) resolved.remove(typeId);
    const QPointer<NetworkBroker> alive(this);
    for (const auto &state : m_pending.values()) if (state->credentialTypes.contains(typeId)) {
        finish(state, {}, "Credential configuration changed; request cancelled.");
        if (!alive) return;
    }
}
void NetworkBroker::setAllowedCredentialTypes(const QString &agentId, const QStringList &typeIds) {
    if (m_allowedTypes.value(agentId) == typeIds) return;
    const QString owner = agentId;
    QSet<QString> removed;
    for (const auto &type : m_allowedTypes.value(owner)) if (!typeIds.contains(type)) removed.insert(type);
    m_allowedTypes[owner] = typeIds;
    if (removed.isEmpty()) return;
    for (auto it = m_secrets.begin(); it != m_secrets.end();) {
        if (it->agentId == owner && removed.contains(it->typeId)) it = m_secrets.erase(it);
        else ++it;
    }
    auto resolved = m_resolved.find(owner);
    if (resolved != m_resolved.end()) {
        for (const auto &type : removed) resolved->remove(type);
        if (resolved->isEmpty()) m_resolved.erase(resolved);
    }
    const QPointer<NetworkBroker> alive(this);
    // Snapshot only requests pending at the policy transition. A callback may
    // re-enable a type and start a new request, which belongs to that newer state.
    const auto pending = m_pending.values();
    for (const auto &state : pending) {
        if (state->agent != owner) continue;
        bool revoked = false;
        for (const auto &type : state->credentialTypes) if (removed.contains(type)) { revoked = true; break; }
        if (revoked) {
            finish(state, {}, "Credential permission changed; request cancelled.");
            if (!alive) return;
        }
    }
}
QJsonValue NetworkBroker::credential(const QString &agentId, const QString &typeId, QString *error) {
    if (error) error->clear();
    if (agentId.isEmpty() || !m_allowedTypes.value(agentId).contains(typeId) || !m_credentials.contains(typeId)) return QJsonValue::Null;
    if (m_resolved.value(agentId).contains(typeId)) return m_resolved[agentId][typeId];
    if (m_secrets.size() + m_types[typeId].secretFields.size() > 4096 || (!m_resolved.contains(agentId) && m_resolved.size() >= 128)) {
        if (error) *error = "Credential handle limit reached.";
        return QJsonValue::Null;
    }
    QJsonObject result; const auto type = m_types[typeId]; const auto data = m_credentials[typeId];
    for (const auto &field : type.publicFields) result[field] = data[field];
    for (const auto &field : type.secretFields) {
        const auto handle = "{{CRED:" + typeId + ':' + field + ':' + QUuid::createUuid().toString(QUuid::WithoutBraces) + "}}";
        result[field] = handle; m_secrets[handle] = {agentId, typeId, data[field].toString(), type.allowedOrigins};
    }
    m_resolved[agentId][typeId] = result; return result;
}
bool NetworkBroker::setLimits(const NetworkLimits &limits) {
    if (!m_pending.isEmpty() || limits.timeoutMs < 10 || limits.timeoutMs > 120000 || limits.maxRequestBytes < 1
        || limits.maxRequestBytes > 16 * 1024 * 1024 || limits.maxResponseBytes < 1 || limits.maxResponseBytes > 16 * 1024 * 1024
        || limits.maxConcurrent < 1 || limits.maxConcurrent > 32 || limits.maxRedirects < 0 || limits.maxRedirects > 10) return false;
    m_limits = limits; return true;
}
bool NetworkBroker::containsKnownSecret(const QByteArray &value) const {
    for (auto it = m_credentials.begin(); it != m_credentials.end(); ++it)
        for (const auto &field : m_types[it.key()].secretFields)
            for (const auto &form : secretForms(it.value()[field].toString())) if (!form.isEmpty() && value.contains(form)) return true;
    return false;
}
QString NetworkBroker::substitute(const QString &value, const QHash<QString, Secret> &used, bool url, qsizetype maxChars, bool *overflow) const {
    *overflow = false;
    QString output; qsizetype position = 0; auto matches = placeholderRegex().globalMatch(value);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto secret = used[match.captured()].value;
        const auto replacement = url ? QString::fromLatin1(QUrl::toPercentEncoding(secret)) : secret;
        const auto literalLength = match.capturedStart() - position;
        if (output.size() + literalLength + replacement.size() > maxChars) { *overflow = true; return {}; }
        output += value.mid(position, literalLength); output += replacement;
        position = match.capturedEnd();
    }
    if (output.size() + value.size() - position > maxChars) { *overflow = true; return {}; }
    return output + value.mid(position);
}

void NetworkBroker::fetch(const QString &id, const QString &agentId, const QJsonObject &request) {
    if (m_pending.contains(id)) return; // Host-generated IDs are idempotent while active.
    auto reject = [&](const QString &error) { emit completed(id, {}, error); };
    if (id.isEmpty() || id.size() > 256 || agentId.isEmpty() || agentId.size() > 256) { reject("Invalid host request identity."); return; }
    if (m_pending.size() >= m_limits.maxConcurrent) { reject("HTTP broker concurrency limit reached."); return; }
    if (!request["url"].isString() || request["url"].toString().size() > 65536) { reject("A bounded HTTP URL is required."); return; }
    auto state = QSharedPointer<Request>::create(); state->id = id; state->agent = agentId; state->originalUrl = request["url"].toString();
    if ((request.contains("method") && !request["method"].isString()) || (request.contains("redirect") && !request["redirect"].isString())) {
        reject("HTTP method and redirect policy must be strings."); return;
    }
    state->method = request["method"].toString("GET").toLatin1().toUpper();
    if (!QRegularExpression("^[!#$%&'*+.^_`|~0-9A-Z-]+$").match(QString::fromLatin1(state->method)).hasMatch()
        || state->method.size() > 32 || state->method == "CONNECT" || state->method == "TRACE" || state->method == "TRACK") {
        reject("Unsupported HTTP method."); return;
    }
    state->redirectMode = request["redirect"].toString("follow");
    if (!QStringList{"follow", "manual", "error"}.contains(state->redirectMode)) { reject("Invalid redirect mode."); return; }
    const auto headers = request["headers"];
    QJsonArray pairs;
    if (headers.isObject()) { const auto object = headers.toObject(); for (auto it = object.begin(); it != object.end(); ++it) pairs.append(QJsonArray{it.key(), it.value()}); }
    else if (headers.isArray()) pairs = headers.toArray();
    else if (!headers.isUndefined() && !headers.isNull()) { reject("Headers must be an object or an array of pairs."); return; }
    if (pairs.size() > 128) { reject("HTTP header count exceeds the limit."); return; }
    qsizetype headerSize = 0;
    const QStringList forbidden{"host", "content-length", "transfer-encoding", "connection", "proxy-authorization", "proxy-connection", "upgrade", "keep-alive", "te", "trailer", "cookie", "cookie2", "accept-encoding"};
    for (const auto &value : pairs) {
        const auto pair = value.toArray();
        if (pair.size() != 2 || !pair[0].isString() || !pair[1].isString()) { reject("Each HTTP header must be a string pair."); return; }
        const auto name = pair[0].toString().toLatin1().toLower(), content = pair[1].toString().toUtf8();
        if (!QRegularExpression("^[!#$%&'*+.^_`|~0-9a-z-]+$").match(QString::fromLatin1(name)).hasMatch()
            || name.size() > 256 || forbidden.contains(QString::fromLatin1(name)) || controlBytes(content)) { reject("Forbidden or malformed HTTP header."); return; }
        headerSize += name.size() + content.size();
        if (headerSize > 65536) { reject("HTTP headers exceed the size limit."); return; }
        state->headers.append({name, content});
    }
    const bool textBody = request["body"].isString();
    if (request.contains("bodyBase64") && !request["body"].isUndefined() && !request["body"].isNull()) { reject("Specify either a text or binary body."); return; }
    if (textBody) {
        if (request["body"].toString().size() > m_limits.maxRequestBytes) { reject("Request body exceeds its byte limit."); return; }
        state->body = request["body"].toString().toUtf8();
    }
    else if (request.contains("bodyBase64")) {
        if (!request["bodyBase64"].isString()) { reject("Binary body must be base64 text."); return; }
        if (request["bodyBase64"].toString().size() > 4 * ((m_limits.maxRequestBytes + 2) / 3)) { reject("Request body exceeds its byte limit."); return; }
        const auto decoded = QByteArray::fromBase64Encoding(request["bodyBase64"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) { reject("Invalid base64 request body."); return; } state->body = decoded.decoded;
    } else if (!request["body"].isUndefined() && !request["body"].isNull()) { reject("Request body must be text or binary base64."); return; }
    if (state->body.size() > m_limits.maxRequestBytes || ((state->method == "GET" || state->method == "HEAD") && !state->body.isEmpty())) {
        reject("Request body exceeds its limit or is invalid for GET/HEAD."); return;
    }
    QStringList strings{state->originalUrl}; for (const auto &header : state->headers) strings.append(QString::fromUtf8(header.second));
    if (textBody) strings.append(request["body"].toString());
    else if (state->body.contains("{{CRED:")) { reject("Credential handles require a text request body."); return; }
    for (const auto &value : strings) {
        auto matches = placeholderRegex().globalMatch(value);
        while (matches.hasNext()) {
            const auto token = matches.next().captured(); const auto found = m_secrets.constFind(token);
            if (found == m_secrets.cend() || found->agentId != agentId || !m_allowedTypes.value(agentId).contains(found->typeId)) {
                reject("Unknown, revoked, or foreign credential handle."); return;
            }
            state->used[token] = *found; if (!state->credentialTypes.contains(found->typeId)) state->credentialTypes.append(found->typeId);
        }
    }
    state->hasCredentials = !state->used.isEmpty();
    QString masked = state->originalUrl;
    for (auto it = state->used.begin(); it != state->used.end(); ++it) masked.replace(it.key(), "ade-credential-handle");
    const QUrl original(masked);
    if (!origin(original).valid || original.hasFragment()) { reject("Only HTTP(S) URLs without user information or fragments are supported."); return; }
    const auto authority = QRegularExpression("^[A-Za-z][A-Za-z0-9+.-]*://([^/?#]*)").match(state->originalUrl).captured(1);
    if (authority.contains("{{CRED:")) { reject("Credential handles cannot appear in URL authority."); return; }
    for (const auto &secret : state->used) {
        bool allowed = false; for (const auto &pattern : secret.origins) if (originMatches(masked, pattern)) { allowed = true; break; }
        if (!allowed) { reject("Credential is not allowed at this destination origin."); return; }
    }
    bool overflow = false;
    state->url = QUrl(substitute(state->originalUrl, state->used, true, 65536, &overflow));
    if (overflow) { reject("Substituted request URL exceeds the limit."); return; }
    if (!sameOrigin(original, state->url)) { reject("Credential substitution must not change the destination origin."); return; }
    headerSize = 0;
    for (auto &header : state->headers) {
        header.second = substitute(QString::fromUtf8(header.second), state->used, false, 65536, &overflow).toUtf8();
        headerSize += header.first.size() + header.second.size();
        if (overflow || controlBytes(header.second) || headerSize > 65536) { reject("Credential substitution produced an invalid HTTP header."); return; }
    }
    if (textBody) state->body = substitute(request["body"].toString(), state->used, false, m_limits.maxRequestBytes, &overflow).toUtf8();
    if (overflow || state->body.size() > m_limits.maxRequestBytes) { reject("Substituted request body exceeds the limit."); return; }
    if (state->hasCredentials) state->redirectMode = "manual";
    bool hasContentType = false; for (const auto &header : state->headers) if (header.first == "content-type") hasContentType = true;
    if (!state->body.isEmpty() && !hasContentType) state->headers.emplaceBack("content-type", textBody ? "text/plain;charset=UTF-8" : "application/octet-stream");
    state->timer = new QTimer(this); state->timer->setSingleShot(true);
    connect(state->timer, &QTimer::timeout, this, [this, state] { finish(state, {}, "HTTP request exceeded its deadline."); });
    m_pending[id] = state; state->timer->start(m_limits.timeoutMs); send(state);
}

void NetworkBroker::send(const QSharedPointer<Request> &state) {
    if (m_pending.value(state->id) != state) return;
    if (!m_network || m_network->thread() != thread()) {
        finish(state, {}, "HTTP transport is unavailable on this thread."); return;
    }
    QNetworkRequest request(state->url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    for (const auto &header : state->headers) {
        if (request.hasRawHeader(header.first)) request.setRawHeader(header.first, request.rawHeader(header.first) + ", " + header.second);
        else request.setRawHeader(header.first, header.second);
    }
    if (!request.hasRawHeader("User-Agent")) request.setRawHeader("User-Agent", "ADE-Native-Sandbox/0.1");
    request.setTransferTimeout(m_limits.timeoutMs); state->response.clear();
    const QPointer<NetworkBroker> alive(this);
    const QPointer<QNetworkAccessManager> manager(m_network);
    QPointer<QNetworkReply> reply = manager->sendCustomRequest(request, state->method, state->body);
    if (!alive || m_pending.value(state->id) != state) {
        if (reply) { reply->abort(); if (reply) reply->deleteLater(); }
        return;
    }
    if (!manager || !reply) { finish(state, {}, "HTTP transport did not return an available reply."); return; }
    state->reply = reply;
    state->reply->setReadBufferSize(m_limits.maxResponseBytes + 1);
    connect(state->reply, &QNetworkReply::readyRead, this, [this, state] { read(state); });
    connect(state->reply, &QNetworkReply::finished, this, [this, state] { received(state); });
    if (state->reply->isFinished()) QTimer::singleShot(0, this, [this, state] { received(state); });
}
void NetworkBroker::read(const QSharedPointer<Request> &state) {
    if (m_pending.value(state->id) != state || !state->reply) return;
    const auto bytes = state->reply->read(m_limits.maxResponseBytes - state->response.size() + 1);
    if (state->response.size() + bytes.size() > m_limits.maxResponseBytes) { finish(state, {}, "HTTP response exceeds its byte limit."); return; }
    state->response += bytes;
}
void NetworkBroker::received(const QSharedPointer<Request> &state) {
    if (m_pending.value(state->id) != state || !state->reply) return;
    read(state); if (!state->reply) return;
    auto reply = state->reply;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const int networkError = int(reply->error());
    if (status == 0 || (networkError > 0 && (networkError < 200 || (networkError >= 300 && networkError < 400)))) {
        finish(state, {}, QString("HTTP transport failed (network error %1).").arg(networkError)); return;
    }
    qsizetype headerSize = 0;
    const auto responseHeaders = reply->rawHeaderPairs();
    for (const auto &header : responseHeaders) headerSize += header.first.size() + header.second.size();
    if (responseHeaders.size() > 128 || headerSize > 65536) { finish(state, {}, "HTTP response headers exceed their limit."); return; }
    const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && !redirect.isEmpty()) {
        if (state->redirectMode == "error") { finish(state, {}, "HTTP redirect was rejected by the request policy."); return; }
        if (state->redirectMode == "follow") {
            if (++state->redirects > m_limits.maxRedirects) { finish(state, {}, "HTTP redirect limit reached."); return; }
            const auto destination = state->url.resolved(redirect);
            if (containsKnownSecret(destination.toString(QUrl::FullyEncoded).toUtf8())) { finish(state, {}, "HTTP redirect reflected protected credential data."); return; }
            if (!origin(destination).valid || destination.hasFragment() || (state->url.scheme() == "https" && destination.scheme() != "https")) {
                finish(state, {}, "HTTP redirect destination is not permitted."); return;
            }
            if (!sameOrigin(state->url, destination)) {
                for (auto it = state->headers.begin(); it != state->headers.end();)
                    if (it->first != "accept" && it->first != "accept-language" && it->first != "content-type") it = state->headers.erase(it); else ++it;
            }
            if ((status == 303 && state->method != "HEAD") || ((status == 301 || status == 302) && state->method == "POST")) {
                state->method = "GET"; state->body.clear();
                for (auto it = state->headers.begin(); it != state->headers.end();) if (it->first.startsWith("content-")) it = state->headers.erase(it); else ++it;
            }
            state->reply = nullptr; reply->disconnect(this); reply->deleteLater(); state->url = destination; state->redirected = true;
            send(state); return;
        }
    }
    QJsonArray headers;
    for (const auto &header : responseHeaders) {
        if (containsKnownSecret(header.first) || containsKnownSecret(header.second)) { finish(state, {}, "HTTP response reflected protected credential data."); return; }
        if (header.first.compare("set-cookie", Qt::CaseInsensitive) == 0 || header.first.compare("set-cookie2", Qt::CaseInsensitive) == 0) continue;
        headers.append(QJsonArray{QString::fromLatin1(header.first), QString::fromLatin1(header.second)});
    }
    const auto statusText = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    if (containsKnownSecret(state->response) || containsKnownSecret(statusText.toUtf8())) { finish(state, {}, "HTTP response reflected protected credential data."); return; }
    const auto exposedUrl = state->hasCredentials ? state->originalUrl : state->url.toString(QUrl::FullyEncoded);
    if (containsKnownSecret(exposedUrl.toUtf8())) { finish(state, {}, "HTTP response URL contained protected credential data."); return; }
    finish(state, {{"status", status}, {"statusText", statusText}, {"headers", headers},
        {"bodyBase64", QString::fromLatin1(state->response.toBase64())}, {"url", exposedUrl},
        {"redirected", state->redirected}, {"ok", status >= 200 && status < 300}}, {});
}
void NetworkBroker::finish(const QSharedPointer<Request> &state, const QJsonObject &response, const QString &error) {
    if (m_pending.value(state->id) != state) return;
    const QPointer<NetworkBroker> alive(this);
    m_pending.remove(state->id);
    if (state->timer) { state->timer->stop(); state->timer->disconnect(this); state->timer->deleteLater(); state->timer = nullptr; }
    if (state->reply) { auto reply = state->reply; state->reply = nullptr; reply->disconnect(this); if (!reply->isFinished()) reply->abort(); if (reply) reply->deleteLater(); }
    if (!alive) return;
    emit completed(state->id, response, error);
}
void NetworkBroker::cancel(const QString &requestId) { if (m_pending.contains(requestId)) finish(m_pending.value(requestId), {}, "HTTP request cancelled."); }
void NetworkBroker::cancelAgent(const QString &agentId) {
    const QPointer<NetworkBroker> alive(this);
    for (const auto &state : m_pending.values()) if (state->agent == agentId) {
        finish(state, {}, "HTTP request cancelled.");
        if (!alive) return;
    }
}
void NetworkBroker::resetAgent(const QString &agentId) {
    m_resolved.remove(agentId);
    for (auto it = m_secrets.begin(); it != m_secrets.end();) { if (it->agentId == agentId) it = m_secrets.erase(it); else ++it; }
    cancelAgent(agentId);
}
}
