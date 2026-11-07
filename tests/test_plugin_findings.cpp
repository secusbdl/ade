#include "core/ProviderStore.h"
#include "network/NetworkBroker.h"
#include "plugins/PluginCatalog.h"
#include "support/PluginTransport.h"
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ade;
using namespace ade::test;

namespace {
ProviderConfig credential(const QString &id, const QString &value) {
    ProviderConfig record;
    record.id = id;
    record.name = id;
    record.protocol = "credential";
    record.apiKey = value;
    record.baseUrl = "https://not-routing-authority.invalid";
    return record;
}
QString tokenHandle(NetworkBroker &broker, const QString &agent, const QString &type) {
    return broker.credential(agent, type).toObject().value("token").toString();
}
QJsonObject request(const QString &url, const QString &handle = {}) {
    QJsonObject value{{"url", url}};
    if (!handle.isEmpty()) value["headers"] = QJsonObject{{"Authorization", "Bearer " + handle}};
    return value;
}
QString failureFor(const QSignalSpy &done, const QString &id) {
    for (const auto &row : done) if (row[0].toString() == id) return row[2].toString();
    return {};
}
bool completed(const QSignalSpy &done, const QString &id) {
    for (const auto &row : done) if (row[0].toString() == id) return true;
    return false;
}
void finishHeld(PluginTransport &transport) {
    const auto replies = transport.replies;
    for (const auto &reply : replies) if (reply && !reply->isFinished()) reply->complete(ReplyPlan{});
}
}

