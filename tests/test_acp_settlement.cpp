#include "core/AgentEngine.h"
#include "harness/AcpHarnessSession.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
AcpHarnessOptions options(const QString &path) {
  AcpHarnessOptions o;
  o.executable = QDir(QCoreApplication::applicationDirPath())
                     .filePath("acp_settlement_fixture.exe");
  o.workspace = path;
  o.providerId = "fixture";
  o.model = "default";
  o.historyContext = "HISTORY_ONLY_INITIAL";
  o.instructions = "CONTEXT_ONLY_INITIAL";
  o.promptTimeoutMs = 10000;
  return o;
}
QJsonArray input() {
  return {
      QJsonObject{{"type", "text"}, {"text", "ORIGINAL_USER_ONLY_INITIAL"}}};
}
QJsonArray prompts(const QString &path) {
  QFile f(path);
  QJsonArray out;
  if (f.open(QIODevice::ReadOnly))
    for (const auto &line : f.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row.value("method") == "session/prompt")
        out.append(row.value("params"));
    }
  return out;
}
} // namespace
class AcpSettlementTest final : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_ACP_SETTLEMENT");
    qunsetenv("ADE_ACP_LOG");
  }
  void denialContinuesOnlyRemainingTask() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "deny");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished),
        events(&s, &AcpHarnessSession::event);
    int decisions = 0;
    connect(&s, &AcpHarnessSession::approvalRequired, &s,
            [&](const QString &token) {
              ++decisions;
              QVERIFY(s.answerApproval(token, false));
            });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 8000);
    QCOMPARE(decisions, 1);
    const auto sent = prompts(log);
    QCOMPARE(sent.size(), 2);
    const auto last = QJsonDocument(sent.last().toObject()).toJson();
    QVERIFY(last.contains("Continue from the rejected tool call"));
    QVERIFY(!last.contains("ORIGINAL_USER_ONLY_INITIAL"));
    QVERIFY(!last.contains("HISTORY_ONLY_INITIAL"));
    QVERIFY(!last.contains("CONTEXT_ONLY_INITIAL"));
    QVERIFY(QJsonDocument(done[0][0].toJsonObject())
                .toJson()
                .contains("CONTINUED_TASK"));
  }
  void lateCommandCompletionIsNotLost() {
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", "late");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    const auto result = done[0][0].toJsonObject();
    QCOMPARE(result.value("status").toString(), QString("completed"));
    QVERIFY(QJsonDocument(result).toJson().contains("LATE_FINAL_OUTPUT"));
  }
  void earlyPromptEndKeepsPendingDecision() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "pending-early");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished),
        asks(&s, &AcpHarnessSession::approvalRequired);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE(asks.size(), 1);
    QTest::qWait(150);
    QVERIFY(s.isBusy());
    QCOMPARE(done.size(), 0);
    QVERIFY(s.answerApproval(asks[0][0].toString(), false));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(prompts(log).size(), 2);
  }
  void repeatedDenialsAreBounded() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "deny-loop");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    connect(&s, &AcpHarnessSession::approvalRequired, &s,
            [&](const QString &t) { QVERIFY(s.answerApproval(t, false)); });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 8000);
    QCOMPARE(prompts(log).size(), 9);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
    QVERIFY(done[0][0].toJsonObject().value("error").toString().contains(
        "continuation limit"));
  }
  void missingCommandStatusHasThreeRecoveryPrompts() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "stale");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 8000);
    const auto sent = prompts(log);
    QCOMPARE(sent.size(), 4);
    int failed = 0;
    for (const auto &value :
         done[0][0].toJsonObject().value("events").toArray())
      if (value.toObject().value("update").toObject().value("status") ==
          "failed")
        ++failed;
    QCOMPARE(failed, 4);
    for (int i = 1; i < sent.size(); ++i) {
      const auto wire = QJsonDocument(sent[i].toObject()).toJson();
      QVERIFY(wire.contains("Do not retry it"));
      QVERIFY(!wire.contains("ORIGINAL_USER_ONLY_INITIAL"));
      QVERIFY(!wire.contains("HISTORY_ONLY_INITIAL"));
    }
  }
  void humanDecisionPauseIncludesEarlyTerminalResponse() {
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", "pending-early");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished),
        asks(&s, &AcpHarnessSession::approvalRequired);
    auto o = options(d.path());
    o.promptTimeoutMs = 200;
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(asks.size(), 1);
    QTest::qWait(350);
    QVERIFY(s.isBusy());
    QVERIFY(s.answerApproval(asks[0][0].toString(), false));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("completed"));
  }
  void nestedTerminalUpdateIsNotOverwrittenByStaleSweep() {
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", "two-stale");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    QString completedByObserver;
    QHash<QString, QString> lastStates;
    int failed = 0;
    connect(&s, &AcpHarnessSession::event, &s, [&](QJsonObject event) {
      const auto update = event.value("update").toObject();
      const auto id = update.value("toolCallId").toString();
      const auto status = update.value("status").toString();
      if (id.isEmpty() || status.isEmpty())
        return;
      lastStates.insert(id, status);
      if (status != "failed")
        return;
      ++failed;
      if (!completedByObserver.isEmpty())
        return;
      completedByObserver = id == "command-a" ? "command-b" : "command-a";
      auto *rpc = s.findChild<JsonRpcProcess *>();
      QVERIFY(rpc);
      // Model nested event delivery during the first timeout publication.
      rpc->notification(
          "session/update",
          {{"sessionId", "owned-session"},
           {"update", QJsonObject{{"sessionUpdate", "tool_call_update"},
                                  {"toolCallId", completedByObserver},
                                  {"status", "completed"},
                                  {"rawOutput", "REENTRANT_FINAL"}}}});
    });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(!completedByObserver.isEmpty());
    QCOMPARE(lastStates.value(completedByObserver), QString("completed"));
    QCOMPARE(failed, 1);
    QVERIFY(QJsonDocument(done[0][0].toJsonObject())
                .toJson()
                .contains("REENTRANT_FINAL"));
  }
  void activeProgressCannotExtendWholeTurnDeadline() {
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", "progress-forever");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    auto o = options(d.path());
    o.promptTimeoutMs = 200;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QVERIFY(clock.elapsed() < 2500);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
    QVERIFY(done[0][0].toJsonObject().value("error").toString().contains(
        "active-time limit"));
  }
  void cancellationFromDeniedToolSuppressesContinuation() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "deny");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    connect(&s, &AcpHarnessSession::approvalRequired, &s,
            [&](const QString &t) { QVERIFY(s.answerApproval(t, false)); });
    connect(&s, &AcpHarnessSession::event, &s, [&](QJsonObject event) {
      if (event.value("update").toObject().value("status") == "failed")
        s.interrupt();
    });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("interrupted"));
    QCOMPARE(prompts(log).size(), 1);
  }
  void lateOutputObserverCanDeleteSession() {
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", "late");
    QPointer<AcpHarnessSession> s = new AcpHarnessSession;
    connect(s, &AcpHarnessSession::event, s, [&](QJsonObject event) {
      if (event.value("update").toObject().value("rawOutput") ==
          "LATE_FINAL_OUTPUT")
        delete s.data();
    });
    QVERIFY(s->startTurn(options(d.path()), input()));
    QTRY_VERIFY_WITH_TIMEOUT(!s, 5000);
  }
  void interruptedSettlementCannotFinishReplacement() {
    QTemporaryDir d;
    const auto log = d.filePath("wire");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SETTLEMENT", "stale");
    AcpHarnessSession s;
    QSignalSpy done(&s, &AcpHarnessSession::finished);
    bool interrupted = false;
    connect(&s, &AcpHarnessSession::event, &s, [&](QJsonObject event) {
      if (!interrupted &&
          event.value("update").toObject().value("status") == "failed") {
        interrupted = true;
        s.interrupt();
      }
    });
    connect(&s, &AcpHarnessSession::finished, &s, [&](QJsonObject result) {
      if (result.value("status") == "interrupted") {
        qputenv("ADE_ACP_SETTLEMENT", "ordinary");
        QVERIFY(s.startTurn(
            options(d.path()),
            {QJsonObject{{"type", "text"}, {"text", "REPLACEMENT_TURN"}}}));
      }
    });
    QVERIFY(s.startTurn(options(d.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 7000);
    QCOMPARE(done[1][0].toJsonObject().value("status").toString(),
             QString("completed"));
    QVERIFY(QJsonDocument(done[1][0].toJsonObject())
                .toJson()
                .contains("ORDINARY_FINISH"));
    QCOMPARE(prompts(log).size(), 2);
  }
  void engineOwnsOneTurn_data() {
    QTest::addColumn<QString>("scenario");
    QTest::newRow("denial") << QString("deny");
    QTest::newRow("late-command") << QString("late");
  }
  void engineOwnsOneTurn() {
    QFETCH(QString, scenario);
    QTemporaryDir d;
    qputenv("ADE_ACP_SETTLEMENT", scenario.toUtf8());
    AgentEngine engine;
    ProviderConfig p;
    p.id = "fixture";
    p.kind = "opencode";
    p.protocol = "opencode-acp";
    p.model = "default";
    p.harnessExecutable = options(d.path()).executable;
    engine.setProvider(p);
    engine.setHarnessContext("owner", d.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    connect(&engine, &AgentEngine::approvalRequired, &engine,
            [&](const QString &id) { engine.approveTool(id, false); });
    QVERIFY(engine.submitWithAttachments("ONE_USER_TURN", {}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(engine.messages().size(), 2);
    const auto display = QJsonDocument(engine.displayMessages()).toJson();
    QVERIFY(display.contains(scenario == "deny" ? "CONTINUED_TASK"
                                                : "LATE_FINAL_OUTPUT"));
  }
};
QTEST_GUILESS_MAIN(AcpSettlementTest)
#include "test_acp_settlement.moc"
