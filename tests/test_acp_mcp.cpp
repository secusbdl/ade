#include "AcpHarnessSession.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
AcpHarnessOptions options(QString workspace) {
  AcpHarnessOptions value;
  value.executable = ACP_MCP_FIXTURE;
  value.providerId = "fixture";
  value.workspace = workspace;
  value.model = "fixture";
  value.questionMcpExecutable = MCP_HELPER;
  return value;
}
QJsonArray input() {
  return {QJsonObject{{"type", "text"}, {"text", "Synthetic request"}}};
}
QJsonObject answer() {
  return {{"completed", true},
          {"cancelled", false},
          {"answers", QJsonObject{{"answer", "fixture answer"}}},
          {"completedSteps", 1}};
}
} // namespace
class AcpMcpTests : public QObject {
  Q_OBJECT
private slots:
  void init() { qunsetenv("ADE_MCP_SCENARIO"); }
  void completeThroughActualConfiguredHelper() {
    QTemporaryDir workspace;
    AcpHarnessSession session;
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        finished(&session, &AcpHarnessSession::finished),
        events(&session, &AcpHarnessSession::event);
    QVERIFY(session.startTurn(options(workspace.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    const auto token = questions[0][0].toString();
    QVERIFY(!session.completeStagewiseQuestion("wrong", answer()));
    QVERIFY(session.completeStagewiseQuestion(token, answer()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "completed");
    int questionEvents = 0;
    for (const auto &event : events) {
      const auto e = event[0].toJsonObject();
      if (e["type"] == "stagewiseQuestion")
        ++questionEvents;
    }
    QCOMPARE(questionEvents, 2);
    QVERIFY(!session.completeStagewiseQuestion(token, answer()));
  }
  void cancellationStopsPendingForm() {
    QTemporaryDir workspace;
    AcpHarnessSession session;
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        cancelled(&session, &AcpHarnessSession::stagewiseQuestionCancelled),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(workspace.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    const auto token = questions[0][0].toString();
    session.interrupt();
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "interrupted");
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!session.completeStagewiseQuestion(token, answer()));
  }
  void disabledPresenterDoesNotAdvertiseMcp() {
    QTemporaryDir workspace;
    AcpHarnessSession session;
    auto config = options(workspace.path());
    config.questionMcpExecutable.clear();
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(config, input()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(questions.size(), 0);
    QVERIFY(QJsonDocument(finished[0][0].toJsonObject())
                .toJson()
                .contains("NO_MCP"));
  }
  void nativeElicitationAndMcpShareOneForm() {
    qputenv("ADE_MCP_SCENARIO", "elicitation");
    QTemporaryDir workspace;
    AcpHarnessSession session;
    QSignalSpy questions(&session, &AcpHarnessSession::questionRequired),
        fallback(&session, &AcpHarnessSession::stagewiseQuestionRequired),
        events(&session, &AcpHarnessSession::event),
        finished(&session, &AcpHarnessSession::finished);
    auto config = options(workspace.path());
    config.promptTimeoutMs = 250;
    QVERIFY(session.startTurn(config, input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
          for (const auto &item : events)
            if (item[0].toJsonObject()["text"].toString().contains("cancelled"))
              return true;
          return false;
        }(),
        5000);
    QCOMPARE(fallback.size(), 0);
    QTest::qWait(500);
    QCOMPARE(finished.size(), 0);
    QVERIFY(session.answerQuestion(questions[0][0].toString(),
                                   {{"name", "fixture"}}));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "completed");
  }
  void humanWaitPausesOnlyPromptRemainingBudget() {
    qputenv("ADE_MCP_SCENARIO", "hold-after-answer");
    QTemporaryDir workspace;
    AcpHarnessSession session;
    auto config = options(workspace.path());
    config.promptTimeoutMs = 350;
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(config, input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    QTest::qWait(650);
    QCOMPARE(finished.size(), 0);
    QVERIFY(session.completeStagewiseQuestion(questions[0][0].toString(),
                                              answer()));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "failed");
  }
  void nativeApprovalAlsoPausesPrompt() {
    qputenv("ADE_MCP_SCENARIO", "approval-only");
    QTemporaryDir workspace;
    AcpHarnessSession session;
    auto config = options(workspace.path());
    config.promptTimeoutMs = 250;
    QSignalSpy approvals(&session, &AcpHarnessSession::approvalRequired),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(config, input()));
    QTRY_COMPARE_WITH_TIMEOUT(approvals.size(), 1, 5000);
    QTest::qWait(500);
    QCOMPARE(finished.size(), 0);
    QVERIFY(session.answerApproval(approvals[0][0].toString(), true));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "completed");
  }
  void processDeathCancelsWaitingForm() {
    qputenv("ADE_MCP_SCENARIO", "process-loss");
    QTemporaryDir workspace;
    AcpHarnessSession session;
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        cancelled(&session, &AcpHarnessSession::stagewiseQuestionCancelled),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options(workspace.path()), input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!session.completeStagewiseQuestion(questions[0][0].toString(),
                                               answer()));
  }
  void helperDeathResumesPromptBudget() {
    qputenv("ADE_MCP_SCENARIO", "helper-loss");
    QTemporaryDir workspace;
    AcpHarnessSession session;
    auto config = options(workspace.path());
    config.promptTimeoutMs = 350;
    QSignalSpy questions(&session,
                         &AcpHarnessSession::stagewiseQuestionRequired),
        cancelled(&session, &AcpHarnessSession::stagewiseQuestionCancelled),
        finished(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(config, input()));
    QTRY_COMPARE_WITH_TIMEOUT(questions.size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(cancelled.size(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
    QCOMPARE(finished[0][0].toJsonObject()["status"].toString(), "failed");
  }
  void questionCallbackCanDeleteSession() {
    QTemporaryDir workspace;
    auto *session = new AcpHarnessSession;
    QPointer<AcpHarnessSession> guard = session;
    connect(session, &AcpHarnessSession::stagewiseQuestionRequired, this,
            [session] { delete session; });
    QVERIFY(session->startTurn(options(workspace.path()), input()));
    QTRY_VERIFY_WITH_TIMEOUT(!guard, 5000);
  }
};
QTEST_GUILESS_MAIN(AcpMcpTests)
#include "test_acp_mcp.moc"
