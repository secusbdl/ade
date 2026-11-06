#include "PluginCatalog.h"
#include "core/ProviderStore.h"
#include "core/ToolExecutor.h"
#include "network/NetworkBroker.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QResource>
#include <QSet>
#include <QTemporaryFile>
#include <algorithm>

static void initializePluginResources() {
    static const bool initialized = [] { Q_INIT_RESOURCE(ade_plugins); return true; }();
    Q_UNUSED(initialized)
}

namespace ade {
namespace pluginDetail {
// Kept outside the catalog to regression-test rename failure without disk-full fault injection.
bool writeNewFile(const QString &path, const QByteArray &bytes, QString *error) {
    QTemporaryFile file(QFileInfo(path).absolutePath() + "/.ade-plugin-XXXXXX");
    if (!file.open() || file.write(bytes) != bytes.size() || !file.flush()) {
        *error = "Cannot write a complete bundled plugin temporary file."; return false;
    }
    file.close();
    // QFile::rename refuses an existing destination. QTemporaryFile removes its own
    // temporary path on failure; a failed write never poisons the final cache path.
    if (!file.rename(path)) { *error = "Cannot install bundled plugin file without replacing existing content."; return false; }
    file.setAutoRemove(false);
    return true;
}
}
namespace {
constexpr auto Commit = "17a0123a0e896885465df36169457199d5225eb5";
QByteArray readBounded(const QString &path, qint64 maxBytes, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = "Cannot read a plugin configuration or bundled file."; return {}; }
    if (file.size() > maxBytes) { *error = "Plugin configuration or bundled file exceeds its size limit."; return {}; }
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes || file.error() != QFileDevice::NoError) { *error = "Cannot read a bounded plugin configuration or bundled file."; return {}; }
    return bytes;
}
QString checkedPath(ToolExecutor &scope, const QString &relative, QString *error) {
    const auto path = scope.resolvePath(relative, error);
    if (path.isEmpty()) return {};
    QString current = scope.workspace();
    for (const auto &part : relative.split('/')) {
        current += '/' + part; const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) { *error = "Managed plugin files cannot be reached through links or junctions."; return {}; }
    }
    return path;
}
bool openScope(const QString &dataDir, bool create, ToolExecutor &scope, QString *error) {
    if (dataDir.isEmpty() || dataDir.contains(QChar::Null)) { *error = "A local plugin data directory is required."; return false; }
    if (create && !QDir().mkpath(dataDir)) { *error = "Cannot create the plugin data directory."; return false; }
    if (!scope.setWorkspace(dataDir)) { *error = "Plugin data directory is unavailable."; return false; }
    return true;
}
bool enabledPlugins(const QJsonObject &preferences, QStringList *enabled, QString *error) {
    const auto value = preferences["plugins"];
    if (!value.isUndefined() && !value.isObject()) { *error = "Plugin preferences must be an object."; return false; }
    const auto plugins = value.toObject();
    for (const auto &definition : PluginCatalog::definitions()) {
        const auto entry = plugins[definition.id];
        if (!entry.isUndefined() && !entry.isObject()) { *error = "Plugin preference entries must be objects."; return false; }
        const auto flag = entry.toObject().value("enabled");
        if (!flag.isUndefined() && !flag.isBool()) { *error = "Plugin enabled preferences must be boolean."; return false; }
        // Upstream plugins-section.tsx enables plugins unless explicitly disabled.
        if (definition.runtimeReady && flag.toBool(true)) enabled->append(definition.id);
    }
    return true;
}
QMap<QString, QByteArray> bundledFiles(QString *error) {
    initializePluginResources();
    QStringList names{"LICENSE", "PROVENANCE.json"};
    for (const auto &definition : PluginCatalog::definitions()) if (definition.runtimeReady)
        names << definition.id + "/SKILL.md" << definition.id + "/metadata.json";
    names << "figma/apps/figma-app/index.html" << "figma/apps/figma-app/styles.css";
    QMap<QString, QByteArray> result;
    for (const auto &name : names) {
        const auto bytes = readBounded(":/ade/plugins/" + name, 1024 * 1024, error);
        if (!error->isEmpty()) return {};
        result[name] = bytes;
    }
    const auto provenance = QJsonDocument::fromJson(result["PROVENANCE.json"]).object();
    const auto hashes = provenance["files"].toObject();
    if (provenance["commit"].toString() != Commit || provenance["version"].toString() != "1.31.1" || hashes.size() != result.size() - 1) {
        *error = "Bundled plugin provenance is invalid."; return {};
    }
    for (auto it = result.cbegin(); it != result.cend(); ++it) if (it.key() != "PROVENANCE.json") {
        const auto hash = QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex();
        if (hashes[it.key()].toString().toLatin1() != hash) { *error = "Bundled plugin provenance hash does not match its resource."; return {}; }
    }
    return result;
}
}

