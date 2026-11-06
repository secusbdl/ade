#include "core/AgentEngine.h"
#include "harness/QuestionMcpStdio.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
void configure(AgentEngine &engine, const QString &directory,
               bool questions = true) {
  ProviderConfig provider;
  provider.id = "mcp-fixture";
  provider.name = "MCP fixture";
  provider.kind = "opencode";
  provider.protocol = "opencode-acp";
  provider.model = "fixture";
  provider.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                                   .filePath("acp_mcp_fixture.exe");
  engine.setProvider(provider);
  engine.setHarnessContext("fixture-owner", directory);
  engine.setQuestionToolEnabled(questions);
}
QJsonObject questionPart(const AgentEngine &engine) {
  for (const auto &value : engine.displayMessages())
    for (const auto &part : value.toObject()["parts"].toArray())
      if (part.toObject()["toolName"] == "askUserQuestions" ||
          part.toObject()["name"] == "askUserQuestions")
        return part.toObject();
  return {};
}
} // namespace
class McpEngineTests final : public QObject {
  Q_OBJECT
private slots:
  void init() { qunsetenv("ADE_MCP_SCENARIO"); }
  void actualSelfHelperValidatesAndCompletesNativeForm() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Synthetic question", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(!engine.submitQuestionStep("foreign", 0, {{"answer", "text"}}));
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"answer", ""}}));
    QCOMPARE(engine.pendingQuestion()["id"].toString(), token);
    QVERIFY(
        engine.submitQuestionStep(token, 0, {{"answer", "accepted fixture"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.lastTurnSucceeded());
    QVERIFY(engine.pendingQuestion().isEmpty());
    const auto part = questionPart(engine);
    QVERIFY2(!part.isEmpty(),
             QJsonDocument(engine.displayMessages()).toJson().constData());
    QCOMPARE(part["state"].toString(), "output-available");
    QVERIFY(
        QJsonDocument(engine.messages()).toJson().contains("accepted fixture"));
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"answer", "late"}}));
  }
  void allStepsAndPartialCancellationKeepFullResult() {
    qputenv("ADE_MCP_SCENARIO", "two-step");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Two steps", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.submitQuestionStep(token, 0, {{"answer", "first answer"}}));
    QCOMPARE(engine.pendingQuestion()["currentStep"].toInt(), 1);
    QVERIFY(!engine.cancelQuestion(token, {{"second", "stale step"}}));
    QVERIFY(engine.cancelQuestion(engine.pendingQuestion()["id"].toString(),
                                  {{"second", "partial draft"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    const auto history = QJsonDocument(engine.messages()).toJson();
    QVERIFY(history.contains("first answer"));
    QVERIFY(history.contains("partial draft"));
    QVERIFY(history.contains("completedSteps"));
    QVERIFY(history.contains("cancelled"));
    QVERIFY(engine.lastTurnSucceeded());
  }
  void disabledPresenterDoesNotAdvertiseMcp() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path(), false);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("No presenter", {}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.pendingQuestion().isEmpty());
    QVERIFY(QJsonDocument(engine.messages()).toJson().contains("NO_MCP"));
  }
  void invalidFormIsReturnedAsToolErrorWithoutWaiting() {
    qputenv("ADE_MCP_SCENARIO", "invalid-form");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished),
        questions(&engine, &AgentEngine::questionChanged);
    QVERIFY(engine.submitWithAttachments("Invalid definition", {}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.pendingQuestion().isEmpty());
    QCOMPARE(questions.size(), 0);
    QVERIFY(QJsonDocument(engine.messages()).toJson().contains("isError"));
    const auto part = questionPart(engine);
    QCOMPARE(part["state"].toString(), "output-error");
    QVERIFY(!part["output"].toString().isEmpty());
  }
  void stopClearsQuestionAndRejectsRetiredToken() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Stop", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto token = engine.pendingQuestion()["id"].toString();
    engine.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(!engine.isBusy());
    QVERIFY(engine.pendingQuestion().isEmpty());
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"answer", "late"}}));
    QTest::qWait(100);
    QCOMPARE(done.size(), 1);
  }
  void nativeElicitationAndFallbackShareOneQuestion() {
    qputenv("ADE_MCP_SCENARIO", "elicitation");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Only one", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto token = engine.pendingQuestion()["id"].toString();
    QTest::qWait(150);
    QCOMPARE(engine.pendingQuestion()["id"].toString(), token);
    QVERIFY(engine.submitQuestionStep(token, 0, {{"name", "native answer"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.lastTurnSucceeded());
  }
  void twoOwnersCannotAnswerEachOthersForms() {
    QTemporaryDir one, two;
    AgentEngine first, second;
    configure(first, one.path());
    configure(second, two.path());
    QSignalSpy firstDone(&first, &AgentEngine::finished),
        secondDone(&second, &AgentEngine::finished);
    QVERIFY(first.submitWithAttachments("One", {}));
    QVERIFY(second.submitWithAttachments("Two", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!first.pendingQuestion().isEmpty() &&
                                 !second.pendingQuestion().isEmpty(),
                             5000);
    const auto firstToken = first.pendingQuestion()["id"].toString(),
               secondToken = second.pendingQuestion()["id"].toString();
    QVERIFY(firstToken != secondToken);
    QVERIFY(
        !second.submitQuestionStep(firstToken, 0, {{"answer", "wrong owner"}}));
    first.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(firstDone.size(), 1, 5000);
    QVERIFY(second.isBusy());
    QVERIFY(
        second.submitQuestionStep(secondToken, 0, {{"answer", "second only"}}));
    QTRY_COMPARE_WITH_TIMEOUT(secondDone.size(), 1, 5000);
    const auto history = QJsonDocument(second.messages()).toJson();
    QVERIFY(history.contains("second only"));
    QVERIFY(!history.contains("STAGEWISE_MCP_CALLBACK_TOKEN"));
    QVERIFY(!history.contains("/tools/request-user-input"));
  }
  void allStepsCompleteWithRotatedPresenterTokens() {
    qputenv("ADE_MCP_SCENARIO", "two-step");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Complete two", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto first = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.submitQuestionStep(first, 0, {{"answer", "first"}}));
    const auto second = engine.pendingQuestion()["id"].toString();
    QVERIFY(first != second);
    QVERIFY(!engine.submitQuestionStep(first, 1, {{"second", "stale"}}));
    QVERIFY(engine.submitQuestionStep(second, 1, {{"second", "last"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(engine.lastTurnSucceeded());
    const auto output = QJsonDocument::fromJson(
                            questionPart(engine)["output"].toString().toUtf8())
                            .object();
    QCOMPARE(output["completedSteps"].toInt(), 2);
    QVERIFY(output["completed"].toBool());
    QCOMPARE(output["answers"].toObject()["second"].toString(), "last");
  }
  void helperLossClearsActualPresenter() {
    qputenv("ADE_MCP_SCENARIO", "helper-loss");
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Lost helper", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto token = engine.pendingQuestion()["id"].toString();
    QTRY_VERIFY_WITH_TIMEOUT(engine.pendingQuestion().isEmpty(), 2000);
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"answer", "late"}}));
    engine.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
  }
  void newUserMessageInterruptsFormAndRejectsItsOldToken() {
    QTemporaryDir dir;
    AgentEngine engine;
    configure(engine, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.sendUserMessage("First prompt"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    const auto old = engine.pendingQuestion()["id"].toString();
    QVERIFY(
        engine.updateQuestionDraft(old, 0, {{"answer", "retained partial"}}));
    QVERIFY(engine.sendUserMessage("Replacement prompt"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty() &&
                                 engine.pendingQuestion()["id"].toString() !=
                                     old,
                             5000);
    QVERIFY(!engine.submitQuestionStep(old, 0, {{"answer", "stale"}}));
    QVERIFY(engine.submitQuestionStep(engine.pendingQuestion()["id"].toString(),
                                      0, {{"answer", "replacement answer"}}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
    const auto history = QJsonDocument(engine.messages()).toJson();
    QVERIFY(history.contains("Replacement prompt"));
    QVERIFY(history.contains("retained partial"));
    QVERIFY(history.contains("user message"));
  }
  void questionObserverCanDeleteEngine() {
    QTemporaryDir dir;
    QPointer<AgentEngine> engine = new AgentEngine;
    configure(*engine, dir.path());
    connect(engine, &AgentEngine::questionChanged, this, [&] {
      if (engine && !engine->pendingQuestion().isEmpty())
        delete engine.data();
    });
    QVERIFY(engine->submitWithAttachments("Owner deletion", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine, 5000);
  }
  void completionDisplayObserverCanDeleteEngine() {
    QTemporaryDir dir;
    QPointer<AgentEngine> engine = new AgentEngine;
    configure(*engine, dir.path());
    QVERIFY(engine->submitWithAttachments("Completion deletion", {}));
    QTRY_VERIFY_WITH_TIMEOUT(!engine->pendingQuestion().isEmpty(), 5000);
    const auto token = engine->pendingQuestion()["id"].toString();
    connect(engine, &AgentEngine::displayChanged, this, [&] {
      if (engine && engine->pendingQuestion().isEmpty())
        delete engine.data();
    });
    engine->submitQuestionStep(token, 0, {{"answer", "delete after result"}});
    QTRY_VERIFY_WITH_TIMEOUT(!engine, 5000);
  }
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--question-mcp-stdio")
    return runQuestionMcpStdio(argc, argv,
                               QuestionForm::toolDefinition()["function"]
                                   .toObject()["parameters"]
                                   .toObject());
  QCoreApplication app(argc, argv);
  McpEngineTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "test_mcp_engine.moc"
