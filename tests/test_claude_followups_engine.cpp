#include "core/AgentEngine.h"
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeFollowupsEngineTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() {
    ProviderConfig p;
    p.id = "claude-fixture";
    p.name = "Claude";
    p.kind = "claude-code";
    p.protocol = "claude-control";
    p.model = "default";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("claude_followups_fixture.exe");
    return p;
  }
  void setup(AgentEngine &e, const QString &root) {
    e.setProvider(provider());
    e.setHarnessContext("owner", root);
  }
private slots:
  void cleanup() { qunsetenv("ADE_CLAUDE_FOLLOWUPS"); }
  void completeTurnRetainsUsefulContent_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("split-final") << QByteArray("split-final");
    QTest::newRow("image-result") << QByteArray("image-result");
  }
  void completeTurnRetainsUsefulContent() {
    QFETCH(QByteArray, mode);
    qputenv("ADE_CLAUDE_FOLLOWUPS", mode);
    QTemporaryDir dir;
    AgentEngine e;
    setup(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished),
        text(&e, &AgentEngine::textDelta);
    QVERIFY(e.submitWithAttachments("SYNTHETIC_REQUEST", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(e.messages().last().toObject().value("content").toString(),
             QString("USEFUL_COMPLETION"));
    QString combined;
    for (const auto &row : text)
      combined += row[0].toString();
    QCOMPARE(combined, QString("USEFUL_COMPLETION"));
    auto display = QJsonDocument(e.displayMessages()).toJson();
    QVERIFY(!display.contains("SYNTHETIC_IMAGE_BYTES"));
    QVERIFY(!display.contains("SYNTHETIC_SIGNATURE"));
    if (mode == "image-result") {
      QVERIFY(display.contains("preview unavailable"));
      QVERIFY(display.contains("USEFUL_TOOL_TEXT"));
    }
  }
  void telemetryIsSeparateDurableAndScoped() {
    qputenv("ADE_CLAUDE_FOLLOWUPS", "telemetry-precedence");
    QTemporaryDir dir;
    AgentEngine e;
    setup(e, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("SYNTHETIC_REQUEST", {}));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(e.actualModel(), QString("default"));
    auto telemetry = e.modelTelemetry();
    QCOMPARE(telemetry.value("requestedModel").toString(), QString("default"));
    QCOMPARE(telemetry.value("observedModel").toString(),
             QString("claude-fixture-concrete"));
    QCOMPARE(e.messages()
                 .last()
                 .toObject()
                 .value("metadata")
                 .toObject()
                 .value("modelTelemetry")
                 .toObject(),
             telemetry);
    auto changed = provider();
    changed.id = "different-owner";
    e.setProvider(changed);
    QVERIFY(e.modelTelemetry().isEmpty());
    changed.kind = "openai";
    changed.protocol = "openai";
    e.setProvider(changed);
    QVERIFY(e.modelTelemetry().isEmpty());
  }
  void publicationObserverCanCancelDeleteReplace_data() {
    QTest::addColumn<QString>("action");
    for (const auto *a : {"cancel", "delete", "replace"})
      QTest::newRow(a) << QString(a);
  }
  void publicationObserverCanCancelDeleteReplace() {
    QFETCH(QString, action);
    qputenv("ADE_CLAUDE_FOLLOWUPS", "image-result");
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    setup(*e, dir.path());
    QSignalSpy done(e, &AgentEngine::finished);
    bool acted = false;
    connect(e, &AgentEngine::displayChanged, this, [&] {
      if (acted || !QJsonDocument(e->displayMessages())
                        .toJson()
                        .contains("preview unavailable"))
        return;
      acted = true;
      if (action == "delete") {
        delete e;
        return;
      }
      e->cancel();
      if (action == "replace") {
        qputenv("ADE_CLAUDE_FOLLOWUPS", "telemetry-assistant");
        QVERIFY(e->submitWithAttachments("REPLACEMENT", {}));
      }
    });
    QVERIFY(e->submitWithAttachments("SYNTHETIC_REQUEST", {}));
    QTRY_VERIFY(acted);
    if (action == "delete") {
      QVERIFY(e.isNull());
      QTest::qWait(80);
      return;
    }
    QTRY_VERIFY(!e->isBusy());
    if (action == "cancel") {
      QVERIFY(!e->lastTurnSucceeded());
      QVERIFY(
          !e->messages().last().toObject().value("content").toString().contains(
              "USEFUL_COMPLETION"));
    } else {
      QVERIFY(e->lastTurnSucceeded());
      QCOMPARE(e->messages().last().toObject().value("content").toString(),
               QString("USEFUL_COMPLETION"));
      QCOMPARE(e->modelTelemetry().value("observedModel").toString(),
               QString("claude-fixture-concrete"));
    }
    delete e;
  }
};
QTEST_GUILESS_MAIN(ClaudeFollowupsEngineTest)
#include "test_claude_followups_engine.moc"
