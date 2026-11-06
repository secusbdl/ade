#include "plugins/PluginCatalog.h"
#include "network/NetworkBroker.h"
#include "core/ProviderStore.h"
#include "core/ContextLoader.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <new>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ade;
namespace ade::pluginDetail { bool writeNewFile(const QString &, const QByteArray &, QString *); }
namespace {
QByteArray read(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
bool write(const QString &path, const QByteArray &value) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(value) == value.size(); }
ProviderConfig credential(const QString &id, const QString &secret = "FAKE_PLUGIN_TEST_TOKEN_7eafc0") {
    ProviderConfig record; record.id = id; record.name = id; record.protocol = "credential"; record.apiKey = secret;
    record.baseUrl = "https://untrusted-stored-origin.invalid"; return record;
}
QJsonObject enabledOnly(const QStringList &ids) {
    QJsonObject plugins;
    for (const auto &definition : PluginCatalog::definitions()) plugins[definition.id] = QJsonObject{{"enabled", ids.contains(definition.id)}};
    return {{"plugins", plugins}};
}
class HeldServer : public QTcpServer {
public:
    int requests = 0;
    HeldServer() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    if (!socket->property("counted").toBool() && bytes.contains("\r\n\r\n")) {
                        socket->setProperty("counted", true); ++requests;
                    }
                });
            }
        });
    }
    QString url() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
};
// Freed storage is protected on the target platform so stale accesses do not
// accidentally pass because a heap allocator retained the destroyed bytes.
template<class T> class ProtectedOwner {
public:
    ProtectedOwner() {
#ifdef Q_OS_WIN
        m_storage = VirtualAlloc(nullptr, sizeof(T), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        m_storage = ::operator new(sizeof(T));
#endif
        if (m_storage) m_object = new (m_storage) T;
    }
    ~ProtectedOwner() {
        if (m_object) m_object->~T();
#ifdef Q_OS_WIN
        if (m_storage) VirtualFree(m_storage, 0, MEM_RELEASE);
#else
        ::operator delete(m_storage);
#endif
    }
    T *get() const { return m_object; }
    bool destroy() {
        if (!m_object) return true;
        m_object->~T(); m_object = nullptr;
#ifdef Q_OS_WIN
        DWORD previous = 0; return VirtualProtect(m_storage, sizeof(T), PAGE_NOACCESS, &previous);
#else
        return true;
#endif
    }
private:
    void *m_storage = nullptr;
    T *m_object = nullptr;
};
struct CatalogOwner { PluginCatalog catalog; QString agentId = "agent-a"; };
}

class PluginsTest : public QObject {
    Q_OBJECT
private slots:
    void resourcesAndContextMatchPinnedMetadata();
    void figmaAssetsArePinnedAndModifiedFilesArePreserved();
    void extractionNeverOverwritesOrIncludesUnexpectedContent();
    void failedInstallPreservesTargetAndCleansTemporaryFile();
    void extractionRejectsJunctionEscapes();
    void encryptedCredentialsAreScopedAndRoutingIsSourceOwned();
    void refreshRevokesHandlesAndMalformedStoreFailsClosed();
    void providerKeysAndMalformedPreferencesAreRejected();
    void cacheDoesNotSurviveBrokerDestruction();
    void cancellationMayDeleteBrokerOrCatalogOwner_data();
    void cancellationMayDeleteBrokerOrCatalogOwner();
};

