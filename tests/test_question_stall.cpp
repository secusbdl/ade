#include "ui/AppStyle.h"
#include "ui/AppFonts.h"
#include "ui/ConversationView.h"
#include "ui/QuestionPanel.h"
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
#include <memory>
#include <vector>
using namespace ade;
namespace {
QJsonObject question() {
  return {{"id", "owned-question"},
          {"title", "Choose a fixture option"},
          {"currentStep", 0},
          {"answers", QJsonObject{}},
          {"steps",
           QJsonArray{QJsonObject{
               {"fields", QJsonArray{QJsonObject{{"questionId", "answer"},
                                                 {"label", "Which option?"},
                                                 {"type", "input"}}}}}}}};
}
QJsonObject tool(QString name, QString state) {
  return {{"id", "tool-part"}, {"type", "tool"},
          {"name", name},      {"toolCallId", "owned-call"},
          {"inputText", "{}"}, {"input", question()},
          {"state", state}};
}
QJsonObject text(QString type = "text", QString state = "streaming") {
  return {{"id", type},
          {"type", type},
          {"state", state},
          {"text", "A synthetic response."}};
}
QJsonObject row(QString id, QJsonArray parts, QString role = "assistant") {
  return {{"id", id}, {"role", role}, {"parts", parts}};
}
void show(ConversationView &view) {
  view.setAttribute(Qt::WA_DontShowOnScreen);
  view.resize(640, 420);
  view.show();
  QCoreApplication::processEvents();
}
QLabel *activity(ConversationView &view) {
  return view.findChild<QLabel *>("transcriptActivity");
}
void waitStall() { QTest::qWait(5500); }
} // namespace
class QuestionStallTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(AppFonts::load().fullyLoaded());
    initializeAppStyle(
        *qobject_cast<QApplication *>(QCoreApplication::instance()));
  }
  void humanDecisionIsNotStreaming_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("state");
    QTest::newRow("native-or-mcp-question") << "askUserQuestions" << "running";
    QTest::newRow("question-input-available")
        << "askUserQuestions" << "input-available";
    QTest::newRow("question-approval")
        << "askUserQuestions" << "approval-requested";
    QTest::newRow("ordinary-tool-approval") << "write" << "approval-requested";
  }
  void humanDecisionIsNotStreaming() {
    QFETCH(QString, name);
    QFETCH(QString, state);
    ConversationView view;
    // A paused form may coexist with an unfinished reasoning part. The form
    // state wins regardless of part order; it is not a working-stream timeout.
    const auto original =
        QJsonArray{row("answer", {text("reasoning"), tool(name, state)})};
    view.setConversation("owner", original, true);
    show(view);
    QVERIFY(activity(view));
    const auto plain = view.toPlainText();
    waitStall();
    QVERIFY2(!activity(view)->isVisible(), qPrintable(state));
    QCOMPARE(view.toPlainText(), plain);
    if (name == "askUserQuestions" && state != "approval-requested")
      QCOMPARE(view.findChild<QLabel *>("questionSummary")->text(),
               QString("Waiting for response..."));
  }
  void genuineWorkAndSettledHistoryRemainCorrect() {
    struct Case {
      QString name;
      QJsonArray messages;
      bool busy, expected;
    };
    const QList<Case> cases{
        {"question-still-generating",
         {row("a", {tool("askUserQuestions", "input-streaming")})},
         true,
         true},
        {"ordinary-running-tool",
         {row("a", {tool("read", "running")})},
         true,
         true},
        {"streaming-text", {row("a", {text()})}, true, true},
        {"answered-question",
         {row("a", {tool("askUserQuestions", "output-available")})},
         true,
         false},
        {"canceled-question",
         {row("a", {tool("askUserQuestions", "canceled")})},
         true,
         false},
        {"idle-part", {row("a", {text()})}, false, false},
        {"prior-turn-question",
         {row("old", {tool("askUserQuestions", "running")}),
          row("user", {text("text", "done")}, "user"), row("new", {text()})},
         true,
         true}};
    std::vector<std::unique_ptr<ConversationView>> views;
    for (const auto &c : cases) {
      auto v = std::make_unique<ConversationView>();
      v->setConversation(c.name, c.messages, c.busy);
      show(*v);
      views.push_back(std::move(v));
    }
    waitStall();
    for (int i = 0; i < cases.size(); ++i)
      QVERIFY2(activity(*views[i])->isVisible() == cases[i].expected,
               qPrintable(cases[i].name));
  }
  void transitionClearsOldStallAndRestartsWorkingDelay() {
    ConversationView view;
    show(view);
    view.setConversation("owner", {row("a", {text()})}, true);
    waitStall();
    QVERIFY(activity(view)->isVisible());
    view.setConversation(
        "owner", {row("a", {tool("askUserQuestions", "running")})}, true);
    QVERIFY(!activity(view)->isVisible());
    waitStall();
    QVERIFY(!activity(view)->isVisible());
    view.setConversation(
        "owner",
        {row("a", {tool("askUserQuestions", "output-available"), text()})},
        true);
    QVERIFY(!activity(view)->isVisible());
    QTest::qWait(500);
    QVERIFY(!activity(view)->isVisible());
    waitStall();
    QVERIFY(activity(view)->isVisible());
    view.clear();
    QTest::qWait(300);
    QVERIFY(!activity(view)->isVisible());
  }
  void clearingOrDestroyingPendingViewRetiresTimer() {
    QPointer<ConversationView> view = new ConversationView;
    view->setConversation(
        "owner", {row("a", {tool("askUserQuestions", "running")})}, true);
    show(*view);
    view->clear();
    QVERIFY(!activity(*view)->isVisible());
    delete view;
    QTest::qWait(300);
    QVERIFY(!view);
  }
  void actualFormAndTranscriptCapture() {
    QTemporaryDir temp;
    const auto out = qEnvironmentVariable("ADE_STALL_CAPTURES", temp.path());
    QVERIFY(QDir().mkpath(out));
    QWidget host;
    host.setAttribute(Qt::WA_DontShowOnScreen);
    host.resize(800, 650);
    auto *layout = new QVBoxLayout(&host);
    auto *view = new ConversationView;
    auto *form = new QuestionPanel;
    layout->addWidget(view, 1);
    layout->addWidget(form);
    view->setConversation(
        "owner", {row("a", {tool("askUserQuestions", "running")})}, true);
    form->setQuestion("owner", question());
    host.show();
    waitStall();
    QVERIFY(form->isVisible());
    QVERIFY(form->findChild<QPushButton *>("questionSubmit")->isEnabled());
    QCOMPARE(view->findChild<QLabel *>("questionSummary")->text(),
             QString("Waiting for response..."));
    QVERIFY(host.grab().save(out + "/pending-question.png"));
    QVERIFY(!activity(*view)->isVisible());
  }
};
QTEST_MAIN(QuestionStallTest)
#include "test_question_stall.moc"