class PluginFindingsTest final : public QObject {
    Q_OBJECT
private slots:
    void disablingAllTypesReleasesResolvedAgentCapacity() {
        PluginTransport transport;
        NetworkBroker broker(&transport, nullptr);
        QVERIFY(broker.setCredential("github-pat", {{"token", "FAKE_CAPACITY_3917"}}));
        for (int index = 0; index < 160; ++index) {
            const auto agent = "agent-" + QString::number(index);
            broker.setAllowedCredentialTypes(agent, {"github-pat"});
            QString failure;
            QVERIFY2(!broker.credential(agent, "github-pat", &failure).isNull(), qPrintable(failure));
            broker.setAllowedCredentialTypes(agent, {});
        }
    }
    void nestedCatalogRefreshKeepsNewestStoreRevision() {
        QTemporaryDir data;
        const auto path = data.filePath("plugin-credentials.json");
        auto github = credential("github", "FAKE_NESTED_GITHUB_1");
        auto vercel = credential("vercel", "FAKE_NESTED_VERCEL_1");
        QVERIFY(ProviderStore::save(path, {github, vercel}));
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        PluginCatalog catalog;
        QVERIFY(catalog.configureAgent(broker, "agent", data.path(), {}));
        const auto handle = tokenHandle(broker, "agent", "github-pat");
        broker.fetch("old", "agent", request("https://api.github.com/user", handle));
        bool nested = false;
        connect(&broker, &NetworkBroker::completed, this, [&](const QString &id, const QJsonObject &, const QString &) {
            if (id != "old") return;
            nested = true;
            vercel.apiKey = "FAKE_NESTED_VERCEL_NEWEST";
            QVERIFY(ProviderStore::save(path, {github, vercel}));
            QVERIFY(catalog.configureAgent(broker, "agent", data.path(), {}));
        });
        github.apiKey = "FAKE_NESTED_GITHUB_2";
        QVERIFY(ProviderStore::save(path, {github, vercel}));
        QString failure;
        catalog.configureAgent(broker, "agent", data.path(), {}, &failure);
        QVERIFY(nested);
        const auto current = tokenHandle(broker, "agent", "vercel-pat");
        broker.fetch("current", "agent", request("https://api.vercel.com/v2/user", current));
        QCOMPARE(transport.requests.last().request.rawHeader("Authorization"), QByteArray("Bearer FAKE_NESTED_VERCEL_NEWEST"));
    }
    void synchronizedDefinitionRestoresAuthorityAndRejectsInvalidDataAtomically() {
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        const CredentialType original{"github-pat", {"token"}, {"https://api.github.com"}, {}};
        QVERIFY(broker.synchronizeCredential(original, {{"token", "FAKE_SYNC_GITHUB_1"}}));
        broker.setAllowedCredentialTypes("agent", {"github-pat"});
        const auto old = tokenHandle(broker, "agent", "github-pat");
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("old", "agent", request("https://api.github.com/user", old));
        QString failure;
        QVERIFY(!broker.synchronizeCredential(original, {{"invalid", "FAKE_IGNORED"}}, &failure));
        QVERIFY(!failure.isEmpty());
        QCOMPARE(tokenHandle(broker, "agent", "github-pat"), old);
        QCOMPARE(done.size(), 0);
        QVERIFY(broker.synchronizeCredential({"github-pat", {"token"}, {"https://fixture.invalid"}, {}}, {{"token", "FAKE_SYNC_GITHUB_1"}}));
        QVERIFY(completed(done, "old"));
        const auto alternate = tokenHandle(broker, "agent", "github-pat");
        broker.fetch("alternate", "agent", request("https://fixture.invalid/user", alternate));
        QVERIFY(broker.synchronizeCredential(original, {{"token", "FAKE_SYNC_GITHUB_1"}}));
        QVERIFY(completed(done, "alternate"));
        const auto restored = tokenHandle(broker, "agent", "github-pat");
        broker.fetch("rejected-origin", "agent", request("https://fixture.invalid/user", restored));
        QVERIFY(failureFor(done, "rejected-origin").contains("destination"));
        QCOMPARE(transport.requests.size(), 2);
    }
    void synchronizationAndAllowlistCallbacksMayReplaceOrDeleteOwner_data() {
        QTest::addColumn<bool>("synchronize");
        QTest::addColumn<bool>("destroy");
        QTest::newRow("synchronize-delete") << true << true;
        QTest::newRow("synchronize-replace") << true << false;
        QTest::newRow("allowlist-delete") << false << true;
        QTest::newRow("allowlist-reenable") << false << false;
    }
    void synchronizationAndAllowlistCallbacksMayReplaceOrDeleteOwner() {
        QFETCH(bool, synchronize);
        QFETCH(bool, destroy);
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        QPointer<NetworkBroker> broker = new NetworkBroker(&transport, nullptr);
        const auto cleanup = qScopeGuard([&] { delete broker.data(); });
        const CredentialType type{"github-pat", {"token"}, {"https://api.github.com"}, {}};
        QVERIFY(broker->synchronizeCredential(type, {{"token", "FAKE_REENTRY_1"}}));
        broker->setAllowedCredentialTypes("agent", {"github-pat"});
        const auto handle = tokenHandle(*broker, "agent", "github-pat");
        QSignalSpy done(broker, &NetworkBroker::completed);
        broker->fetch("old-a", "agent", request("https://api.github.com/user", handle));
        broker->fetch("old-b", "agent", request("https://api.github.com/user", handle));
        bool acted = false;
        connect(broker, &NetworkBroker::completed, this, [&](const QString &, const QJsonObject &, const QString &) {
            if (acted) return;
            acted = true;
            if (destroy) { delete broker.data(); return; }
            if (synchronize) QVERIFY(broker->synchronizeCredential(type, {{"token", "FAKE_REENTRY_NEWEST"}}));
            else broker->setAllowedCredentialTypes("agent", {"github-pat"});
            const auto replacement = tokenHandle(*broker, "agent", "github-pat");
            broker->fetch("replacement", "agent", request("https://api.github.com/user", replacement));
        });
        if (synchronize) QCOMPARE(broker->synchronizeCredential(type, {{"token", "FAKE_REENTRY_2"}}), !destroy);
        else broker->setAllowedCredentialTypes("agent", {});
        QVERIFY(acted);
        if (destroy) { QVERIFY(broker.isNull()); QCOMPARE(done.size(), 1); return; }
        QVERIFY(broker);
        QVERIFY(!completed(done, "replacement"));
        if (synchronize) QCOMPARE(transport.requests.last().request.rawHeader("Authorization"), QByteArray("Bearer FAKE_REENTRY_NEWEST"));
        finishHeld(transport);
        QTRY_COMPARE(done.size(), 3);
        QVERIFY(failureFor(done, "replacement").isEmpty());
    }
    void unrelatedTokenUpdatePreservesHandlesAndPendingRequests_data() {
        QTest::addColumn<bool>("remove");
        QTest::newRow("rotate-vercel") << false;
        QTest::newRow("remove-vercel") << true;
    }
    void unrelatedTokenUpdatePreservesHandlesAndPendingRequests() {
        QFETCH(bool, remove);
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const auto path = data.filePath("plugin-credentials.json");
        const auto github = credential("github", "FAKE_REVIEW_GITHUB_73a14");
        auto vercel = credential("vercel", "FAKE_REVIEW_VERCEL_09ac7");
        QString failure;
        QVERIFY2(ProviderStore::save(path, {github, vercel}, &failure), qPrintable(failure));
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        PluginCatalog catalog;
        QVERIFY2(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure), qPrintable(failure));
        QVERIFY2(catalog.configureAgent(broker, "agent-b", data.path(), {}, &failure), qPrintable(failure));
        const auto githubA = tokenHandle(broker, "agent-a", "github-pat");
        const auto githubB = tokenHandle(broker, "agent-b", "github-pat");
        const auto vercelA = tokenHandle(broker, "agent-a", "vercel-pat");
        QVERIFY(!githubA.isEmpty() && !githubB.isEmpty() && !vercelA.isEmpty());
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("github-a", "agent-a", request("https://api.github.com/user", githubA));
        broker.fetch("github-b", "agent-b", request("https://api.github.com/user", githubB));
        broker.fetch("vercel-a", "agent-a", request("https://api.vercel.com/v2/user", vercelA));
        broker.fetch("plain", "agent-a", request("https://fixture.invalid/plain"));
        QCOMPARE(transport.requests.size(), 4);
        vercel.apiKey = "FAKE_REVIEW_VERCEL_CHANGED_c9e52";
        QVERIFY2(ProviderStore::save(path, remove ? QList<ProviderConfig>{github} : QList<ProviderConfig>{github, vercel}, &failure), qPrintable(failure));
        QVERIFY2(catalog.configureAgent(broker, "agent-a", data.path(), {}, &failure), qPrintable(failure));
        QVERIFY2(!completed(done, "github-a"), qPrintable(failureFor(done, "github-a")));
        QVERIFY(!completed(done, "github-b"));
        QVERIFY(!completed(done, "plain"));
        QVERIFY(completed(done, "vercel-a"));
        QVERIFY(failureFor(done, "vercel-a").contains("cancelled"));
        QCOMPARE(tokenHandle(broker, "agent-a", "github-pat"), githubA);
        QCOMPARE(tokenHandle(broker, "agent-b", "github-pat"), githubB);
        if (remove) QVERIFY(broker.credential("agent-a", "vercel-pat").isNull());
        else QVERIFY(tokenHandle(broker, "agent-a", "vercel-pat") != vercelA);
        broker.fetch("old-vercel", "agent-a", request("https://api.vercel.com/v2/user", vercelA));
        QVERIFY(failureFor(done, "old-vercel").contains("revoked"));
        QCOMPARE(transport.requests.size(), 4);
        finishHeld(transport);
        QTRY_COMPARE(done.size(), 5);
        QVERIFY(failureFor(done, "github-a").isEmpty());
        QVERIFY(failureFor(done, "github-b").isEmpty());
        QVERIFY(failureFor(done, "plain").isEmpty());
    }
    void identicalCredentialResavePreservesHandle() {
        QTemporaryDir data;
        const auto path = data.filePath("plugin-credentials.json");
        const auto github = credential("github", "FAKE_REVIEW_SAME_1a238");
        QVERIFY(ProviderStore::save(path, {github}));
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        PluginCatalog catalog;
        QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}));
        const auto handle = tokenHandle(broker, "agent-a", "github-pat");
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("held", "agent-a", request("https://api.github.com/user", handle));
        QVERIFY(ProviderStore::save(path, {github}));
        QVERIFY(catalog.configureAgent(broker, "agent-a", data.path(), {}));
        QCOMPARE(done.size(), 0);
        QCOMPARE(tokenHandle(broker, "agent-a", "github-pat"), handle);
        finishHeld(transport);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(failureFor(done, "held").isEmpty());
    }
    void allowlistOnlyRevokesRemovedTypesForExactAgent() {
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        QVERIFY(broker.setCredential("github-pat", {{"token", "FAKE_ALLOW_GITHUB_5173"}}));
        QVERIFY(broker.setCredential("figma-pat", {{"token", "FAKE_ALLOW_FIGMA_1715"}}));
        const QStringList both{"github-pat", "figma-pat"};
        broker.setAllowedCredentialTypes("agent-a", both);
        broker.setAllowedCredentialTypes("agent-b", both);
        const auto githubA = tokenHandle(broker, "agent-a", "github-pat");
        const auto figmaA = tokenHandle(broker, "agent-a", "figma-pat");
        const auto figmaB = tokenHandle(broker, "agent-b", "figma-pat");
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("plain", "agent-a", request("https://fixture.invalid/plain"));
        broker.fetch("github-a", "agent-a", request("https://api.github.com/user", githubA));
        broker.fetch("figma-a", "agent-a", request("https://api.figma.com/v1/me", figmaA));
        broker.fetch("figma-b", "agent-b", request("https://api.figma.com/v1/me", figmaB));
        QCOMPARE(transport.requests.size(), 4);
        broker.setAllowedCredentialTypes("agent-a", {"github-pat"});
        QVERIFY(!completed(done, "plain"));
        QVERIFY(!completed(done, "github-a"));
        QVERIFY(!completed(done, "figma-b"));
        QVERIFY(completed(done, "figma-a"));
        QCOMPARE(tokenHandle(broker, "agent-a", "github-pat"), githubA);
        QCOMPARE(tokenHandle(broker, "agent-b", "figma-pat"), figmaB);
        QVERIFY(broker.credential("agent-a", "figma-pat").isNull());
        broker.fetch("denied", "agent-a", request("https://api.figma.com/v1/me", figmaA));
        QVERIFY(failureFor(done, "denied").contains("revoked"));
        broker.setAllowedCredentialTypes("agent-a", both);
        QVERIFY(tokenHandle(broker, "agent-a", "figma-pat") != figmaA);
        QCOMPARE(tokenHandle(broker, "agent-a", "github-pat"), githubA);
        finishHeld(transport);
        QTRY_COMPARE(done.size(), 5);
        QVERIFY(failureFor(done, "plain").isEmpty());
        QVERIFY(failureFor(done, "github-a").isEmpty());
        QVERIFY(failureFor(done, "figma-b").isEmpty());
    }
    void allowlistAddAndReorderPreserveRequests() {
        PluginTransport transport;
        transport.defaultPlan.hold = true;
        NetworkBroker broker(&transport, nullptr);
        QVERIFY(broker.setCredential("github-pat", {{"token", "FAKE_REORDER_TOKEN_6112"}}));
        broker.setAllowedCredentialTypes("agent", {"github-pat"});
        const auto handle = tokenHandle(broker, "agent", "github-pat");
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("held", "agent", request("https://api.github.com/user", handle));
        broker.setAllowedCredentialTypes("agent", {"github-pat", "figma-pat"});
        broker.setAllowedCredentialTypes("agent", {"figma-pat", "github-pat"});
        QCOMPARE(done.size(), 0);
        QCOMPARE(tokenHandle(broker, "agent", "github-pat"), handle);
        finishHeld(transport);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(failureFor(done, "held").isEmpty());
    }
    void callerUserAgentReplacesDefault_data() {
        QTest::addColumn<QJsonValue>("headers");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("object-upper") << QJsonValue(QJsonObject{{"User-Agent", "Fixture/1"}}) << QByteArray("Fixture/1");
        QTest::newRow("object-lower") << QJsonValue(QJsonObject{{"user-agent", "Fixture/2"}}) << QByteArray("Fixture/2");
        QTest::newRow("pair-duplicates") << QJsonValue(QJsonArray{QJsonArray{"User-Agent", "First/1"}, QJsonArray{"user-agent", "Second/2"}}) << QByteArray("First/1, Second/2");
        QTest::newRow("absent") << QJsonValue(QJsonObject{}) << QByteArray("ADE-Native-Sandbox/0.1");
    }
    void callerUserAgentReplacesDefault() {
        QFETCH(QJsonValue, headers);
        QFETCH(QByteArray, expected);
        PluginTransport transport;
        NetworkBroker broker(&transport, nullptr);
        QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("ua", "agent", {{"url", "https://fixture.invalid/user-agent"}, {"headers", headers}});
        QCOMPARE(transport.requests.size(), 1);
        QCOMPARE(transport.requests.first().request.rawHeader("User-Agent"), expected);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done[0][2].toString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(PluginFindingsTest)
#include "test_plugin_findings.moc"