void PluginsTest::resourcesAndContextMatchPinnedMetadata() {
    QTemporaryDir data; QVERIFY(data.isValid()); QString failure;
    const auto context = PluginCatalog::context(data.path(), {}, &failure);
    QVERIFY2(failure.isEmpty(), qPrintable(failure)); QVERIFY(!context.pluginsDirectory.isEmpty());
    QCOMPARE(context.enabledCredentialTypes.size(), 5); QVERIFY(context.disabledPluginIds.isEmpty());
    const auto provenance = QJsonDocument::fromJson(read(context.pluginsDirectory + "/PROVENANCE.json")).object();
    QCOMPARE(provenance["commit"].toString(), QString("17a0123a0e896885465df36169457199d5225eb5"));
    const auto hashes = provenance["files"].toObject(); QCOMPARE(hashes.size(), 13);
    for (auto it = hashes.begin(); it != hashes.end(); ++it)
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(read(context.pluginsDirectory + '/' + it.key()), QCryptographicHash::Sha256).toHex()), it.value().toString());
    for (const auto &definition : PluginCatalog::definitions()) {
        QVERIFY(definition.runtimeReady); QVERIFY(definition.pendingReason.isEmpty());
        const auto metadata = QJsonDocument::fromJson(read(context.pluginsDirectory + '/' + definition.id + "/metadata.json")).object();
        QCOMPARE(metadata["requiredCredentials"].toArray(), QJsonArray{definition.credentialType});
        QCOMPARE(metadata["displayName"].toString(), definition.displayName);
    }
    QCOMPARE(PluginCatalog::materialize(data.path(), &failure), context.pluginsDirectory); QVERIFY(failure.isEmpty());
    const auto filtered = PluginCatalog::context(data.path(), enabledOnly({"github", "figma"}), &failure);
    QCOMPARE(filtered.enabledCredentialTypes, (QStringList{"github-pat", "figma-pat"})); QVERIFY(!filtered.disabledPluginIds.contains("figma"));
    ContextOptions options; options.pluginsDirectory = filtered.pluginsDirectory; options.disabledPluginIds = filtered.disabledPluginIds;
    ContextLoader loader; const auto snapshot = loader.load(options); QCOMPARE(snapshot.skills.size(), 2);
    QVERIFY(loader.readFile("plugins/github/SKILL.md", &failure).contains("API.getCredential('github-pat')"));
    QVERIFY(failure.isEmpty()); QVERIFY(loader.readFile("plugins/figma/SKILL.md", &failure).contains("API.openApp")); QVERIFY(failure.isEmpty());
    const auto disabled = PluginCatalog::context(data.path(), enabledOnly({"github"}), &failure);
    options.disabledPluginIds = disabled.disabledPluginIds; QCOMPARE(loader.load(options).skills.size(), 1);
    QVERIFY(loader.readFile("plugins/figma/SKILL.md", &failure).isEmpty()); QVERIFY(!failure.isEmpty());
}

void PluginsTest::figmaAssetsArePinnedAndModifiedFilesArePreserved() {
    QTemporaryDir data; QString failure; const auto root = PluginCatalog::materialize(data.path(), &failure);
    QVERIFY2(!root.isEmpty(), qPrintable(failure));
    const auto app = root + "/figma/apps/figma-app/index.html"; const auto original = read(app);
    QVERIFY(!original.isEmpty()); QVERIFY(!read(root + "/figma/apps/figma-app/styles.css").isEmpty());
    const auto provenance = QJsonDocument::fromJson(read(root + "/PROVENANCE.json")).object();
    const auto patch = provenance["patches"].toArray().first().toObject();
    QCOMPARE(patch["file"].toString(), QString("figma/apps/figma-app/index.html"));
    QCOMPARE(patch["packagedSha256"].toString(), QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()));
    QVERIFY(patch["upstreamSha256"].toString() != patch["packagedSha256"].toString());
    // Nested app directories are registered; a second extraction must remain idempotent.
    QCOMPARE(PluginCatalog::materialize(data.path(), &failure), root); QVERIFY(failure.isEmpty());
    QVERIFY(write(app, "user-modified app")); QVERIFY(PluginCatalog::materialize(data.path(), &failure).isEmpty());
    QCOMPARE(read(app), QByteArray("user-modified app")); QVERIFY(!failure.isEmpty());
    QVERIFY(write(app, original)); QVERIFY(write(root + "/figma/apps/figma-app/unregistered.js", "extra app script"));
    QVERIFY(PluginCatalog::materialize(data.path(), &failure).isEmpty()); QVERIFY(failure.contains("Unexpected"));
    QCOMPARE(read(root + "/figma/apps/figma-app/unregistered.js"), QByteArray("extra app script"));
}