QList<PluginDefinition> PluginCatalog::definitions() {
    initializePluginResources();
    QList<PluginDefinition> result;
    const QList<QPair<QString, QString>> ready{{"github", "token"}, {"posthog", "personalApiKey"}, {"supabase", "accessToken"}, {"vercel", "token"}, {"figma", "token"}};
    for (const auto &entry : ready) {
        QString failure; const auto metadata = QJsonDocument::fromJson(readBounded(":/ade/plugins/" + entry.first + "/metadata.json", 65536, &failure)).object();
        result.append({entry.first, metadata["displayName"].toString(), metadata["description"].toString(), entry.first + "-pat", entry.second,
            true, {}, "apps/browser/bundled/plugins/" + entry.first + "/SKILL.md"});
    }
    return result;
}

QString PluginCatalog::materialize(const QString &dataDir, QString *error) {
    QString failure; if (error) error->clear();
    auto reject = [&](const QString &message) { if (error) *error = message; return QString(); };
    const auto files = bundledFiles(&failure);
    if (!failure.isEmpty()) return reject(failure);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    for (auto it = files.cbegin(); it != files.cend(); ++it) { digest.addData(it.key().toUtf8()); digest.addData(QByteArray(1, '\0')); digest.addData(it.value()); digest.addData(QByteArray(1, '\0')); }
    const auto relativeRoot = "bundled-plugins/stagewise-1.31.1-" + QString::fromLatin1(digest.result().toHex());
    ToolExecutor scope;
    if (!openScope(dataDir, true, scope, &failure)) return reject(failure);
    const auto root = checkedPath(scope, relativeRoot, &failure);
    if (root.isEmpty()) return reject(failure);
    QSet<QString> directories;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        auto parent = it.key();
        while (parent.contains('/')) {
            parent = parent.left(parent.lastIndexOf('/'));
            directories.insert(parent);
        }
    }
    if (QFileInfo::exists(root)) {
        if (!QFileInfo(root).isDir()) return reject("Bundled plugin destination is not a directory.");
        QDirIterator entries(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        int count = 0;
        while (entries.hasNext()) {
            const auto path = entries.next(); const auto relative = QDir(root).relativeFilePath(path);
            if (++count > 64 || checkedPath(scope, relativeRoot + '/' + relative, &failure).isEmpty())
                return reject(failure.isEmpty() ? "Unexpected files in the bundled plugin directory." : failure);
            const auto info = entries.fileInfo();
            if ((info.isDir() && !directories.contains(relative)) || (!info.isDir() && !files.contains(relative)))
                return reject("Unexpected files in the bundled plugin directory; nothing was overwritten.");
        }
    }
    // Check every existing file before creating any missing file. Modified copies are never replaced.
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto path = checkedPath(scope, relativeRoot + '/' + it.key(), &failure);
        if (path.isEmpty()) return reject(failure);
        if (QFileInfo::exists(path)) {
            if (!QFileInfo(path).isFile()) return reject("Bundled plugin file path is not a file.");
            const auto existing = readBounded(path, it.value().size(), &failure);
            if (!failure.isEmpty() || existing != it.value()) return reject("Bundled plugin extraction differs from its pinned resource; nothing was overwritten.");
        }
    }
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const auto path = checkedPath(scope, relativeRoot + '/' + it.key(), &failure);
        if (path.isEmpty()) return reject(failure);
        if (QFileInfo::exists(path)) continue;
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) return reject("Cannot create the bundled plugin directory.");
        if (checkedPath(scope, relativeRoot + '/' + it.key(), &failure).isEmpty()) return reject(failure);
        if (!pluginDetail::writeNewFile(path, it.value(), &failure)) return reject(failure);
    }
    return root;
}

PluginContext PluginCatalog::context(const QString &dataDir, const QJsonObject &preferences, QString *error) {
    if (error) error->clear();
    PluginContext result; QStringList enabled; QString failure;
    if (!enabledPlugins(preferences, &enabled, &failure)) { if (error) *error = failure; return result; }
    for (const auto &definition : definitions()) {
        if (!enabled.contains(definition.id)) result.disabledPluginIds.append(definition.id);
        else result.enabledCredentialTypes.append(definition.credentialType);
    }
    result.pluginsDirectory = materialize(dataDir, &failure);
    if (!failure.isEmpty()) { result.enabledCredentialTypes.clear(); if (error) *error = failure; }
    return result;
}

