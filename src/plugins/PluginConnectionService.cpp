#include "PluginConnectionService.h"
#include "PluginCatalog.h"
#include "network/NetworkBroker.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>

namespace ade {
namespace {
struct CheckEndpoint { QString url, scope; };
CheckEndpoint endpoint(const QString& id, const QString& region) {
    if (id == "github") return {"https://api.github.com/user", "authenticated user"};
    if (id == "supabase") return {"https://api.supabase.com/v1/projects", "projects_read"};
    if (id == "figma") return {"https://api.figma.com/v1/me", "current_user:read"};
    if (id == "vercel") return {"https://api.vercel.com/v2/user", "user profile"};
    if (id == "posthog" && (region == "us" || region == "eu"))
        return {"https://" + region + ".posthog.com/api/users/@me/", "user:read"};
    return {};
}
bool expectedResponse(const QString& id, const QJsonDocument& doc) {
    if (id == "supabase") return doc.isArray(); // An account may have no projects.
    if (!doc.isObject()) return false;
    const auto object = id == "vercel" ? doc.object()["user"].toObject() : doc.object();
    const auto value = object["id"];
    return (value.isString() && !value.toString().isEmpty()) || value.isDouble();
}
}

PluginConnectionService::PluginConnectionService(QObject* parent)
    : PluginConnectionService(nullptr, parent) {}
PluginConnectionService::PluginConnectionService(QNetworkAccessManager* network, QObject* parent)
    : QObject(parent), m_network(network ? network : new QNetworkAccessManager(this)) {}
PluginConnectionService::~PluginConnectionService() { m_destroying = true; clear(); }
void PluginConnectionService::clear() {
    ++m_epoch;
    auto broker = m_active;
    m_active = nullptr;
    m_requestId = 0;
    if (!broker) return;
    broker->disconnect(this);
    broker->cancel("plugin-check");
    if (broker) broker->deleteLater();
}
void PluginConnectionService::cancel(quint64 requestId) {
    if (requestId == m_requestId) clear();
}
void PluginConnectionService::checkSaved(quint64 requestId, const QString& pluginId,
                                        const QString& dataDirectory, const QString& posthogRegion) {
    if (m_destroying) return;
    const QPointer<PluginConnectionService> alive(this);
    const auto epoch = m_epoch + 1;
    clear();
    if (!alive || m_epoch != epoch) return;
    const auto target = endpoint(pluginId, posthogRegion);
    if (!requestId || target.url.isEmpty()) {
        emit finished(requestId, pluginId, false, "Choose a supported plugin and region.");
        return;
    }
    if (!m_network) {
        emit finished(requestId, pluginId, false, "Connection service is unavailable. Reopen Settings and retry.");
        return;
    }
    auto* broker = new NetworkBroker(m_network, this);
    m_active = broker;
    m_requestId = requestId;
    NetworkLimits limits;
    limits.timeoutMs = 15000;
    limits.maxConcurrent = 1;
    broker->setLimits(limits);
    PluginCatalog catalog;
    QString error;
    if (!catalog.configureAgent(*broker, "settings-plugin-check", dataDirectory, {}, &error)) {
        clear();
        emit finished(requestId, pluginId, false, "Saved plugin credentials could not be loaded. Reopen Settings and check the credential store.");
        return;
    }
    PluginDefinition definition;
    for (const auto& item : PluginCatalog::definitions()) if (item.id == pluginId) definition = item;
    broker->setAllowedCredentialTypes("settings-plugin-check", {definition.credentialType});
    const auto credential = broker->credential("settings-plugin-check", definition.credentialType).toObject();
    const auto handle = credential[definition.secretField].toString();
    if (handle.isEmpty()) {
        clear();
        emit finished(requestId, pluginId, false, "Save a personal access token before checking the connection.");
        return;
    }
    QJsonObject headers{{"Accept", "application/json"}, {"User-Agent", "ADE-plugin-connection-check"}};
    if (pluginId == "figma") headers["X-Figma-Token"] = handle;
    else headers["Authorization"] = "Bearer " + handle;
    if (pluginId == "github") {
        headers["Accept"] = "application/vnd.github+json";
        // Match the pinned Stagewise skill's API contract.
        headers["X-GitHub-Api-Version"] = "2022-11-28";
    }
    connect(broker, &NetworkBroker::completed, this,
            [this, broker, requestId, pluginId, target](const QString&, const QJsonObject& response, const QString& failure) {
        if (m_active != broker || m_requestId != requestId) return;
        bool success = false;
        QString message;
        const int status = response["status"].toInt();
        if (!failure.isEmpty()) message = "Connection check failed or timed out. Check your network and retry.";
        else if (status >= 200 && status < 300) {
            const auto body = QByteArray::fromBase64(response["bodyBase64"].toString().toLatin1());
            success = expectedResponse(pluginId, QJsonDocument::fromJson(body));
            message = success ? "Connection verified for this read endpoint. Other operations depend on the token's permissions."
                              : "The service returned an unexpected response. Connection was not verified.";
        } else if (status == 401) message = "The service rejected this token. Check that it is current and belongs to this service.";
        else if (status == 403) message = "The service denied this check. Check token permissions (" + target.scope + ")"
            + (pluginId == "posthog" ? " and the selected region." : ".");
        else if (status == 429) message = "The service rate limit was reached. Try again later.";
        else if (status >= 300 && status < 400) message = "The service requested a redirect. The token was not forwarded; connection was not verified.";
        else if (status >= 500) message = "The service is unavailable. Try again later.";
        else message = "Connection was not verified (HTTP " + QString::number(status) + "). Check the token's permissions.";
        const auto completionEpoch = m_epoch + 1;
        const QPointer<PluginConnectionService> self(this);
        clear();
        if (!self || m_epoch != completionEpoch) return;
        emit finished(requestId, pluginId, success, message);
    });
    broker->fetch("plugin-check", "settings-plugin-check", {{"url", target.url}, {"method", "GET"},
                  {"headers", headers}, {"redirect", "manual"}});
}
}