void PluginsTest::extractionNeverOverwritesOrIncludesUnexpectedContent() {
    QTemporaryDir data; QString failure; const auto root = PluginCatalog::materialize(data.path(), &failure); QVERIFY2(!root.isEmpty(), qPrintable(failure));
    const auto file = root + "/github/SKILL.md"; const auto original = read(file);
    QVERIFY(write(file, "user-modified file")); QVERIFY(PluginCatalog::materialize(data.path(), &failure).isEmpty());
    QVERIFY(!failure.isEmpty()); QCOMPARE(read(file), QByteArray("user-modified file"));
    QVERIFY(write(file, original)); QVERIFY(QDir().mkpath(root + "/unregistered")); QVERIFY(write(root + "/unregistered/SKILL.md", "unexpected skill"));
    QVERIFY(PluginCatalog::materialize(data.path(), &failure).isEmpty()); QVERIFY(failure.contains("Unexpected"));
    QCOMPARE(read(root + "/unregistered/SKILL.md"), QByteArray("unexpected skill"));
    const auto context = PluginCatalog::context(data.path(), {}, &failure); QVERIFY(context.pluginsDirectory.isEmpty()); QVERIFY(context.enabledCredentialTypes.isEmpty());
}

void PluginsTest::extractionRejectsJunctionEscapes() {
#ifdef Q_OS_WIN
    QTemporaryDir data, outside; const auto link = data.path() + "/bundled-plugins";
    QProcess process; process.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(outside.path())});
    QVERIFY(process.waitForFinished(5000)); QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
    QString failure; const auto extracted = PluginCatalog::materialize(data.path(), &failure);
    const auto outsideFiles = QDir(outside.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    QVERIFY(QDir().rmdir(link));
    QVERIFY(extracted.isEmpty()); QVERIFY(!failure.isEmpty()); QVERIFY(outsideFiles.isEmpty());
#else
    QSKIP("Windows deployment junction fixture.");
#endif
}

void PluginsTest::failedInstallPreservesTargetAndCleansTemporaryFile() {
    QTemporaryDir data; const auto target = data.path() + "/SKILL.md";
    QVERIFY(write(target, "user-owned content")); QString failure;
    QVERIFY(!ade::pluginDetail::writeNewFile(target, "replacement content", &failure)); QVERIFY(!failure.isEmpty());
    QCOMPARE(read(target), QByteArray("user-owned content"));
    QCOMPARE(QDir(data.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"SKILL.md"});
    const auto fresh = data.path() + "/new.md"; failure.clear();
    QVERIFY(ade::pluginDetail::writeNewFile(fresh, "complete bytes", &failure)); QCOMPARE(read(fresh), QByteArray("complete bytes"));
    QCOMPARE(QDir(data.path()).entryList({".ade-plugin-*"}, QDir::Files | QDir::Hidden).size(), 0);
}

void PluginsTest::encryptedCredentialsAreScopedAndRoutingIsSourceOwned() {
    QTemporaryDir data; const auto path = data.path() + "/plugin-credentials.json";
    QString failure; QVERIFY(ProviderStore::save(path, {credential("github"), credential("posthog"), credential("figma")}, &failure));
    QVERIFY(!read(path).contains("FAKE_PLUGIN_TEST_TOKEN"));
    NetworkBroker broker; PluginCatalog catalog;
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), enabledOnly({"github", "figma"}), &failure));
    const auto handle = broker.credential("agent-a", "github-pat").toObject()["token"].toString(); QVERIFY(handle.startsWith("{{CRED:"));
    QVERIFY(broker.credential("agent-a", "posthog-pat").isNull());
    const auto figmaHandle = broker.credential("agent-a", "figma-pat").toObject()["token"].toString(); QVERIFY(figmaHandle.startsWith("{{CRED:"));
    QVERIFY(catalog.configureAgent(broker, "agent-b", data.path(), enabledOnly({"posthog"}), &failure));
    QVERIFY(broker.credential("agent-b", "github-pat").isNull());
    QVERIFY(broker.credential("agent-b", "figma-pat").isNull());
    QVERIFY(broker.credential("agent-b", "posthog-pat").toObject().contains("personalApiKey"));
    QCOMPARE(broker.credential("agent-a", "github-pat").toObject()["token"].toString(), handle);
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), enabledOnly({"github", "figma"}), &failure));
    QCOMPARE(broker.credential("agent-a", "github-pat").toObject()["token"].toString(), handle);
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), enabledOnly({"github"}), &failure));
    QVERIFY(broker.credential("agent-a", "figma-pat").isNull());
    const auto afterScopeChange = broker.credential("agent-a", "github-pat").toObject()["token"].toString();
    QVERIFY(!afterScopeChange.isEmpty()); QVERIFY(afterScopeChange != handle);
    QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("blocked", "agent-a", {{"url", "https://untrusted-stored-origin.invalid/"}, {"headers", QJsonObject{{"Authorization", afterScopeChange}}}});
    QCOMPARE(done.size(), 1); QVERIFY(done[0][2].toString().contains("destination"));
    broker.resetAgent("agent-a");
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), enabledOnly({"github"}), &failure));
    const auto afterReset = broker.credential("agent-a", "github-pat").toObject()["token"].toString();
    QVERIFY(!afterReset.isEmpty()); QVERIFY(afterReset != afterScopeChange);
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), enabledOnly({}), &failure));
    QVERIFY(broker.credential("agent-a", "github-pat").isNull());
    broker.fetch("revoked", "agent-a", {{"url", "https://api.github.com/"}, {"headers", QJsonObject{{"Authorization", handle}}}});
    QCOMPARE(done.size(), 2); QVERIFY(done[1][2].toString().contains("revoked"));
    broker.fetch("disabled", "agent-a", {{"url", "https://api.github.com/"}, {"headers", QJsonObject{{"Authorization", afterReset}}}});
    QCOMPARE(done.size(), 3); QVERIFY(done[2][2].toString().contains("revoked"));
    broker.fetch("figma-disabled", "agent-a", {{"url", "https://api.figma.com/v1/me"}, {"headers", QJsonObject{{"X-Figma-Token", figmaHandle}}}});
    QCOMPARE(done.size(), 4); QVERIFY(done[3][2].toString().contains("revoked"));
}

