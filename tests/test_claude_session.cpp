#include "ClaudeHarnessSession.h"
#include "ClaudeQuestion.h"
#include "core/QuestionForm.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeSessionTest : public QObject {
  Q_OBJECT
  ClaudeHarnessOptions options(const QString &directory) {
    ClaudeHarnessOptions o;
    o.executable = QDir(QCoreApplication::applicationDirPath())
                       .filePath("claude_fixture.exe");
    o.providerId = "fixture";
    o.workspace = directory;
    o.model = "fixture-model";
    o.instructions = "CURRENT_CONTEXT";
    o.historyContext = "ONCE_ONLY_HISTORY";
    return o;
  }
  QJsonArray input() {
    return {QJsonObject{{"type", "text"}, {"text", "USER_TEXT"}}};
  }
  QList<QJsonObject> wire(const QString &path, const QString &type) {
    QFile f(path);
    QList<QJsonObject> rows;
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n')) {
        auto row = QJsonDocument::fromJson(line).object();
        if (row["type"] == type)
          rows.append(row);
      }
    return rows;
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_CLAUDE_FIXTURE");
    qunsetenv("ADE_CLAUDE_LOG");
    qunsetenv("ADE_CLAUDE_ARGS_LOG");
  }
  void twoTurnsAndResumeUseFreshContextWithoutRepeatedHistory() {
    QTemporaryDir dir;
    auto o = options(dir.path());
    auto path = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", path.toUtf8());
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("completed"));
    auto saved = s.sessionState();
    QVERIFY(!saved.isEmpty());
    o.instructions = "NEW_CONTEXT";
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 2);
    s.shutdown();
    QVERIFY(s.startTurn(o, input(), saved));
    QTRY_COMPARE(done.size(), 3);
    const auto prompts = wire(path, "user");
    QCOMPARE(prompts.size(), 3);
    QVERIFY(QJsonDocument(prompts[0]).toJson().contains("ONCE_ONLY_HISTORY"));
    for (int i = 1; i < 3; ++i) {
      auto bytes = QJsonDocument(prompts[i]).toJson();
      QVERIFY(!bytes.contains("ONCE_ONLY_HISTORY"));
      QVERIFY(bytes.contains("NEW_CONTEXT"));
      QVERIFY(!bytes.contains("CURRENT_CONTEXT"));
    }
    QCOMPARE(s.sessionState()["sessionId"], saved["sessionId"]);
  }
  void thinkingAndPermissionChangesRestartWithOfficialFlags() {
    QTemporaryDir dir;
    const auto path = dir.filePath("arguments");
    qputenv("ADE_CLAUDE_ARGS_LOG", path.toUtf8());
    auto o = options(dir.path());
    o.effort = "low";
    o.thinkingMode = "disabled";
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 1);
    o.thinkingMode = "enabled";
    o.thinkingBudget = 2048;
    o.alwaysAllow = true;
    QVERIFY(s.startTurn(o, input(), s.sessionState()));
    QTRY_COMPARE(done.size(), 2);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto lines = f.readAll().split('\n');
    QCOMPARE(lines.size(), 3);
    const auto first = QJsonDocument::fromJson(lines[0]).array(),
               second = QJsonDocument::fromJson(lines[1]).array();
    QVERIFY(first.contains("--thinking"));
    QVERIFY(first.contains("disabled"));
    QVERIFY(first.contains("--effort"));
    QVERIFY(first.contains("low"));
    QVERIFY(first.contains("--settings"));
    QVERIFY(second.contains("--max-thinking-tokens"));
    QVERIFY(second.contains("2048"));
    QVERIFY(!second.contains("--settings"));
    QVERIFY(!second.contains("--thinking"));
    for (const auto &args : {first, second}) {
      QVERIFY(!args.contains("--bare"));
      QVERIFY(!args.contains("--dangerously-skip-permissions"));
      QVERIFY(args.contains("stdio"));
    }
  }
  void permissionAnswerAndDenialAreCorrelated_data() {
    QTest::addColumn<bool>("allow");
    QTest::newRow("allow") << true;
    QTest::newRow("deny") << false;
  }
  void permissionAnswerAndDenialAreCorrelated() {
    QFETCH(bool, allow);
    qputenv("ADE_CLAUDE_FIXTURE", "permission-turn");
    QTemporaryDir dir;
    auto path = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", path.toUtf8());
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::approvalRequired),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 1);
    QVERIFY(!s.answerApproval("wrong", true));
    const auto token = ask[0][0].toString();
    QVERIFY(s.answerApproval(token, allow));
    QVERIFY(!s.answerApproval(token, !allow));
    QTRY_COMPARE(done.size(), 1);
    const auto response = wire(path, "control_response")
                              .last()["response"]
                              .toObject()["response"]
                              .toObject();
    QCOMPARE(response["behavior"].toString(),
             QString(allow ? "allow" : "deny"));
    QCOMPARE(
        done[0][0].toJsonObject()["usage"].toObject()["input_tokens"].toInt(),
        7);
  }
  void stoppingRevokesApprovalsAndNextTurnIsFresh() {
    qputenv("ADE_CLAUDE_FIXTURE", "permission-turn");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::approvalRequired),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(ask.size(), 1);
    const auto token = ask[0][0].toString();
    s.interrupt();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(done[0][0].toJsonObject()["status"].toString(),
             QString("interrupted"));
    QVERIFY(!s.answerApproval(token, true));
    qunsetenv("ADE_CLAUDE_FIXTURE");
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done[1][0].toJsonObject()["status"].toString(),
             QString("completed"));
  }
  void foreignWorkspaceCannotResumeAndForkGetsIndependentSession() {
    QTemporaryDir first, second;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(first.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    auto saved = s.sessionState();
    auto fork = saved;
    fork["fork"] = true;
    QVERIFY(s.startTurn(options(first.path()), input(), fork));
    QTRY_COMPARE(done.size(), 2);
    QVERIFY(s.sessionState()["sessionId"] != saved["sessionId"]);
    QVERIFY(s.startTurn(options(second.path()), input(), saved));
    QTRY_COMPARE(done.size(), 3);
    QVERIFY(s.sessionState()["sessionId"] != saved["sessionId"]);
    QCOMPARE(s.sessionState()["workspaceIdentity"].toString(),
             HarnessWorkspace::resolve(second.path()).identity);
  }
  void resultDoesNotDisposeBackgroundOwner() {
    qputenv("ADE_CLAUDE_FIXTURE", "background");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(s.hasBackgroundTasks());
    QVERIFY(!s.isBusy());
    s.shutdown();
    QVERIFY(!s.hasBackgroundTasks());
  }
  void nativeProcessProjectsPartialAndFinalTextOnlyOnce() {
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy events(&s, &ClaudeHarnessSession::event),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QString deltas;
    QString id;
    for (const auto &row : events) {
      const auto event = row[0].toJsonObject();
      if (event.value("type") == "text") {
        deltas += event.value("text").toString();
        if (id.isEmpty())
          id = event.value("itemId").toString();
        QCOMPARE(event.value("itemId").toString(), id);
      }
      QVERIFY(event.value("type") != "stream_event");
    }
    QCOMPARE(deltas, QString::fromUtf8("TEXT_😀"));
    const auto snapshot = done[0][0].toJsonObject().value("events").toArray();
    QCOMPARE(snapshot.size(), 2);
    QCOMPARE(snapshot[0].toObject().value("type").toString(),
             QString("textSnapshot"));
    QCOMPARE(snapshot[0].toObject().value("itemId").toString(), id);
    QCOMPARE(snapshot[0].toObject().value("text").toString(), deltas);
    QCOMPARE(snapshot[1].toObject().value("state").toString(), QString("done"));
  }
  void backgroundResultCannotFinishAnOwnedUserTurn() {
    qputenv("ADE_CLAUDE_FIXTURE", "background-result");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    const auto bytes = QJsonDocument(done[0][0].toJsonObject()).toJson();
    QVERIFY(bytes.contains("TEXT_"));
    QVERIFY(!bytes.contains("UNOWNED_BACKGROUND_RESULT"));
  }
  void stoppingFromTerminalPartObserverCannotReportSuccess() {
    qputenv("ADE_CLAUDE_FIXTURE", "partial-result");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy done(&s, &ClaudeHarnessSession::finished);
    connect(&s, &ClaudeHarnessSession::event, &s,
            [&](const QJsonObject &event) {
              if (event.value("type") == "partState")
                s.interrupt();
            });
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("interrupted"));
    QVERIFY(!s.isBusy());
  }
  void questionIsExplicitCorrelatedAndPreservesOriginalInput() {
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QTemporaryDir dir;
    const auto path = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", path.toUtf8());
    auto o = options(dir.path());
    o.questionsEnabled = true;
    o.alwaysAllow = true;
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::questionRequired),
        approval(&s, &ClaudeHarnessSession::approvalRequired),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(ask.size(), 1);
    QCOMPARE(done.size(), 0);
    QCOMPARE(approval.size(), 0);
    const auto token = ask[0][0].toString();
    const auto request = ask[0][1].toJsonObject();
    const auto form = ask[0][2].toJsonObject();
    QString error;
    QVERIFY2(QuestionForm::validate(form, &error), qPrintable(error));
    QVERIFY(!s.answerApproval(token, true));
    QVERIFY(
        !s.answerQuestion("foreign", {{"q0", "o0"}, {"q1", QJsonArray{"o1"}}}));
    QVERIFY(!s.answerQuestion(token, {{"q0", "o9"}, {"q1", QJsonArray{"o1"}}}));
    const QJsonObject answers{{"q0", "__other__:My own choice"},
                              {"q1", QJsonArray{"o1", "o0"}}};
    QVERIFY(s.validQuestionAnswer(token, answers));
    QVERIFY(s.answerQuestion(token, answers));
    QVERIFY(!s.answerQuestion(token, answers));
    QTRY_COMPARE(done.size(), 1);
    const auto result = wire(path, "control_response")
                            .last()
                            .value("response")
                            .toObject()
                            .value("response")
                            .toObject();
    QCOMPARE(result.value("behavior").toString(), QString("allow"));
    const auto updated = result.value("updatedInput").toObject();
    QCOMPARE(updated.value("questions"),
             request.value("input").toObject().value("questions"));
    QCOMPARE(updated.value("answers").toObject(),
             (QJsonObject{{"Choose one?", "My own choice"},
                          {"Choose several?", QJsonArray{"Two", "One"}}}));
  }
  void questionsDisabledOrCanceledAreDenied_data() {
    QTest::addColumn<bool>("enabled");
    QTest::newRow("disabled") << false;
    QTest::newRow("user-canceled") << true;
  }
  void questionsDisabledOrCanceledAreDenied() {
    QFETCH(bool, enabled);
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QTemporaryDir dir;
    const auto path = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", path.toUtf8());
    auto o = options(dir.path());
    o.questionsEnabled = enabled;
    o.alwaysAllow = true;
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::questionRequired),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    if (enabled) {
      QTRY_COMPARE(ask.size(), 1);
      const auto token = ask[0][0].toString();
      QVERIFY(s.cancelQuestion(token));
      QVERIFY(!s.cancelQuestion(token));
    }
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(ask.size(), enabled ? 1 : 0);
    QCOMPARE(wire(path, "control_response")
                 .last()
                 .value("response")
                 .toObject()
                 .value("response")
                 .toObject()
                 .value("behavior")
                 .toString(),
             QString("deny"));
  }
  void wireCancellationAndStopRevokeQuestionTokens() {
    qputenv("ADE_CLAUDE_FIXTURE", "cancel-question");
    QTemporaryDir dir;
    auto o = options(dir.path());
    o.questionsEnabled = true;
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::questionRequired),
        canceled(&s, &ClaudeHarnessSession::decisionCanceled),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(canceled.size(), 1);
    QCOMPARE(ask.size(), 1);
    const auto token = ask[0][0].toString();
    QCOMPARE(canceled[0][0].toString(), token);
    QVERIFY(!s.cancelQuestion(token));
    QVERIFY(!s.answerQuestion(token, {{"q0", "o0"}, {"q1", QJsonArray{"o1"}}}));
    s.interrupt();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(ask.size(), 2);
    const auto next = ask[1][0].toString();
    QVERIFY(next != token);
    QVERIFY(!s.cancelQuestion(token));
    s.interrupt();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 4000);
    QVERIFY(!s.cancelQuestion(next));
  }
  void questionObserverMayDeleteSession() {
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QTemporaryDir dir;
    auto o = options(dir.path());
    o.questionsEnabled = true;
    QPointer<ClaudeHarnessSession> s = new ClaudeHarnessSession;
    connect(s, &ClaudeHarnessSession::questionRequired, s,
            [&] { delete s.data(); });
    QVERIFY(s->startTurn(o, input()));
    QTRY_VERIFY(!s);
  }
  void authenticationFailureIsNotAToolPermissionRequest() {
    qputenv("ADE_CLAUDE_FIXTURE", "authentication-error");
    QTemporaryDir dir;
    ClaudeHarnessSession s;
    QSignalSpy failure(&s, &ClaudeHarnessSession::failed),
        done(&s, &ClaudeHarnessSession::finished),
        approval(&s, &ClaudeHarnessSession::approvalRequired),
        question(&s, &ClaudeHarnessSession::questionRequired);
    QVERIFY(s.startTurn(options(dir.path()), input()));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(failure.size(), 1);
    QCOMPARE(approval.size(), 0);
    QCOMPARE(question.size(), 0);
    QVERIFY(failure[0][0].toString().contains("Sign in"));
    QVERIFY(!QJsonDocument(done[0][0].toJsonObject())
                 .toJson()
                 .contains("SYNTHETIC_ACCOUNT_PRIVATE_ERROR"));
  }
  void humanQuestionWaitSuspendsOnlyOwningTurnDeadline() {
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    QTemporaryDir dir;
    auto o = options(dir.path());
    o.questionsEnabled = true;
    o.turnTimeoutMs = 300;
    ClaudeHarnessSession s;
    QSignalSpy ask(&s, &ClaudeHarnessSession::questionRequired),
        done(&s, &ClaudeHarnessSession::finished);
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(ask.size(), 1);
    QTest::qWait(450);
    QCOMPARE(done.size(), 0);
    QVERIFY(s.isBusy());
    QVERIFY(s.answerQuestion(ask[0][0].toString(),
                             {{"q0", "o0"}, {"q1", QJsonArray{"o1"}}}));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("completed"));
    s.shutdown();
    qputenv("ADE_CLAUDE_FIXTURE", "hold-turn");
    QVERIFY(s.startTurn(o, input()));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done[1][0].toJsonObject().value("status").toString(),
             QString("failed"));
  }
  void malformedQuestionAndAnswerShapesFailClosed() {
    const QJsonObject question{
        {"question", "Exact question?"},
        {"multiSelect", false},
        {"options", QJsonArray{QJsonObject{{"label", "__other__:literal"}},
                               QJsonObject{{"label", "Other"}}}}};
    const QJsonObject input{{"questions", QJsonArray{question}}};
    QVERIFY(!claudeQuestionForm(input).isEmpty());
    QJsonObject updated;
    QVERIFY(claudeQuestionResponse(input, {{"q0", "o0"}}, &updated));
    QCOMPARE(
        updated.value("answers").toObject().value("Exact question?").toString(),
        QString("__other__:literal"));
    QVERIFY(
        !claudeQuestionResponse(input, {{"q0", QJsonArray{"o0"}}}, nullptr));
    QVERIFY(!claudeQuestionResponse(input, {{"q0", "o0"}, {"unowned", "text"}},
                                    nullptr));
    QVERIFY(!claudeQuestionResponse(input, {{"q0", "__other__:   "}}, nullptr));
    QVERIFY(claudeQuestionForm({{"questions", QJsonArray{question, question}}})
                .isEmpty());
    auto malformed = question;
    malformed["multiSelect"] = "false";
    QVERIFY(
        claudeQuestionForm({{"questions", QJsonArray{malformed}}}).isEmpty());
    malformed = question;
    malformed["options"] = QJsonArray{QJsonObject{{"label", "same"}},
                                      QJsonObject{{"label", "same"}}};
    QVERIFY(
        claudeQuestionForm({{"questions", QJsonArray{malformed}}}).isEmpty());
  }
  void deletionFromOutputOrPermissionIsSafe_data() {
    QTest::addColumn<bool>("permission");
    QTest::newRow("event") << false;
    QTest::newRow("permission") << true;
  }
  void deletionFromOutputOrPermissionIsSafe() {
    QFETCH(bool, permission);
    if (permission)
      qputenv("ADE_CLAUDE_FIXTURE", "permission-turn");
    QTemporaryDir dir;
    QPointer<ClaudeHarnessSession> s = new ClaudeHarnessSession;
    if (permission)
      connect(s, &ClaudeHarnessSession::approvalRequired, s,
              [&] { delete s.data(); });
    else
      connect(s, &ClaudeHarnessSession::event, s, [&] { delete s.data(); });
    QVERIFY(s->startTurn(options(dir.path()), input()));
    QTRY_VERIFY(!s);
  }
};
QTEST_GUILESS_MAIN(ClaudeSessionTest)
#include "test_claude_session.moc"
