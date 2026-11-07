#include "core/AgentEngine.h"
#include "harness/QuestionMcpStdio.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#ifdef DIRECT_MCP_ADAPTER_CASES
#include "core/AgentPresentation.h"
#include "harness/AcpToolPresentation.h"
#include "harness/ClaudeHarnessSession.h"
#include "harness/CodexHarnessSession.h"
#include "harness/HarnessQuestionMcp.h"
#endif
using namespace ade;
namespace {
void rows() {
  QTest::addColumn<QString>("kind");
  QTest::newRow("codex") << QString("codex");
  QTest::newRow("claude") << QString("claude-code");
}
void configure(AgentEngine &e, const QString &kind, const QString &dir,
               bool enabled = true) {
  ProviderConfig p;
  p.id = kind;
  p.kind = kind;
  p.name = kind;
  p.model = "default";
  p.protocol = kind == "codex" ? "codex-app-server" : "claude-control";
  p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                            .filePath("direct_mcp_fixture.exe");
  e.setProvider(p);
  e.setHarnessContext("owner", dir);
  e.setQuestionToolEnabled(enabled);
}
QJsonObject part(const AgentEngine &e) {
  for (const auto &r : e.displayMessages())
    for (const auto &p : r.toObject().value("parts").toArray())
      if (p.toObject().value("toolName") == "askUserQuestions" ||
          p.toObject().value("name") == "askUserQuestions")
        return p.toObject();
  return {};
}
QByteArray bytes(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return f.readAll();
}
} // namespace
class DirectMcpTests : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_DIRECT_MCP_SCENARIO");
    qunsetenv("ADE_DIRECT_MCP_LOG");
  }
  void actualBridge_data() { rows(); }
  void actualBridge() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    const auto log = dir.filePath("fixture.jsonl");
    qputenv("ADE_DIRECT_MCP_LOG", log.toUtf8());
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Ask shared form", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty() || !done.isEmpty(),
                             5000);
    QVERIFY2(!e.pendingQuestion().isEmpty(),
             QJsonDocument(e.messages()).toJson().constData());
    const auto token = e.pendingQuestion().value("id").toString();
    QVERIFY(!e.submitQuestionStep("foreign", 0, {{"answer", "wrong"}}));
    QVERIFY(!e.submitQuestionStep(token, 0, {{"answer", ""}}));
    QVERIFY(e.submitQuestionStep(token, 0, {{"answer", "bounded answer"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(part(e).value("state").toString(), QString("output-available"));
    const auto history = QJsonDocument(e.messages()).toJson();
    QVERIFY(history.contains("bounded answer"));
    QVERIFY(!history.contains("STAGEWISE_MCP_CALLBACK"));
    QVERIFY(!history.contains("127.0.0.1"));
    QVERIFY(!e.submitQuestionStep(token, 0, {{"answer", "stale"}}));
    const auto evidence = bytes(log);
    QVERIFY(evidence.contains("\"configured\":true"));
    QVERIFY(evidence.contains("stagewise-agent-instructions"));
    if (kind == "codex") {
      QVERIFY(evidence.contains("If your runtime exposes `functions.exec` and `ALL_TOOLS`"));
      QVERIFY(evidence.contains("mcp__stagewise__stagewise_request_user_input"));
      QVERIFY(evidence.contains("Do not invent"));
    }
    QVERIFY(evidence.contains("\"tokenShape\":true"));
  }
  void partialCancel_data() { rows(); }
  void partialCancel() {
    QFETCH(QString, kind);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "two-step");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("Two steps");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    const auto token = e.pendingQuestion().value("id").toString();
    QVERIFY(e.submitQuestionStep(token, 0, {{"answer", "retained first"}}));
    QVERIFY(!e.cancelQuestion(token));
    QVERIFY(e.cancelQuestion(e.pendingQuestion().value("id").toString()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    const auto output = part(e).value("output").toString();
    const auto result = QJsonDocument::fromJson(output.toUtf8()).object();
    QCOMPARE(result.value("completedSteps").toInt(), 1);
    QCOMPARE(result.value("cancelled").toBool(), true);
    QCOMPARE(result.value("answers").toObject().value("answer").toString(),
             QString("retained first"));
  }
  void stoppedAndRestarted_data() { rows(); }
  void stoppedAndRestarted() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QJsonArray history;
    QString old;
    {
      AgentEngine e;
      configure(e, kind, dir.path());
      e.submit("first");
      QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
      old = e.pendingQuestion().value("id").toString();
      e.cancel();
      QTRY_VERIFY(!e.isBusy());
      history = e.messages();
      QVERIFY(e.pendingQuestion().isEmpty());
    }
    AgentEngine e;
    configure(e, kind, dir.path());
    e.setMessages(history);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("replacement");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    const auto current = e.pendingQuestion().value("id").toString();
    QVERIFY(current != old);
    QVERIFY(!e.submitQuestionStep(old, 0, {{"answer", "stale"}}));
    QVERIFY(e.submitQuestionStep(current, 0, {{"answer", "new"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
  }
  void nativeFallback_data() { rows(); }
  void nativeFallback() {
    QFETCH(QString, kind);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "native");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("native question");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    const auto token = e.pendingQuestion().value("id").toString();
    const auto current = e.pendingQuestion();
    if (kind == "codex")
      QVERIFY(e.submitQuestionStep(token, 0, {{"native", "yes"}}));
    else {
      const auto fields =
          current.value("step").toObject().value("fields").toArray();
      Q_UNUSED(fields);
      QVERIFY(e.submitQuestionStep(token, 0, {{"q0", "o0"}}));
    }
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
  }
  void disabledDoesNotAdvertise_data() { rows(); }
  void disabledDoesNotAdvertise() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    const auto log = dir.filePath("log");
    qputenv("ADE_DIRECT_MCP_LOG", log.toUtf8());
    AgentEngine e;
    configure(e, kind, dir.path(), false);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("no questions");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.pendingQuestion().isEmpty());
    QVERIFY(!bytes(log).contains("stagewise-agent-instructions"));
    QVERIFY(!bytes(log).contains("\"configured\":true"));
  }
  void disabledAfterConfiguredSession_data() { rows(); }
  void disabledAfterConfiguredSession() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    const auto log = dir.filePath("log");
    qputenv("ADE_DIRECT_MCP_LOG", log.toUtf8());
    QJsonArray history;
    {
      AgentEngine e;
      configure(e, kind, dir.path());
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("enabled");
      QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
      QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(),
                                   0, {{"answer", "before disable"}}));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
      history = e.messages();
      e.setQuestionToolEnabled(false);
      e.submit("disabled live");
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 5000);
      QVERIFY(e.pendingQuestion().isEmpty());
      QVERIFY(e.lastTurnSucceeded());
      QVERIFY(QJsonDocument(e.messages().last().toObject())
                  .toJson()
                  .contains("NO_MCP"));
    }
    AgentEngine e;
    configure(e, kind, dir.path(), false);
    e.setMessages(history);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("disabled restored");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.pendingQuestion().isEmpty());
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(QJsonDocument(e.messages().last().toObject())
                .toJson()
                .contains("NO_MCP"));
  }
  void deleteFromForm_data() { rows(); }
  void deleteFromForm() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, kind, dir.path());
    connect(e, &AgentEngine::questionChanged, this, [&] {
      if (e && !e->pendingQuestion().isEmpty())
        delete e.data();
    });
    e->submit("delete");
    QTRY_VERIFY_WITH_TIMEOUT(!e, 5000);
  }
  void deleteFromResult_data() { rows(); }
  void deleteFromResult() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QPointer<AgentEngine> e = new AgentEngine;
    configure(*e, kind, dir.path());
    e->submit("answer delete");
    QTRY_VERIFY_WITH_TIMEOUT(!e->pendingQuestion().isEmpty(), 5000);
    const auto token = e->pendingQuestion().value("id").toString();
    connect(e, &AgentEngine::toolFinished, this, [&] { delete e.data(); });
    e->submitQuestionStep(token, 0, {{"answer", "delete"}});
    QTRY_VERIFY_WITH_TIMEOUT(!e, 5000);
  }
  void normalLiveAndRestoredSession_data() { rows(); }
  void normalLiveAndRestoredSession() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    QJsonArray history;
    {
      AgentEngine e;
      configure(e, kind, dir.path());
      QSignalSpy done(&e, &AgentEngine::finished);
      for (int i = 0; i < 2; ++i) {
        e.submit("normal turn " + QString::number(i));
        QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
        QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(),
                                     0, {{"answer", "value"}}));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), i + 1, 5000);
        QVERIFY(e.lastTurnSucceeded());
      }
      history = e.messages();
    }
    AgentEngine e;
    configure(e, kind, dir.path());
    e.setMessages(history);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("resumed turn");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(), 0,
                                 {{"answer", "resumed"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
  }
  void nativePendingCannotBeReplaced_data() { rows(); }
  void restoredRecoveryAndFork_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("fork");
    for (const auto &kind : QStringList{"codex", "claude-code"})
      for (bool fork : {false, true})
        QTest::newRow(qPrintable(kind + (fork ? "-fork" : "-missing-resume")))
            << kind << fork;
  }
  void restoredRecoveryAndFork() {
    QFETCH(QString, kind);
    QFETCH(bool, fork);
    QTemporaryDir dir;
    QJsonArray history;
    {
      AgentEngine e;
      configure(e, kind, dir.path());
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("source");
      QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
      QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(),
                                   0, {{"answer", "source answered"}}));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
      history = e.messages();
    }
    const auto log = dir.filePath("recovered.jsonl");
    qputenv("ADE_DIRECT_MCP_LOG", log.toUtf8());
    if (!fork)
      qputenv("ADE_DIRECT_MCP_SCENARIO", "resume-missing");
    AgentEngine e;
    configure(e, kind, dir.path());
    if (fork)
      e.setHarnessContext("fork-owner", dir.path());
    e.setMessages(history);
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("continue");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty() || !done.isEmpty(),
                             5000);
    QVERIFY2(!e.pendingQuestion().isEmpty(), bytes(log).constData());
    QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(), 0,
                                 {{"answer", "current callback"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
    const auto evidence = bytes(log);
    QVERIFY(evidence.contains("\"configured\":true"));
    if (kind == "codex")
      QVERIFY(evidence.contains(fork ? "thread/fork" : "thread/resume"));
    else
      QVERIFY(evidence.contains(fork ? "\"fork\":true" : "\"resume\":true"));
    if (!fork) {
      if (kind == "codex")
        QVERIFY(evidence.contains("thread/start"));
      else
        QVERIFY(evidence.contains("\"resume\":false"));
    }
  }
  void nativePendingCannotBeReplaced() {
    QFETCH(QString, kind);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "native-first");
    QTemporaryDir dir;
    const auto log = dir.filePath("events");
    qputenv("ADE_DIRECT_MCP_LOG", log.toUtf8());
    AgentEngine e;
    configure(e, kind, dir.path());
    e.submit("native first");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    const auto token = e.pendingQuestion().value("id").toString();
    QTRY_VERIFY_WITH_TIMEOUT(bytes(log).contains("\"cancelled\":true"), 5000);
    QCOMPARE(e.pendingQuestion().value("id").toString(), token);
    QVERIFY(part(e).isEmpty());
    e.cancel();
  }
  void approvalBeforeQuestion_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("allow");
    for (const auto &kind : QStringList{"codex", "claude-code"})
      for (bool allow : {false, true})
        QTest::newRow(qPrintable(kind + (allow ? "-allow" : "-deny")))
            << kind << allow;
  }
  void approvalBeforeQuestion() {
    QFETCH(QString, kind);
    QFETCH(bool, allow);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "approval");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy asked(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    e.submit("approval");
    QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 1, 5000);
    QVERIFY(e.pendingQuestion().isEmpty());
    e.approveTool(asked[0][0].toString(), allow);
    if (allow) {
      QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
      QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(),
                                   0, {{"answer", "approved separately"}}));
    }
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    if (!allow) {
      QVERIFY(e.pendingQuestion().isEmpty());
      QVERIFY(part(e).isEmpty());
    }
  }
  void cancelAtPresentation_data() { rows(); }
  void cancelAtPresentation() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    connect(&e, &AgentEngine::toolStarted, this,
            [&](const QString &, const QString &name) {
              if (name == "askUserQuestions")
                e.cancel();
            });
    e.submit("stop at presentation");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.pendingQuestion().isEmpty());
    QVERIFY(!e.lastTurnSucceeded());
  }
  void helperExitClearsOwnedForm_data() { rows(); }
  void helperExitClearsOwnedForm() {
    QFETCH(QString, kind);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "helper-loss");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    e.submit("helper loss");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(e.pendingQuestion().isEmpty(), 5000);
    e.cancel();
    QVERIFY(!e.isBusy());
  }
