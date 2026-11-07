#include "core/AgentEngine.h"
#include "harness/ClaudeHarnessSession.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ClaudeHarnessOptions options(QString path) {
  ClaudeHarnessOptions o;
  o.executable = QDir(QCoreApplication::applicationDirPath())
                     .filePath("claude_resume_fixture.exe");
  o.providerId = "fixture";
  o.workspace = path;
  o.turnTimeoutMs = 1500;
  o.instructions = "CURRENT_CONTEXT";
  o.historyContext = "CANONICAL_HISTORY_ONCE";
  return o;
}
QJsonObject saved(const ClaudeHarnessOptions &o) {
  const auto w = HarnessWorkspace::resolve(o.workspace);
  return {{"version", 1},
          {"kind", "claude-code"},
          {"providerId", o.providerId},
          {"executable", o.executable},
          {"workspaceIdentity", w.identity},
          {"alwaysAllow", o.alwaysAllow},
          {"sessionId", "22222222-2222-2222-2222-222222222222"}};
}
QJsonArray input() {
  return {QJsonObject{{"type", "text"}, {"text", "CURRENT_USER_ONCE"}}};
}
QJsonArray rows(QString path, QString type) {
  QFile f(path);
  QJsonArray out;
  if (f.open(QIODevice::ReadOnly))
    for (auto b : f.readAll().split('\n')) {
      auto v = QJsonDocument::fromJson(b).object();
      if (v.value("type") == type)
        out.append(v);
    }
  return out;
}
} // namespace
class ClaudeResumeTest : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_CLAUDE_RESUME_LOG");
    qunsetenv("ADE_CLAUDE_RESUME_MODE");
  }
  void missingSessionRecoversOnce_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("before-initialize") << QString("missing-before-init");
    QTest::newRow("after-prompt") << QString("missing-after-prompt");
  }
  void missingSessionRecoversOnce() {
    QFETCH(QString, mode);
    QTemporaryDir d;
    auto o = options(d.path());
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", mode.toUtf8());
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished),
        failure(&s, &ClaudeHarnessSession::failed);
    const auto old = saved(o);
    QVERIFY(s.startTurn(o, input(), old));
    const auto turn = s.turnId();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("completed"));
    QCOMPARE(failure.size(), 0);
    QCOMPARE(s.turnId(), turn);
    const auto launches = rows(log, "launch");
    QCOMPARE(launches.size(), 2);
    QVERIFY(launches[0].toObject().value("resume").toBool());
    QVERIFY(!launches[1].toObject().value("resume").toBool());
    QVERIFY(s.sessionState().value("sessionId") != old.value("sessionId"));
    const auto prompts = rows(log, "user");
    QCOMPARE(prompts.size(), mode == "missing-before-init" ? 1 : 2);
    const auto final = QJsonDocument(prompts.last().toObject()).toJson();
    QCOMPARE(final.count("CANONICAL_HISTORY_ONCE"), 1);
    QCOMPARE(final.count("CURRENT_USER_ONCE"), 1);
    QCOMPARE(final.count("CURRENT_CONTEXT"), 1);
    if (prompts.size() == 2)
      QVERIFY(!QJsonDocument(prompts[0].toObject())
                   .toJson()
                   .contains("CANONICAL_HISTORY_ONCE"));
  }
  void otherFailuresNeverReplay_data() {
    QTest::addColumn<QString>("mode");
    for (const auto *mode :
         {"auth", "quota", "billing", "network", "arbitrary", "wrong-id",
          "extra-error", "string-errors", "wrong-subtype", "false-error",
          "nested-origin", "malformed-origin", "result-content", "transport",
          "partial", "permission"})
      QTest::newRow(mode) << QString(mode);
  }
  void otherFailuresNeverReplay() {
    QFETCH(QString, mode);
    QTemporaryDir d;
    auto o = options(d.path());
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", mode.toUtf8());
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished),
        asks(&s, &ClaudeHarnessSession::approvalRequired);
    QVERIFY(s.startTurn(o, input(), saved(o)));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
    QCOMPARE(rows(log, "launch").size(), 1);
    if (mode == "partial")
      QVERIFY(QJsonDocument(done[0][0].toJsonObject())
                  .toJson()
                  .contains("PARTIAL_KEEP"));
    if (mode == "permission") {
      QCOMPARE(asks.size(), 1);
      QVERIFY(!s.answerApproval(asks[0][0].toString(), true));
    }
  }
  void recoveryCannotLoopOnFreshFailure() {
    QTemporaryDir d;
    auto o = options(d.path());
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "missing-always");
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished),
        failure(&s, &ClaudeHarnessSession::failed);
    QVERIFY(s.startTurn(o, input(), saved(o)));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(rows(log, "launch").size(), 2);
    QCOMPARE(failure.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
  }
  void observerCanRetirePendingRecovery_data() {
    QTest::addColumn<QString>("action");
    for (auto a : {"cancel", "shutdown", "delete", "replacement"})
      QTest::newRow(a) << QString(a);
  }
  void observerCanRetirePendingRecovery() {
    QFETCH(QString, action);
    QTemporaryDir d;
    auto o = options(d.path());
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "missing-after-prompt");
    QPointer<ClaudeHarnessSession> s = new ClaudeHarnessSession;
    QSignalSpy done(s, &ClaudeHarnessSession::finished);
    bool observed = false;
    auto *transport = s->findChild<ClaudeControlProcess *>();
    QVERIFY(transport);
    connect(
        transport, &ClaudeControlProcess::frame, this, [&](QJsonObject frame) {
          if (observed || frame.value("subtype") != "error_during_execution")
            return;
          observed = true;
          if (action == "cancel")
            s->interrupt();
          else if (action == "delete")
            delete s.data();
          else {
            s->shutdown();
            if (action == "replacement") {
              auto replacement = o;
              replacement.historyContext = "REPLACEMENT_HISTORY";
              QVERIFY(s->startTurn(
                  replacement, {QJsonObject{{"type", "text"},
                                            {"text", "REPLACEMENT_USER"}}}));
            }
          }
        });
    QVERIFY(s->startTurn(o, input(), saved(o)));
    QTRY_VERIFY(observed);
    if (action == "delete")
      QVERIFY(!s);
    else if (action == "shutdown") {
      QVERIFY(!s->isBusy());
      QCOMPARE(done.size(), 0);
    } else {
      QTRY_COMPARE(done.size(), 1);
      QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
               action == "cancel" ? QString("interrupted")
                                  : QString("completed"));
    }
    QCoreApplication::processEvents();
    QCOMPARE(rows(log, "launch").size(), action == "replacement" ? 2 : 1);
    if (action == "replacement") {
      auto last = QJsonDocument(rows(log, "user").last().toObject()).toJson();
      QVERIFY(last.contains("REPLACEMENT_HISTORY"));
      QVERIFY(last.contains("REPLACEMENT_USER"));
      QVERIFY(!last.contains("CURRENT_USER_ONCE"));
    }
    delete s.data();
  }
  void freshRecoveryKeepsRemainingDeadline() {
    QTemporaryDir d;
    auto o = options(d.path());
    o.turnTimeoutMs = 1800;
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "missing-deadline");
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QElapsedTimer clock;
    clock.start();
    QVERIFY(s.startTurn(o, input(), saved(o)));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(rows(log, "launch").size(), 2);
    QVERIFY(clock.elapsed() < 2500);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("failed"));
  }
  void forkCanRecoverButForeignOwnerOrRootNeverResumes_data() {
    QTest::addColumn<QString>("kind");
    for (auto v : {"fork", "owner", "workspace"})
      QTest::newRow(v) << QString(v);
  }
  void forkCanRecoverButForeignOwnerOrRootNeverResumes() {
    QFETCH(QString, kind);
    QTemporaryDir d, other;
    auto o = options(d.path());
    auto state = saved(o);
    auto log = d.filePath("wire");
    if (kind == "fork")
      state["fork"] = true;
    else if (kind == "owner")
      o.providerId = "other";
    else
      o.workspace = other.path();
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "missing-after-prompt");
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input(), state));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("completed"));
    const auto launches = rows(log, "launch");
    QCOMPARE(launches.size(), kind == "fork" ? 2 : 1);
    if (kind != "fork")
      QVERIFY(!launches[0].toObject().value("resume").toBool());
    QCOMPARE(s.sessionState().value("workspaceIdentity").toString(),
             HarnessWorkspace::resolve(o.workspace).identity);
  }
  void ordinaryResumeAndLiveNextTurnDoNotReplay() {
    QTemporaryDir d;
    auto o = options(d.path());
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "ordinary");
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input(), saved(o)));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(rows(log, "launch").size(), 1);
    for (const auto &row : rows(log, "user"))
      QVERIFY(!QJsonDocument(row.toObject())
                   .toJson()
                   .contains("CANONICAL_HISTORY_ONCE"));
  }
  void actualEngineRestoresCanonicalHistoryOnce() {
    QTemporaryDir d;
    auto log = d.filePath("wire");
    qputenv("ADE_CLAUDE_RESUME_LOG", log.toUtf8());
    qputenv("ADE_CLAUDE_RESUME_MODE", "ordinary");
    ProviderConfig p;
    p.id = "fixture";
    p.kind = "claude-code";
    p.protocol = "claude-sdk";
    p.model = "default";
    p.harnessExecutable = options(d.path()).executable;
    QJsonArray history;
    {
      AgentEngine e;
      e.setProvider(p);
      e.setHarnessContext("owner", d.path());
      QSignalSpy done(&e, &AgentEngine::finished);
      QVERIFY(e.submitWithAttachments("HISTORY_USER", {}));
      QTRY_COMPARE(done.size(), 1);
      QVERIFY(e.lastTurnSucceeded());
      history = e.messages();
    }
    qputenv("ADE_CLAUDE_RESUME_MODE", "missing-after-prompt");
    AgentEngine e;
    e.setProvider(p);
    e.setHarnessContext("owner", d.path());
    e.setMessages(history);
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("LATEST_USER", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(e.messages().size(), 4);
    const auto launches = rows(log, "launch");
    QCOMPARE(launches.size(), 3);
    QVERIFY(launches[1].toObject().value("resume").toBool());
    QVERIFY(!launches[2].toObject().value("resume").toBool());
    const auto wire =
        QJsonDocument(rows(log, "user").last().toObject()).toJson();
    QCOMPARE(wire.count("HISTORY_USER"), 1);
    QCOMPARE(wire.count("LATEST_USER"), 1);
    QCOMPARE(wire.count("FRESH_SUCCESS"), 1);
    QVERIFY(!wire.contains("workspaceIdentity"));
    QVERIFY(!wire.contains("harnessUsage"));
    QVERIFY(!wire.contains("ade_native"));
  }
};
QTEST_GUILESS_MAIN(ClaudeResumeTest)
#include "test_claude_resume.moc"
