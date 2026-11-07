#include "ClaudeHarnessSession.h"
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeFollowupsSessionTest : public QObject {
  Q_OBJECT
  ClaudeHarnessOptions options(const QString &root) {
    ClaudeHarnessOptions o;
    o.executable = QDir(QCoreApplication::applicationDirPath())
                       .filePath("claude_followups_fixture.exe");
    o.workspace = root;
    o.providerId = "fixture";
    o.model = "default";
    return o;
  }
  QJsonArray input() {
    return {QJsonObject{{"type", "text"}, {"text", "SYNTHETIC_REQUEST"}}};
  }
private slots:
  void cleanup() { qunsetenv("ADE_CLAUDE_FOLLOWUPS"); }
  void realProcessSplitAndImageComplete_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("split-final") << QByteArray("split-final");
    QTest::newRow("image-result") << QByteArray("image-result");
  }
  void realProcessSplitAndImageComplete() {
    QFETCH(QByteArray, mode);
    qputenv("ADE_CLAUDE_FOLLOWUPS", mode);
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    auto result = done[0][0].toJsonObject();
    QCOMPARE(result.value("status").toString(), QString("completed"));
    auto bytes = QJsonDocument(result.value("events").toArray()).toJson();
    QVERIFY(bytes.contains("USEFUL_COMPLETION"));
    QVERIFY(!bytes.contains("SYNTHETIC_SIGNATURE"));
    QVERIFY(!bytes.contains("SYNTHETIC_IMAGE_BYTES"));
    if (mode == "image-result") {
      QVERIFY(bytes.contains("USEFUL_TOOL_TEXT"));
      QVERIFY(bytes.contains("preview unavailable"));
    }
  }
  void observedModelsAreOwnedAndExplicit_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::addColumn<QString>("model");
    QTest::addColumn<QString>("source");
    QTest::addColumn<bool>("conflict");
    for (const auto *mode :
         {"telemetry-none", "telemetry-child", "telemetry-foreign",
          "telemetry-invalid", "telemetry-trailing-newline"})
      QTest::newRow(mode) << QByteArray(mode) << QString{} << QString{}
                          << false;
    QTest::newRow("init") << QByteArray("telemetry-init")
                          << QString("cli-init-model") << QString("system/init")
                          << false;
    for (const auto *mode : {"telemetry-assistant", "telemetry-precedence"})
      QTest::newRow(mode) << QByteArray(mode)
                          << QString("claude-fixture-concrete")
                          << QString("assistant/message") << false;
    QTest::newRow("conflict")
        << QByteArray("telemetry-conflict") << QString{} << QString{} << true;
  }
  void observedModelsAreOwnedAndExplicit() {
    QFETCH(QByteArray, mode);
    QFETCH(QString, model);
    QFETCH(QString, source);
    QFETCH(bool, conflict);
    qputenv("ADE_CLAUDE_FOLLOWUPS", mode);
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto result = done[0][0].toJsonObject();
    QCOMPARE(result.value("status").toString(), QString("completed"));
    const auto t = result.value("modelTelemetry").toObject();
    QCOMPARE(t.value("requestedModel").toString(), QString("default"));
    QCOMPARE(t.value("observedModel").toString(), model);
    QCOMPARE(t.value("observedModelSource").toString(), source);
    QCOMPARE(t.value("observedModelConflict").toBool(), conflict);
  }
  void reusedSessionDoesNotInheritObservation() {
    qputenv("ADE_CLAUDE_FOLLOWUPS", "telemetry-reset");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0]
                 .toJsonObject()
                 .value("modelTelemetry")
                 .toObject()
                 .value("observedModel")
                 .toString(),
             QString("claude-fixture-concrete"));
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(done[1][0]
                .toJsonObject()
                .value("modelTelemetry")
                .toObject()
                .value("observedModel")
                .toString()
                .isEmpty());
  }
  void imagePublicationCanCancelDeleteOrReplace_data() {
    QTest::addColumn<QString>("action");
    for (const auto *a : {"cancel", "delete", "replace"})
      QTest::newRow(a) << QString(a);
  }
  void imagePublicationCanCancelDeleteOrReplace() {
    QFETCH(QString, action);
    qputenv("ADE_CLAUDE_FOLLOWUPS", "image-result");
    QTemporaryDir dir;
    QPointer<ClaudeHarnessSession> s = new ClaudeHarnessSession;
    QSignalSpy done(s, &ClaudeHarnessSession::finished);
    bool acted = false;
    QString oldTurn;
    connect(s, &ClaudeHarnessSession::event, this, [&](QJsonObject event) {
      if (acted ||
          !event.value("output").toString().contains("preview unavailable"))
        return;
      acted = true;
      oldTurn = s->turnId();
      if (action == "delete") {
        delete s;
        return;
      }
      if (action == "cancel") {
        s->interrupt();
        return;
      }
      s->shutdown();
      qputenv("ADE_CLAUDE_FOLLOWUPS", "telemetry-assistant");
      QVERIFY(s->startTurn(options(dir.path()), input()));
    });
    QVERIFY(s->startTurn(options(dir.path()), input()));
    QTRY_VERIFY(acted);
    if (action == "delete") {
      QVERIFY(s.isNull());
      QTest::qWait(80);
      QCOMPARE(done.size(), 0);
      return;
    }
    QTRY_COMPARE(done.size(), 1);
    auto result = done[0][0].toJsonObject();
    QCOMPARE(result.value("status").toString(), action == "cancel"
                                                    ? QString("interrupted")
                                                    : QString("completed"));
    if (action == "replace") {
      QVERIFY(result.value("turnId").toString() != oldTurn);
      QVERIFY(!QJsonDocument(result).toJson().contains("preview unavailable"));
    }
    delete s;
  }
  void activeOutputDoesNotResetExistingBudget() {
    qputenv("ADE_CLAUDE_FOLLOWUPS", "active-budget");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    auto o = options(dir.path());
    o.turnTimeoutMs = 450;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished),
        events(&s, &ClaudeHarnessSession::event);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
    QVERIFY(done[0][0].toJsonObject().value("error").toString().contains(
        "timed out"));
    QVERIFY(events.size() >= 3);
    QVERIFY(elapsed.elapsed() < 2000);
  }
};
QTEST_GUILESS_MAIN(ClaudeFollowupsSessionTest)
#include "test_claude_followups_session.moc"