#ifdef DIRECT_MCP_ADAPTER_CASES
  void ownedWrapperPresentation_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("hidden");
    for (const auto &kind : QStringList{"codex", "claude-code"})
      for (const auto &scenario :
           QStringList{"wrapper", "wrapper-foreign", "wrapper-similar"})
        QTest::newRow(qPrintable(kind + "-" + scenario))
            << kind << scenario << (scenario == "wrapper");
  }
  void ownedWrapperPresentation() {
    QFETCH(QString, kind);
    QFETCH(QString, scenario);
    QFETCH(bool, hidden);
    qputenv("ADE_DIRECT_MCP_SCENARIO", scenario.toUtf8());
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, kind, dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("show one form");
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    auto inspect = [&](const QJsonArray &rows) {
      int wrappers = 0, forms = 0, visible = 0;
      for (const auto &r : rows)
        for (const auto &v : r.toObject().value("parts").toArray()) {
          const auto p = v.toObject();
          if (p.value("type") != "tool")
            continue;
          const auto name = p.value("name").toString();
          if (name == "askUserQuestions")
            ++forms;
          else if (p.value("toolCallId")
                       .toString()
                       .endsWith("/question-wrapper")) {
            ++wrappers;
            QCOMPARE(AcpToolPresentation::suppressQuestionWrapperPart(p),
                     hidden);
            QVERIFY(name.contains("stagewise_request_user_input"));
            QVERIFY(p.value("inputText")
                        .toString()
                        .contains("Shared native question"));
          }
          if (!AcpToolPresentation::suppressQuestionWrapperPart(p))
            ++visible;
        }
      QCOMPARE(wrappers, 1);
      QCOMPARE(forms, 1);
      QCOMPARE(visible, hidden ? 1 : 2);
    };
    inspect(e.displayMessages());
    QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(), 0,
                                 {{"answer", "wrapper answer"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
    inspect(e.displayMessages());
    inspect(AgentEngine::displayHistory(e.messages()));
    const auto saved = QJsonDocument(e.messages()).toJson();
    QVERIFY(saved.contains("question-wrapper"));
    QVERIFY(saved.contains("wrapper answer"));
    QVERIFY(!saved.contains("STAGEWISE_MCP_CALLBACK"));
  }
  void wrapperApprovalHistoryRemainsVisible() {
    qputenv("ADE_DIRECT_MCP_SCENARIO", "wrapper-approval");
    QTemporaryDir dir;
    AgentEngine e;
    configure(e, "claude-code", dir.path());
    QSignalSpy asked(&e, &AgentEngine::approvalRequired),
        done(&e, &AgentEngine::finished);
    e.submit("approval retained");
    QTRY_COMPARE_WITH_TIMEOUT(asked.size(), 1, 5000);
    auto inspect = [&](const QJsonArray &rows) {
      int matches = 0;
      for (const auto &r : rows)
        for (const auto &v : r.toObject().value("parts").toArray()) {
          const auto p = v.toObject();
          if (!p.value("toolCallId").toString().endsWith("/question-wrapper"))
            continue;
          ++matches;
          QVERIFY(p.value("approvalSeen").toBool());
          QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(p));
        }
      QCOMPARE(matches, 1);
    };
    inspect(e.displayMessages());
    e.approveTool(asked[0][0].toString(), true);
    QTRY_VERIFY_WITH_TIMEOUT(!e.pendingQuestion().isEmpty(), 5000);
    inspect(e.displayMessages());
    QVERIFY(e.submitQuestionStep(e.pendingQuestion().value("id").toString(), 0,
                                 {{"answer", "after approval"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    inspect(AgentEngine::displayHistory(e.messages()));
  }
  void markerCannotHideApprovalHistory_data() { rows(); }
  void markerCannotHideApprovalHistory() {
    QFETCH(QString, kind);
    const auto name =
        kind == "codex" ? "stagewise_request_user_input"
                        : "claude.mcp__stagewise__stagewise_request_user_input";
    AgentPresentation display;
    display.begin();
    display.tool(0, "call", name, "{}");
    display.toolState("call", "approval-requested");
    display.setToolPresentation(
        "call",
        QJsonObject{{"version", 1},
                    {"questionWrapper", true},
                    {"omitted", 0},
                    {"truncated", false},
                    {"entries",
                     QJsonArray{QJsonObject{
                         {"id", "0"}, {"kind", "generic"}, {"name", name}}}}});
    auto check = [&] {
      const auto part =
          display.row(false).value("parts").toArray()[0].toObject();
      QVERIFY(part.value("approvalSeen").toBool());
      QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(part));
    };
    check();
    display.toolState("call", "output-available");
    check();
  }
  void nativeAnswerCannotConsumeMcp() {
    QTemporaryDir dir;
    ClaudeHarnessSession session;
    ClaudeHarnessOptions o;
    o.executable = QDir(QCoreApplication::applicationDirPath())
                       .filePath("direct_mcp_fixture.exe");
    o.providerId = "fixture";
    o.model = "default";
    o.workspace = dir.path();
    o.questionMcpExecutable = QCoreApplication::applicationFilePath();
    QSignalSpy ask(&session, &ClaudeHarnessSession::stagewiseQuestionRequired),
        done(&session, &ClaudeHarnessSession::finished);
    QVERIFY(session.startTurn(
        o, QJsonArray{QJsonObject{{"type", "text"}, {"text", "wrong route"}}}));
    QTRY_COMPARE_WITH_TIMEOUT(ask.size(), 1, 5000);
    const auto token = ask[0][0].toString();
    QVERIFY(session.validQuestionAnswer(token, {{"answer", "value"}}));
    QVERIFY(!session.answerQuestion(token, {{"answer", "value"}}));
    QVERIFY(session.isStagewiseQuestion(token));
    QVERIFY(done.isEmpty());
    QVERIFY(session.completeStagewiseQuestion(
        token, {{"completed", true},
                {"cancelled", false},
                {"completedSteps", 1},
                {"answers", QJsonObject{{"answer", "correct route"}}}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
             QString("completed"));
  }
  void humanWaitSuspendsActiveDeadline_data() { rows(); }
  void humanWaitSuspendsActiveDeadline() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    const QString exe = QDir(QCoreApplication::applicationDirPath())
                            .filePath("direct_mcp_fixture.exe");
    const QJsonArray input{QJsonObject{{"type", "text"}, {"text", "wait"}}};
    const QJsonObject answer{{"completed", true},
                             {"cancelled", false},
                             {"completedSteps", 1},
                             {"answers", QJsonObject{{"answer", "waited"}}}};
    if (kind == "codex") {
      CodexHarnessSession session;
      CodexHarnessOptions o;
      o.executable = exe;
      o.providerId = "fixture";
      o.model = "default";
      o.workspace = dir.path();
      o.questionMcpExecutable = QCoreApplication::applicationFilePath();
      o.turnTimeoutMs = 500;
      QSignalSpy ask(&session, &CodexHarnessSession::stagewiseQuestionRequired),
          done(&session, &CodexHarnessSession::finished);
      QVERIFY(session.startTurn(o, input));
      QTRY_COMPARE_WITH_TIMEOUT(ask.size(), 1, 5000);
      QTest::qWait(700);
      QVERIFY(done.isEmpty());
      QVERIFY(session.completeStagewiseQuestion(ask[0][0].toString(), answer));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
      QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
               QString("completed"));
    } else {
      ClaudeHarnessSession session;
      ClaudeHarnessOptions o;
      o.executable = exe;
      o.providerId = "fixture";
      o.model = "default";
      o.workspace = dir.path();
      o.questionMcpExecutable = QCoreApplication::applicationFilePath();
      o.turnTimeoutMs = 500;
      QSignalSpy ask(&session,
                     &ClaudeHarnessSession::stagewiseQuestionRequired),
          done(&session, &ClaudeHarnessSession::finished);
      QVERIFY(session.startTurn(o, input));
      QTRY_COMPARE_WITH_TIMEOUT(ask.size(), 1, 5000);
      QTest::qWait(700);
      QVERIFY(done.isEmpty());
      QVERIFY(session.completeStagewiseQuestion(ask[0][0].toString(), answer));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
      QCOMPARE(done[0][0].toJsonObject().value("status").toString(),
               QString("completed"));
    }
  }
#endif
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--question-mcp-stdio")
    return runQuestionMcpStdio(argc, argv,
                               QuestionForm::toolDefinition()
                                   .value("function")
                                   .toObject()
                                   .value("parameters")
                                   .toObject());
  QCoreApplication app(argc, argv);
  DirectMcpTests test;
  return QTest::qExec(&test, argc, argv);
}
#include "test_direct_question_mcp.moc"