void PluginsTest::refreshRevokesHandlesAndMalformedStoreFailsClosed() {
    QTemporaryDir data; const auto path = data.path() + "/plugin-credentials.json"; QString failure;
    QVERIFY(ProviderStore::save(path, {credential("github")}, &failure));
    NetworkBroker broker; PluginCatalog catalog; QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    const auto handle = broker.credential("agent-a", "github-pat").toObject()["token"].toString();
    QVERIFY(ProviderStore::save(path, {credential("github", "FAKE_ROTATED_PLUGIN_TOKEN_93abc")}, &failure));
    QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    const auto rotated = broker.credential("agent-a", "github-pat").toObject()["token"].toString(); QVERIFY(rotated != handle);
    QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("old", "agent-a", {{"url", "https://api.github.com/"}, {"headers", QJsonObject{{"Authorization", handle}}}});
    QCOMPARE(done.size(), 1); QVERIFY(done[0][2].toString().contains("revoked"));
    QVERIFY(catalog.configureAgent(broker, "agent-b", data.path(), {}, &failure));
    QVERIFY(!broker.credential("agent-b", "github-pat").isNull());
    QVERIFY(write(path, "{invalid encrypted store")); QVERIFY(!catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    QVERIFY(!failure.isEmpty()); QVERIFY(broker.credential("agent-a", "github-pat").isNull()); QVERIFY(broker.credential("agent-b", "github-pat").isNull());
    QCOMPARE(read(path), QByteArray("{invalid encrypted store"));
    QVERIFY(QFile::remove(path)); QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    QVERIFY(broker.credential("agent-a", "github-pat").isNull());
    QVERIFY(write(path, {})); QVERIFY(!catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure)); QVERIFY(!failure.isEmpty());
}

void PluginsTest::providerKeysAndMalformedPreferencesAreRejected() {
    QTemporaryDir data; QString failure; const auto path = data.path() + "/plugin-credentials.json";
    NetworkBroker broker; PluginCatalog catalog;
    auto model = credential("openai"); model.protocol = "openai-compatible";
    QVERIFY(ProviderStore::save(path, {model}, &failure)); QVERIFY(!catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    QVERIFY(!failure.contains(model.apiKey)); QVERIFY(broker.credential("agent-a", "github-pat").isNull());
    model.id = "github"; QVERIFY(ProviderStore::save(path, {model}, &failure));
    QVERIFY(!catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    QVERIFY(ProviderStore::save(path, {credential("github")}, &failure)); QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
    const QList<QByteArray> invalidPreferences{R"({"plugins":1})", R"({"plugins":{"github":true}})", R"({"plugins":{"github":{"enabled":"true"}}})"};
    for (const auto &encoded : invalidPreferences) {
        const auto preferences = QJsonDocument::fromJson(encoded).object();
        QVERIFY(!catalog.configureAgent(broker, "agent-a", data.path(), preferences, &failure)); QVERIFY(!failure.isEmpty());
        QVERIFY(broker.credential("agent-a", "github-pat").isNull());
        QVERIFY(PluginCatalog::context(data.path(), preferences, &failure).pluginsDirectory.isEmpty());
    }
}

void PluginsTest::cacheDoesNotSurviveBrokerDestruction() {
    QTemporaryDir data; QString failure; QVERIFY(ProviderStore::save(data.path() + "/plugin-credentials.json", {credential("github")}, &failure));
    PluginCatalog catalog; QString prior;
    {
        NetworkBroker broker; QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure));
        prior = broker.credential("agent-a", "github-pat").toObject()["token"].toString(); QVERIFY(!prior.isEmpty());
    }
    NetworkBroker replacement; QVERIFY(catalog.configureAgent(replacement, "agent-a", data.path(), {}, &failure));
    const auto next = replacement.credential("agent-a", "github-pat").toObject()["token"].toString(); QVERIFY(!next.isEmpty()); QVERIFY(next != prior);
}

