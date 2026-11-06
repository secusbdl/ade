#include "ClaudeModelDiscovery.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeDiscoveryTest : public QObject {
  Q_OBJECT
  QString executable() const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("claude_fixture.exe");
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CLAUDE_FIXTURE");
    qunsetenv("ADE_CLAUDE_LOG");
  }
  void initializeReturnsCatalogWithoutUserPrompt() {
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv("ADE_CLAUDE_LOG", log.toUtf8());
    ClaudeModelDiscovery d;
    QSignalSpy done(&d, &ClaudeModelDiscovery::completed);
    QVERIFY(d.start(executable()));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][1].toString().isEmpty());
    QVERIFY(!d.isBusy());
    const auto models = done[0][0].toJsonArray();
    QCOMPARE(models.size(), 1);
    QCOMPARE(models[0].toObject().value("id").toString(),
             QString("fixture-model"));
    QFile f(log);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto lines = f.readAll().split('\n');
    QCOMPARE(lines.size(), 2);
    const auto request = QJsonDocument::fromJson(lines[0]).object();
    QCOMPARE(request.value("type").toString(), QString("control_request"));
    QCOMPARE(request.value("request").toObject().value("subtype").toString(),
             QString("initialize"));
  }
  void catalogWhitelistsModelFieldsWithoutAccountData() {
    QString error;
    const QJsonObject response{
        {"account", QJsonObject{{"token", "SYNTHETIC_PRIVATE_ACCOUNT"}}},
        {"models", QJsonArray{QJsonObject{
                       {"value", "default"},
                       {"displayName", "Default"},
                       {"resolvedModel", "claude-sonnet-5.5"},
                       {"apiKey", "SYNTHETIC_PRIVATE_MODEL"},
                       {"supportsEffort", true},
                       {"supportedEffortLevels",
                        QJsonArray{"high", "unknown", "high", "max"}}}}}};
    const auto models = ClaudeModelDiscovery::parse(response, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(models.size(), 1);
    QCOMPARE(models[0].toObject().value("reasoningEfforts").toArray(),
             (QJsonArray{"high", "max"}));
    QCOMPARE(models[0].toObject().value("resolvedModel").toString(),
             QString("claude-sonnet-5.5"));
    QVERIFY(!QJsonDocument(models).toJson().contains("PRIVATE"));
    QVERIFY(!models[0].toObject().contains("contextWindow"));
  }
  void malformedCatalogFailsClosed_data() {
    QTest::addColumn<QJsonObject>("value");
    QTest::newRow("missing") << QJsonObject{};
    QTest::newRow("empty") << QJsonObject{{"models", QJsonArray{}}};
    QTest::newRow("nonobject") << QJsonObject{{"models", QJsonArray{"wrong"}}};
    QTest::newRow("duplicate") << QJsonObject{
        {"models",
         QJsonArray{QJsonObject{{"value", "a"}, {"displayName", "A"}},
                    QJsonObject{{"value", "a"}, {"displayName", "B"}}}}};
  }
  void malformedCatalogFailsClosed() {
    QFETCH(QJsonObject, value);
    QString error;
    QVERIFY(ClaudeModelDiscovery::parse(value, &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
  void cancelAndReplacementCannotPublishOldCatalog() {
    qputenv("ADE_CLAUDE_FIXTURE", "hold");
    ClaudeModelDiscovery d;
    QSignalSpy done(&d, &ClaudeModelDiscovery::completed);
    QVERIFY(d.start(executable()));
    QTest::qWait(80);
    d.cancel();
    QVERIFY(!d.isBusy());
    qunsetenv("ADE_CLAUDE_FIXTURE");
    QVERIFY(d.start(executable()));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][1].toString().isEmpty());
    QTest::qWait(50);
    QCOMPARE(done.size(), 1);
  }
  void completedObserverMayDeleteDiscovery() {
    QPointer<ClaudeModelDiscovery> d = new ClaudeModelDiscovery;
    connect(d, &ClaudeModelDiscovery::completed, d, [&] { delete d.data(); });
    QVERIFY(d->start(executable()));
    QTRY_VERIFY(!d);
  }
};
QTEST_GUILESS_MAIN(ClaudeDiscoveryTest)
#include "test_claude_discovery.moc"
