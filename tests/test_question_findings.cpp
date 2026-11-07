#include "TestQuestionHelpers.h"
#include <QDialog>
#include <QVBoxLayout>
namespace {
QJsonObject questions() {
  return {{"title", "Synthetic answers"},
          {"steps",
           QJsonArray{
               QJsonObject{{"fields",
                            QJsonArray{QJsonObject{{"questionId", "submitted"},
                                                   {"label", "Submitted"},
                                                   {"type", "input"},
                                                   {"required", true}}}}},
               QJsonObject{{"fields", QJsonArray{QJsonObject{
                                          {"questionId", "draft"},
                                          {"label", "Synthetic password"},
                                          {"type", "input"},
                                          {"inputType", "password"}}}}}}}};
}
QJsonObject panelQuestion(QJsonObject field) {
  return {{"id", "owned-token"},
          {"title", "Synthetic form"},
          {"currentStep", 0},
          {"steps", QJsonArray{QJsonObject{{"fields", QJsonArray{field}}}}}};
}
QJsonObject textField() {
  return {{"questionId", "value"},
          {"type", "input"},
          {"label", "Value"},
          {"required", true}};
}
QJsonObject questionPart(const AgentEngine &engine) {
  for (const auto &row : engine.displayMessages())
    for (const auto &v : row.toObject()["parts"].toArray()) {
      const auto part = v.toObject();
      if (part["toolName"] == "askUserQuestions" ||
          part["name"] == "askUserQuestions")
        return part;
    }
  return {};
}
void configureHarness(AgentEngine &engine, const QString &kind,
                      const QString &root) {
  ProviderConfig p;
  p.id = kind;
  p.kind = kind;
  p.name = kind;
  p.model = "default";
  p.protocol = kind == "codex"      ? "codex-app-server"
               : kind == "opencode" ? "opencode-acp"
                                    : "claude-control";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath())
          .filePath(kind == "opencode"
                        ? "question_findings_Acp_fixture.exe"
                        : "question_findings_Direct_fixture.exe");
  engine.setProvider(p);
  engine.setHarnessContext("synthetic-owner", root);
  engine.setQuestionToolEnabled(true);
}
} // namespace
class QuestionFindingsTest final : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_DIRECT_MCP_SCENARIO");
    qunsetenv("ADE_MCP_SCENARIO");
    qunsetenv("ADE_QUESTION_RELEASE");
  }
  void resultDistinguishesSubmittedFromDraft_data() {
    QTest::addColumn<QString>("reason");
    QTest::addColumn<bool>("keep");
    QTest::newRow("cancel") << QString("user_cancelled") << false;
    QTest::newRow("stop") << QString("agent_stopped") << false;
    QTest::newRow("message") << QString("user_sent_message") << true;
  }
  void resultDistinguishesSubmittedFromDraft() {
    QFETCH(QString, reason);
    QFETCH(bool, keep);
    QuestionForm form;
    QVERIFY(form.begin(questions()));
    QVERIFY(form.submit(0, {{"submitted", "accepted"}}));
    QVERIFY(form.updateDraft(1, {{"draft", "synthetic-unsubmitted"}}));
    const auto result = form.result(reason);
    QCOMPARE(result["completedSteps"], 1);
    QCOMPARE(result["answers"].toObject()["submitted"], "accepted");
    QCOMPARE(result["answers"].toObject().contains("draft"), keep);
  }
  void explicitlySubmittedPasswordRemainsAnAnswer() {
    QuestionForm form;
    QVERIFY(form.begin(questions()));
    QVERIFY(form.submit(0, {{"submitted", "accepted"}}));
    QVERIFY(form.submit(1, {{"draft", "synthetic-submitted-password"}}));
    const auto result = form.result();
    QCOMPARE(result["completedSteps"], 2);
    QCOMPARE(result["completed"], true);
    QCOMPARE(result["answers"].toObject()["draft"],
             "synthetic-submitted-password");
  }
  void engineCancellationDoesNotPersistDraft_data() {
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QString>("action");
    for (const auto *p : {"openai", "responses", "anthropic", "google"})
      for (const auto *a : {"cancel", "stop", "message"})
        QTest::newRow(qPrintable(QString(p) + "-" + a))
            << QString(p) << QString(a);
  }
  void engineCancellationDoesNotPersistDraft() {
    QFETCH(QString, protocol);
    QFETCH(QString, action);
    Server server;
    server.protocol = protocol;
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setQuestionToolEnabled(true);
    engine.submit("Ask synthetic questions");
    QTRY_COMPARE(server.requests.size(), 1);
    server.question(0, questions());
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    QVERIFY(engine.submitQuestionStep(engine.pendingQuestion()["id"].toString(),
                                      0, {{"submitted", "accepted"}}));
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.updateQuestionDraft(token, 1,
                                       {{"draft", "synthetic-unsubmitted"}}));
    if (action == "stop")
      engine.cancel();
    else if (action == "cancel")
      QVERIFY(
          engine.cancelQuestion(token, {{"draft", "synthetic-unsubmitted"}}));
    else
      QVERIFY(engine.sendUserMessage("Use the submitted answer"));
    QTRY_VERIFY(!toolResult(engine).isEmpty());
    const auto result = toolResult(engine);
    QCOMPARE(result["answers"].toObject()["submitted"], "accepted");
    QCOMPARE(result["completedSteps"], 1);
    QCOMPARE(result["answers"].toObject().contains("draft"),
             action == "message");
    if (action != "stop") {
      QTRY_COMPARE(server.requests.size(), 2);
      QCOMPARE(QJsonDocument(server.requests[1])
                   .toJson()
                   .contains("synthetic-unsubmitted"),
               action == "message");
    }
    engine.cancel();
  }
  void panelCancelOmitsDraft() {
    QuestionPanel panel;
    auto field = textField();
    field["inputType"] = "password";
    panel.setQuestion("chat", panelQuestion(field));
    auto *input = panel.findChild<QLineEdit *>("answer_value");
    QVERIFY(input);
    input->setText("synthetic-unsubmitted");
    QSignalSpy spy(&panel, &QuestionPanel::cancelled);
    panel.findChild<QPushButton *>("questionCancel")->click();
    QCOMPARE(spy.size(), 1);
    QVERIFY(spy[0][2].toJsonObject().isEmpty());
  }
  void numberAnswersKeepJsonType_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<QJsonValue>("expected");
    QTest::newRow("default") << QString() << QJsonValue(5);
    QTest::newRow("precise-default")
        << QString() << QJsonValue(1.2345678901234567);
    QTest::newRow("decimal") << QString("-1.25") << QJsonValue(-1.25);
    QTest::newRow("exponent") << QString("1e2") << QJsonValue(100);
  }
  void numberAnswersKeepJsonType() {
    QFETCH(QString, text);
    QFETCH(QJsonValue, expected);
    QuestionPanel panel;
    auto field = textField();
    field["inputType"] = "number";
    field["defaultValue"] = expected;
    panel.setQuestion("chat", panelQuestion(field));
    if (!text.isEmpty())
      panel.findChild<QLineEdit *>("answer_value")->setText(text);
    QSignalSpy spy(&panel, &QuestionPanel::submitStep);
    panel.findChild<QPushButton *>("questionSubmit")->click();
    QCOMPARE(spy.size(), 1);
    const QJsonValue value = spy[0][3].toJsonObject().value("value");
    QVERIFY(value.isDouble());
    QCOMPARE(value, expected);
  }
  void emptyAndInvalidNumbersAreNotCoerced() {
    QuestionPanel panel;
    auto field = textField();
    field["inputType"] = "number";
    field["required"] = false;
    panel.setQuestion("chat", panelQuestion(field));
    auto *input = panel.findChild<QLineEdit *>("answer_value");
    input->clear();
    QCOMPARE(panel.draftAnswers()["value"], QJsonValue(""));
    QSignalSpy spy(&panel, &QuestionPanel::submitStep);
    panel.findChild<QPushButton *>("questionSubmit")->click();
    QCOMPARE(spy.size(), 1);
    input->setText("not-a-number");
    panel.findChild<QPushButton *>("questionSubmit")->click();
    QCOMPARE(spy.size(), 1);
    input->setText("1e309");
    panel.findChild<QPushButton *>("questionSubmit")->click();
    QCOMPARE(spy.size(), 1);
  }
  void enterSubmitsCurrentTextStep_data() {
    QTest::addColumn<int>("modifiers");
    QTest::newRow("plain") << int(Qt::NoModifier);
    QTest::newRow("control") << int(Qt::ControlModifier);
    QTest::newRow("meta") << int(Qt::MetaModifier);
  }
  void enterSubmitsCurrentTextStep() {
    QFETCH(int, modifiers);
    QuestionPanel panel;
    panel.resize(500, 400);
    panel.setQuestion("chat", panelQuestion(textField()));
    panel.show();
    QApplication::setActiveWindow(&panel);
    auto *input = panel.findChild<QLineEdit *>("answer_value");
    input->setFocus();
    input->setText("accepted");
    QCoreApplication::processEvents();
    QSignalSpy spy(&panel, &QuestionPanel::submitStep);
    QTest::keyClick(input, Qt::Key_Return, Qt::KeyboardModifiers(modifiers));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy[0][1].toString(), QString("owned-token"));
  }
  void enterOtherAndExclusions() {
    QuestionPanel panel;
    auto field = QJsonObject{
        {"questionId", "value"},
        {"type", "radio-group"},
        {"label", "Value"},
        {"options",
         QJsonArray{QJsonObject{{"value", "one"}, {"label", "One"}}}},
        {"required", true}};
    panel.setQuestion("chat", panelQuestion(field));
    panel.show();
    QApplication::setActiveWindow(&panel);
    auto *radio = panel.findChild<QRadioButton *>("answer_value_other");
    radio->click();
    auto *input = panel.findChild<QLineEdit *>("answer_value_otherText");
    input->setFocus();
    QSignalSpy spy(&panel, &QuestionPanel::submitStep);
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(spy.isEmpty());
    input->setText("accepted");
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QVERIFY(spy.isEmpty());
    QTest::keyClick(radio, Qt::Key_Return);
    QVERIFY(spy.isEmpty());
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy[0][3].toJsonObject()["value"], "__other__:accepted");
  }
  void submitCallbackMayDeletePanel() {
    auto *panel = new QuestionPanel;
    panel->setQuestion("chat", panelQuestion(textField()));
    auto *input = panel->findChild<QLineEdit *>("answer_value");
    input->setText("accepted");
    QPointer<QuestionPanel> alive(panel);
    connect(panel, &QuestionPanel::submitStep, this, [panel] { delete panel; });
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &press);
    const bool deleted = !alive;
    if (alive)
      delete panel;
    QVERIFY(deleted);
  }
  void dialogCheckboxEnterDoesNotSubmit() {
    QDialog dialog;
    auto *layout = new QVBoxLayout(&dialog);
    auto *panel = new QuestionPanel(&dialog);
    layout->addWidget(panel);
    panel->setQuestion("chat", panelQuestion({{"questionId", "flag"},
                                              {"type", "checkbox"},
                                              {"label", "Flag"}}));
    dialog.show();
    QApplication::setActiveWindow(&dialog);
    auto *check = panel->findChild<QCheckBox *>("answer_flag");
    check->setFocus();
    QCoreApplication::processEvents();
    QSignalSpy submit(panel, &QuestionPanel::submitStep);
    QSignalSpy cancelled(panel, &QuestionPanel::cancelled);
    QTest::keyClick(check, Qt::Key_Return);
    QVERIFY(submit.isEmpty());
    QVERIFY(cancelled.isEmpty());
    auto *submitButton = panel->findChild<QPushButton *>("questionSubmit");
    submitButton->setFocus();
    QTest::keyClick(submitButton, Qt::Key_Return);
    QCOMPARE(submit.size(), 1);
    auto *cancelButton = panel->findChild<QPushButton *>("questionCancel");
    cancelButton->setFocus();
    QTest::keyClick(cancelButton, Qt::Key_Return);
    QCOMPARE(cancelled.size(), 1);
  }
  void commandEnterTargetsFocusedVisibleForm() {
    QWidget window;
    QHBoxLayout layout(&window);
    QuestionPanel first, second;
    layout.addWidget(&first);
    layout.addWidget(&second);
    first.setQuestion("main", panelQuestion(textField()));
    second.setQuestion("side", panelQuestion(textField()));
    auto *a = first.findChild<QLineEdit *>("answer_value");
    auto *b = second.findChild<QLineEdit *>("answer_value");
    a->setText("main answer");
    b->setText("side answer");
    QSignalSpy main(&first, &QuestionPanel::submitStep),
        side(&second, &QuestionPanel::submitStep);
    window.resize(900, 500);
    window.show();
    QApplication::setActiveWindow(&window);
    b->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(b, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(side.size(), 1);
    QVERIFY(main.isEmpty());
    a->setFocus();
    QCoreApplication::processEvents();
    QTest::keyClick(a, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(main.size(), 1);
    QCOMPARE(side.size(), 1);
  }
  void harnessCancelAndStopOmitDraft_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("stop");
    for (const auto *kind : {"codex", "claude-code", "opencode"})
      for (const bool stop : {false, true})
        QTest::newRow(qPrintable(QString(kind) + (stop ? "-stop" : "-cancel")))
            << QString(kind) << stop;
  }
  void harnessCancelAndStopOmitDraft() {
    QFETCH(QString, kind);
    QFETCH(bool, stop);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "two-step");
    qputenv("ADE_MCP_SCENARIO", "two-step");
    QTemporaryDir dir;
    AgentEngine engine;
    configureHarness(engine, kind, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("Synthetic form");
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 5000);
    QVERIFY(engine.submitQuestionStep(engine.pendingQuestion()["id"].toString(),
                                      0, {{"answer", "accepted"}}));
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.updateQuestionDraft(token, 1,
                                       {{"second", "synthetic-unsubmitted"}}));
    if (stop)
      engine.cancel();
    else
      QVERIFY(
          engine.cancelQuestion(token, {{"second", "synthetic-unsubmitted"}}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    const auto result = QJsonDocument::fromJson(
                            questionPart(engine)["output"].toString().toUtf8())
                            .object();
    QCOMPARE(result["cancelReason"], stop ? "agent_stopped" : "user_cancelled");
    QCOMPARE(result["completedSteps"], 1);
    QCOMPARE(result["answers"].toObject()["answer"], "accepted");
    QVERIFY(!result["answers"].toObject().contains("second"));
    QVERIFY(!QJsonDocument(engine.messages())
                 .toJson()
                 .contains("synthetic-unsubmitted"));
  }
  void cancelledDisplayObserverMayDeleteEngine_data() {
    helperDisconnectKeepsSubmittedProgress_data();
  }
  void cancelledDisplayObserverMayDeleteEngine() {
    QFETCH(QString, kind);
    qputenv("ADE_DIRECT_MCP_SCENARIO", "controlled-loss");
    qputenv("ADE_MCP_SCENARIO", "controlled-loss");
    QTemporaryDir dir;
    const auto release = dir.filePath("release");
    qputenv("ADE_QUESTION_RELEASE", release.toUtf8());
    QPointer<AgentEngine> engine = new AgentEngine;
    configureHarness(*engine, kind, dir.path());
    engine->submit("Synthetic form");
    QTRY_VERIFY_WITH_TIMEOUT(!engine->pendingQuestion().isEmpty(), 5000);
    QVERIFY(
        engine->submitQuestionStep(engine->pendingQuestion()["id"].toString(),
                                   0, {{"answer", "accepted"}}));
    bool deleted = false;
    connect(engine, &AgentEngine::toolFinished, this,
            [&](const QString &, const QString &output, bool) {
              const auto value =
                  QJsonDocument::fromJson(output.toUtf8()).object();
              if (value["cancelled"] == true) {
                deleted = true;
                delete engine.data();
              }
            });
    QFile file(release);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("release");
    file.close();
    QTRY_VERIFY_WITH_TIMEOUT(!engine, 5000);
    QVERIFY(deleted);
  }
  void helperDisconnectKeepsSubmittedProgress_data() {
    QTest::addColumn<QString>("kind");
    for (const auto *k : {"codex", "claude-code", "opencode"})
      QTest::newRow(k) << QString(k);
  }
  void helperDisconnectKeepsSubmittedProgress() {
    QFETCH(QString, kind);
    QTemporaryDir dir;
    const auto release = dir.filePath("release");
    qputenv("ADE_DIRECT_MCP_SCENARIO", "controlled-loss");
    qputenv("ADE_MCP_SCENARIO", "controlled-loss");
    qputenv("ADE_QUESTION_RELEASE", release.toUtf8());
    AgentEngine engine;
    configureHarness(engine, kind, dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("Ask synthetic questions");
    QTRY_VERIFY_WITH_TIMEOUT(
        !engine.pendingQuestion().isEmpty() || !done.isEmpty(), 5000);
    QVERIFY(!engine.pendingQuestion().isEmpty());
    const auto first = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.submitQuestionStep(first, 0, {{"answer", "accepted"}}));
    const auto second = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.updateQuestionDraft(second, 1,
                                       {{"second", "synthetic-unsubmitted"}}));
    QFile file(release);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("release");
    file.close();
    QTRY_VERIFY_WITH_TIMEOUT(engine.pendingQuestion().isEmpty(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    const auto part = questionPart(engine);
    const auto result =
        QJsonDocument::fromJson(part["output"].toString().toUtf8()).object();
    QVERIFY2(!result.isEmpty(), QJsonDocument(part).toJson().constData());
    QCOMPARE(result["cancelReason"], "agent_stopped");
    QCOMPARE(result["completedSteps"], 1);
    QCOMPARE(result["answers"].toObject()["answer"], "accepted");
    QVERIFY(!result["answers"].toObject().contains("second"));
    QCOMPARE(part["state"], "canceled");
    QVERIFY(!engine.submitQuestionStep(second, 1, {{"second", "late"}}));
  }
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--question-mcp-stdio")
    return runQuestionMcpStdio(argc, argv,
                               QuestionForm::toolDefinition()["function"]
                                   .toObject()["parameters"]
                                   .toObject());
  QApplication app(argc, argv);
  QuestionFindingsTest test;
  return QTest::qExec(&test, argc, argv);
}
#include "test_question_findings.moc"