bool PluginCatalog::configureAgent(NetworkBroker &broker, const QString &agentId, const QString &dataDir,
                                   const QJsonObject &preferences, QString *error) {
    if (error) error->clear();
    // Broker mutations can synchronously cancel requests. Their observers may
    // destroy the broker, this catalog's owner, or both. Do not access this or
    // caller-owned references after the first mutation callback boundary.
    const auto revisions = m_revisions;
    const QPointer<NetworkBroker> alive(&broker);
    auto *const identity = &broker;
    const QString hostAgentId = agentId;
    QString failure; QStringList enabled;
    auto forget = [revisions, identity] { revisions->erase(std::remove_if(revisions->begin(), revisions->end(), [identity](const BrokerRevision &entry) { return entry.broker.isNull() || entry.broker == identity; }), revisions->end()); };
    auto brokerAvailable = [&] {
        if (alive) return true;
        forget(); if (error) *error = "Plugin network broker was destroyed during configuration."; return false;
    };
    auto reject = [&](const QString &message) {
        forget(); if (error) *error = message;
        if (!brokerAvailable()) return false;
        alive->setAllowedCredentialTypes(hostAgentId, {});
        if (!brokerAvailable()) return false;
        for (const auto &definition : definitions()) {
            alive->removeCredential(definition.credentialType);
            if (!brokerAvailable()) return false;
        }
        return false;
    };
    if (hostAgentId.isEmpty() || hostAgentId.size() > 256) return reject("A valid host agent identity is required for plugin configuration.");
    if (!enabledPlugins(preferences, &enabled, &failure)) return reject(failure);
    ToolExecutor scope;
    if (!openScope(dataDir, false, scope, &failure)) return reject(failure);
    const auto path = checkedPath(scope, "plugin-credentials.json", &failure);
    if (path.isEmpty()) return reject(failure);
    QByteArray bytes; const bool existed = QFileInfo::exists(path);
    if (existed) bytes = readBounded(path, 4 * 1024 * 1024, &failure);
    if (!failure.isEmpty()) return reject(failure);
    const auto revision = QCryptographicHash::hash(path.toUtf8() + (existed ? "\1" : "\0") + bytes, QCryptographicHash::Sha256);
    revisions->erase(std::remove_if(revisions->begin(), revisions->end(), [](const BrokerRevision &entry) { return entry.broker.isNull(); }), revisions->end());
    const bool alreadyLoaded = std::any_of(revisions->cbegin(), revisions->cend(), [&](const BrokerRevision &entry) { return entry.broker == identity && entry.revision == revision; });
    if (!alreadyLoaded) {
        const auto records = ProviderStore::load(path, &failure);
        if (!failure.isEmpty()) return reject("Encrypted plugin credentials could not be loaded: " + failure);
        QMap<QString, QJsonObject> credentials;
        const auto registry = definitions();
        for (const auto &record : records) {
            const auto definition = std::find_if(registry.cbegin(), registry.cend(), [&](const PluginDefinition &item) { return item.id == record.id; });
            if (definition == registry.cend() || record.protocol != "credential" || record.apiKey.size() > 65536 || record.apiKey.contains(QChar::Null))
                return reject("Plugin credential records must use registered plugin IDs and the credential protocol.");
            if (definition->runtimeReady && !record.apiKey.isEmpty()) credentials[definition->credentialType] = {{definition->secretField, record.apiKey}};
        }
        QByteArray after; const bool existsAfter = QFileInfo::exists(path);
        if (existsAfter) after = readBounded(path, 4 * 1024 * 1024, &failure);
        if (!failure.isEmpty() || bytes != after || existed != existsAfter) return reject("Plugin credentials changed during loading; retry after the save completes.");
        // Restore the host's exact source registry; stored base URLs are never authority for secret routing.
        for (const auto &definition : NetworkBroker::builtinCredentialTypes()) {
            const bool registered = alive->registerCredentialType(definition, &failure);
            if (!brokerAvailable()) return false;
            if (!registered) return reject(failure);
        }
        for (auto it = credentials.cbegin(); it != credentials.cend(); ++it) {
            const bool configured = alive->setCredential(it.key(), it.value(), &failure);
            if (!brokerAvailable()) return false;
            if (!configured) return reject("Plugin credential fields could not be configured.");
        }
        forget(); revisions->append({alive, revision});
    }
    QStringList allowed;
    for (const auto &definition : definitions()) if (enabled.contains(definition.id)) allowed.append(definition.credentialType);
    alive->setAllowedCredentialTypes(hostAgentId, allowed);
    return brokerAvailable();
}
}
