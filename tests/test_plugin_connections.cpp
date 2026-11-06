#include "plugins/PluginConnectionService.h"
#include "core/ProviderStore.h"
#include "support/PluginTransport.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ade;
using namespace ade::test;
namespace {
const QString token = "FAKE_PLUGIN_CHECK_TOKEN_a7f91_not_a_real_secret";
bool saveToken(const QString& directory, const QString& id, const QString& value = token) {
    ProviderConfig record;
    record.id = id; record.name = id; record.protocol = "credential"; record.apiKey = value;
    return ProviderStore::save(directory + "/plugin-credentials.json", {record});
}
}
class PluginConnectionsTest : public QObject {
    Q_OBJECT
private slots:
    void serviceContracts_data();
    void serviceContracts();
    void failuresAreSafeAndSpecific_data();
    void failuresAreSafeAndSpecific();
    void cancellationAndSuperseding();
    void noTokenOrInvalidStoreNeverConnects();
    void secretReflectionAndResponseBounds();
    void completionMayDeleteOrRestartService();
    void borrowedTransportDestruction();
    void abortObserversMayRestartOrDeleteService();
};
void PluginConnectionsTest::serviceContracts_data() {
    QTest::addColumn<QString>("id"); QTest::addColumn<QString>("region");
    QTest::addColumn<QString>("url"); QTest::addColumn<QByteArray>("body");
    QTest::newRow("github") << QString("github") << QString("us") << QString("https://api.github.com/user") << QByteArray(R"({"id":123,"login":"fixture"})");
    QTest::newRow("supabase") << QString("supabase") << QString("us") << QString("https://api.supabase.com/v1/projects") << QByteArray("[]");
    QTest::newRow("figma") << QString("figma") << QString("us") << QString("https://api.figma.com/v1/me") << QByteArray(R"({"id":"fixture"})");
    QTest::newRow("vercel") << QString("vercel") << QString("us") << QString("https://api.vercel.com/v2/user") << QByteArray(R"({"user":{"id":"fixture"}})");
    QTest::newRow("posthog-us") << QString("posthog") << QString("us") << QString("https://us.posthog.com/api/users/@me/") << QByteArray(R"({"id":123})");
    QTest::newRow("posthog-eu") << QString("posthog") << QString("eu") << QString("https://eu.posthog.com/api/users/@me/") << QByteArray(R"({"id":123})");
}
void PluginConnectionsTest::serviceContracts() {
    QFETCH(QString,id); QFETCH(QString,region); QFETCH(QString,url); QFETCH(QByteArray,body);
    QTemporaryDir profile; QVERIFY(profile.isValid()); QVERIFY(saveToken(profile.path(), id));
    PluginTransport transport; transport.defaultPlan.body = body;
    PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(42, id, profile.path(), region);
    QTRY_COMPARE(completed.size(), 1); QCOMPARE(completed[0][0].toULongLong(), 42); QCOMPARE(completed[0][1].toString(), id);
    QVERIFY(completed[0][2].toBool()); QVERIFY(completed[0][3].toString().contains("read endpoint"));
    QCOMPARE(transport.requests.size(), 1); const auto request = transport.requests[0].request;
    QCOMPARE(request.url(), QUrl(url)); QCOMPARE(request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray(), QByteArray("GET"));
    QVERIFY(transport.requests[0].body.isEmpty());
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), int(QNetworkRequest::ManualRedirectPolicy));
    QCOMPARE(request.rawHeader(id == "figma" ? "X-Figma-Token" : "Authorization"),
             (id == "figma" ? token : "Bearer " + token).toUtf8());
    if (id == "github") QCOMPARE(request.rawHeader("X-GitHub-Api-Version"), QByteArray("2022-11-28"));
    QVERIFY(!completed[0][3].toString().contains(token));
}
void PluginConnectionsTest::failuresAreSafeAndSpecific_data() {
    QTest::addColumn<int>("status"); QTest::addColumn<QString>("expected");
    QTest::newRow("unauthorized") << 401 << QString("rejected this token");
    QTest::newRow("permission") << 403 << QString("permissions (projects_read)");
    QTest::newRow("limit") << 429 << QString("rate limit");
    QTest::newRow("server") << 503 << QString("unavailable");
    QTest::newRow("redirect") << 302 << QString("not forwarded");
    QTest::newRow("not-found") << 404 << QString("HTTP 404");
    QTest::newRow("wrong-success-shape") << 200 << QString("unexpected response");
}
void PluginConnectionsTest::failuresAreSafeAndSpecific() {
    QFETCH(int,status); QFETCH(QString,expected);
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "supabase"));
    PluginTransport transport; transport.defaultPlan.status = status;
    transport.defaultPlan.body = R"({"message":"Untrusted server text never displayed"})";
    if (status == 302) transport.defaultPlan.redirect = QUrl("https://untrusted.invalid/token-target");
    PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(1, "supabase", profile.path()); QTRY_COMPARE(completed.size(), 1);
    QVERIFY(!completed[0][2].toBool()); QVERIFY2(completed[0][3].toString().contains(expected), qPrintable(completed[0][3].toString()));
    QVERIFY(!completed[0][3].toString().contains("Untrusted")); QCOMPARE(transport.requests.size(), 1);
}
void PluginConnectionsTest::cancellationAndSuperseding() {
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "github"));
    PluginTransport transport; transport.defaultPlan.hold = true;
    PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(1, "github", profile.path()); const auto first = transport.replies[0];
    service.checkSaved(2, "github", profile.path()); QVERIFY(first->isFinished());
    service.cancel(1); QVERIFY(!transport.replies[1]->isFinished());
    service.cancel(2); QVERIFY(transport.replies[1]->isFinished());
    ReplyPlan okay; okay.body = R"({"id":1})";
    if (first) first->complete(okay);
    QCoreApplication::processEvents(); QCOMPARE(completed.size(), 0);
    transport.defaultPlan = okay; service.checkSaved(3, "github", profile.path());
    QTRY_COMPARE(completed.size(), 1); QCOMPARE(completed[0][0].toULongLong(), 3);
}
void PluginConnectionsTest::noTokenOrInvalidStoreNeverConnects() {
    QTemporaryDir profile; PluginTransport transport; PluginConnectionService service(&transport, nullptr);
    QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(1, "github", profile.path()); QCOMPARE(completed.size(), 1); QVERIFY(!completed[0][2].toBool());
    QVERIFY(completed[0][3].toString().contains("Save a personal"));
    QVERIFY(saveToken(profile.path(), "posthog"));
    service.checkSaved(2, "posthog", profile.path(), "https://evil.invalid");
    service.checkSaved(3, "unknown", profile.path());
    QFile file(profile.path() + "/plugin-credentials.json"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("malformed"); file.close();
    service.checkSaved(4, "github", profile.path()); QCOMPARE(completed.size(), 4);
    for (const auto& result : completed) QVERIFY(!result[2].toBool());
    QCOMPARE(transport.requests.size(), 0);
}
void PluginConnectionsTest::secretReflectionAndResponseBounds() {
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "github"));
    PluginTransport transport; PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    transport.defaultPlan.body = "{\"id\":\"" + token.toUtf8() + "\"}";
    service.checkSaved(1, "github", profile.path()); QTRY_COMPARE(completed.size(), 1);
    QVERIFY(!completed[0][2].toBool()); QVERIFY(!completed[0][3].toString().contains(token));
    transport.defaultPlan.body = QByteArray(2 * 1024 * 1024 + 1, 'x');
    service.checkSaved(2, "github", profile.path()); QTRY_COMPARE(completed.size(), 2); QVERIFY(!completed[1][2].toBool());
}
void PluginConnectionsTest::completionMayDeleteOrRestartService() {
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "github"));
    PluginTransport transport; transport.defaultPlan.body = R"({"id":1})";
    QPointer<PluginConnectionService> disposable = new PluginConnectionService(&transport, nullptr);
    connect(disposable, &PluginConnectionService::finished, this, [&] { delete disposable.data(); });
    disposable->checkSaved(1, "github", profile.path()); QTRY_VERIFY(disposable.isNull());
    PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    connect(&service, &PluginConnectionService::finished, this, [&](quint64 id) {
        if (id == 2) service.checkSaved(3, "github", profile.path());
    });
    service.checkSaved(2, "github", profile.path()); QTRY_COMPARE(completed.size(), 2);
    QCOMPARE(completed[1][0].toULongLong(), 3);
}
void PluginConnectionsTest::borrowedTransportDestruction() {
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "github"));
    auto* transport = new PluginTransport; transport->defaultPlan.hold = true;
    PluginConnectionService service(transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(1, "github", profile.path()); delete transport;
    QCOMPARE(completed.size(), 1); QVERIFY(!completed[0][2].toBool());
    service.checkSaved(2, "github", profile.path()); QCOMPARE(completed.size(), 2); QVERIFY(!completed[1][2].toBool());
}
void PluginConnectionsTest::abortObserversMayRestartOrDeleteService() {
    QTemporaryDir profile; QVERIFY(saveToken(profile.path(), "github"));
    PluginTransport transport; transport.defaultPlan.hold = true;
    PluginConnectionService service(&transport, nullptr); QSignalSpy completed(&service, &PluginConnectionService::finished);
    service.checkSaved(1, "github", profile.path());
    connect(transport.replies[0], &QNetworkReply::errorOccurred, &service, [&] {
        service.checkSaved(3, "github", profile.path());
    });
    service.checkSaved(2, "github", profile.path());
    QCOMPARE(transport.requests.size(), 2); // The nested newer check supersedes check 2.
    ReplyPlan okay; okay.body = R"({"id":1})"; transport.replies.last()->complete(okay);
    QCOMPARE(completed.size(), 1); QCOMPARE(completed[0][0].toULongLong(), 3);
    QPointer<PluginConnectionService> disposable = new PluginConnectionService(&transport, nullptr);
    disposable->checkSaved(4, "github", profile.path());
    connect(transport.replies.last(), &QNetworkReply::errorOccurred, this, [&] { delete disposable.data(); });
    disposable->checkSaved(5, "github", profile.path());
    QVERIFY(disposable.isNull());
}
QTEST_GUILESS_MAIN(PluginConnectionsTest)
#include "test_plugin_connections.moc"