void PluginsTest::cancellationMayDeleteBrokerOrCatalogOwner_data() {
    QTest::addColumn<QString>("operation"); QTest::addColumn<QString>("destroyed");
    for (const auto &operation : QStringList{"malformed", "reload", "allowlist"})
        for (const auto &destroyed : QStringList{"broker", "catalog", "both"})
            QTest::newRow(qPrintable(operation + '-' + destroyed)) << operation << destroyed;
}
void PluginsTest::cancellationMayDeleteBrokerOrCatalogOwner() {
    QFETCH(QString, operation); QFETCH(QString, destroyed);
    HeldServer server; QVERIFY(server.isListening()); QTemporaryDir data; QString failure;
    const auto store = data.path() + "/plugin-credentials.json";
    QVERIFY(ProviderStore::save(store, {credential("github")}, &failure));
    ProtectedOwner<NetworkBroker> network; ProtectedOwner<CatalogOwner> owner;
    QVERIFY(network.get()); QVERIFY(owner.get()); const QPointer<NetworkBroker> alive(network.get());
    QVERIFY(owner.get()->catalog.configureAgent(*network.get(), owner.get()->agentId, data.path(), {}, &failure));
    QJsonObject request{{"url", server.url()}};
    if (operation == "reload") {
        // Fixture-only origin override: no real provider/plugin destination or
        // credential is contacted. Reload must restore the source-owned registry.
        QVERIFY(network.get()->registerCredentialType({"github-pat", {"token"}, {server.url()}, {}}, &failure));
        QVERIFY(network.get()->setCredential("github-pat", {{"token", "FAKE_LOOPBACK_CATALOG_TOKEN_93abc"}}, &failure));
        const auto handle = network.get()->credential("agent-a", "github-pat").toObject()["token"].toString();
        QVERIFY(!handle.isEmpty()); request["headers"] = QJsonObject{{"Authorization", handle}};
        QVERIFY(ProviderStore::save(store, {credential("github", "FAKE_ROTATED_PLUGIN_TOKEN_93abc")}, &failure));
    }
    network.get()->fetch("held", "agent-a", request); QTRY_COMPARE(server.requests, 1);
    int completions = 0;
    connect(network.get(), &NetworkBroker::completed, this, [&](const QString &, const QJsonObject &, const QString &reason) {
        ++completions; QVERIFY(reason.contains("cancelled"));
        if (destroyed != "catalog") QVERIFY(network.destroy());
        if (destroyed != "broker") QVERIFY(owner.destroy());
    });
    const auto preferences = operation == "malformed" ? QJsonObject{{"plugins", 7}}
        : operation == "allowlist" ? enabledOnly({}) : QJsonObject{};
    const bool configured = owner.get()->catalog.configureAgent(*network.get(), owner.get()->agentId, data.path(), preferences, &failure);
    QCOMPARE(completions, 1);
    if (destroyed != "catalog") { QVERIFY(alive.isNull()); QVERIFY(!configured); }
    else {
        QVERIFY(alive); QCOMPARE(configured, operation != "malformed");
        if (operation != "reload") QVERIFY(network.get()->credential("agent-a", "github-pat").isNull());
    }
    QCoreApplication::processEvents(); QCOMPARE(completions, 1);
}
QTEST_GUILESS_MAIN(PluginsTest)
#include "test_plugins.moc"
