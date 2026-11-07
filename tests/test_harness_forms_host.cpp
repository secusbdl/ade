#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "harness/QuestionMcpStdio.h"
#include "runtime/AgentToolHost.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/MarkdownParts.h"
#include "ui/ProviderDialog.h"
#include "ui/QuestionPanel.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QToolButton>
#include <QtTest>
#include <cmath>
using namespace ade;
namespace {
ProviderConfig provider(bool claude = false) {
  ProviderConfig p;
  p.id = claude ? "claude-fixture" : "mcp-fixture";
  p.name = claude ? "Claude fixture" : "OpenCode MCP fixture";
  p.kind = claude ? "claude-code" : "opencode";
  p.protocol = claude ? "claude-control" : "opencode-acp";
  p.model = "default";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath())
          .filePath(claude ? "claude_fixture.exe" : "acp_mcp_fixture.exe");
  return p;
}
bool seed(const QString &directory, bool claude = false) {
  const auto p = provider(claude);
  if (!ProviderStore::save(directory + "/providers.json", {p}))
    return false;
  QList<Conversation> chats;
  for (const auto &id : {"first", "second"}) {
    Conversation chat;
    chat.id = id;
    chat.title = id;
    chat.titleLocked = true;
    chat.providerId = p.id;
    chat.model = p.model;
    chats.append(chat);
  }
  return SessionStore(directory).save(chats);
}
bool select(MainWindow &window, const QString &id) {
  const auto *list = window.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole).toString() == id) {
      const_cast<QListWidget *>(list)->setCurrentItem(list->item(i));
      return window.currentSessionId() == id;
    }
  return false;
}
AgentEngine *engine(MainWindow &window, const QString &id) {
  for (auto *host : window.findChildren<AgentToolHost *>())
    if (host->agentId() == id)
      return qobject_cast<AgentEngine *>(host->parent());
  return nullptr;
}
QJsonObject questionPart(const AgentEngine &e) {
  for (const auto &value : e.displayMessages())
    for (const auto &part : value.toObject().value("parts").toArray())
      if (part.toObject().value("name") == "askUserQuestions")
        return part.toObject();
  return {};
}
QWidget *renderedPart(MainWindow &window, const QString &partId) {
  auto *view = window.findChild<ConversationView *>("conversationView");
  if (!view)
    return nullptr;
  for (auto *widget : view->findChildren<QWidget *>())
    if (widget->property("partId").toString() == partId)
      return widget;
  return nullptr;
}
Conversation stored(const QString &directory, const QString &id) {
  for (const auto &chat : SessionStore(directory).load())
    if (chat.id == id)
      return chat;
  return {};
}
void send(MainWindow &window, const QString &prompt) {
  auto *composer = window.findChild<Composer *>();
  auto *button = window.findChild<QToolButton *>("sendButton");
  QVERIFY(composer && button);
  composer->setPlainText(prompt);
  QVERIFY(button->isEnabled());
  QVERIFY(button->isVisible());
  QTest::mouseClick(button, Qt::LeftButton);
}
bool answer(QuestionPanel *panel, const QString &id, const QString &value) {
  for (auto *input : panel->findChildren<QLineEdit *>("answer_" + id)) {
    if (!input->isVisible())
      continue;
    input->setText(value);
    return true;
  }
  return false;
}
QPushButton *currentButton(QuestionPanel *panel, const char *name) {
  for (auto *button : panel->findChildren<QPushButton *>(name))
    if (button->isVisible() && button->isEnabled())
      return button;
  return nullptr;
}
bool click(QuestionPanel *panel, const char *name) {
  auto *button = currentButton(panel, name);
  if (!button)
    return false;
  QTest::mouseClick(button, Qt::LeftButton);
  return true;
}
bool formTextMeasured(QuestionPanel *panel) {
  const auto labels = panel->findChildren<TranscriptText *>();
  if (labels.isEmpty())
    return false;
  for (auto *label : labels) {
    if (qAbs(label->document()->textWidth() - label->viewport()->width()) > 1 ||
        label->height() !=
            qMax(1, int(std::ceil(label->document()->size().height())) + 2))
      return false;
  }
  return true;
}
double luminance(const QColor &c) {
  const auto channel = [](double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) +
         0.0722 * channel(c.blueF());
}
double contrast(QColor a, QColor b) {
  const auto x = luminance(a), y = luminance(b);
  return (qMax(x, y) + 0.05) / (qMin(x, y) + 0.05);
}
} // namespace
class HarnessFormsHostTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    initializeAppStyle(*qApp);
    QVERIFY(QFileInfo::exists(provider().harnessExecutable));
    QVERIFY(QFileInfo::exists(provider(true).harnessExecutable));
  }
  void cleanup() {
    qunsetenv("ADE_MCP_SCENARIO");
    qunsetenv("ADE_CLAUDE_FIXTURE");
  }
  void mcpOwnerSwitchSubmitAndTranscriptStayCorrelated() {
    QTemporaryDir dir;
    QVERIFY(seed(dir.path()));
    MainWindow window(dir.path());
    window.resize(1100, 750);
    window.show();
    QVERIFY(select(window, "first"));
    send(window, "Show the native MCP form");
    QTRY_VERIFY(engine(window, "first"));
    auto *first = engine(window, "first");
    QTRY_VERIFY_WITH_TIMEOUT(!first->pendingQuestion().isEmpty(), 8000);
    auto *panel = window.findChild<QuestionPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    QCOMPARE(panel->chatId(), QString("first"));
    const auto token = panel->token();
    const auto original = questionPart(*first);
    QVERIFY(!original.isEmpty());
    QVERIFY(
        original.value("toolCallId").toString().contains("/stagewise-mcp:"));
    const auto partId = original.value("id").toString();
    QTRY_VERIFY(renderedPart(window, partId));
    QVERIFY(select(window, "second"));
    QTRY_VERIFY(!panel->isVisible());
    panel->submitStep("first", token, 0, {{"answer", "foreign-visible-chat"}});
    QCOMPARE(first->pendingQuestion().value("id").toString(), token);
    QVERIFY(select(window, "first"));
    QTRY_VERIFY(panel->isVisible());
    QTRY_VERIFY(answer(panel, "answer", "accepted in current owner"));
    QTRY_VERIFY(currentButton(panel, "questionSubmit"));
    QVERIFY(click(panel, "questionSubmit"));
    QTRY_VERIFY_WITH_TIMEOUT(!first->isBusy(), 8000);
    QVERIFY(first->lastTurnSucceeded());
    const auto part = questionPart(*first);
    QCOMPARE(part.value("id").toString(), partId);
    QCOMPARE(part.value("state").toString(), QString("output-available"));
    const auto result =
        QJsonDocument::fromJson(part.value("output").toString().toUtf8())
            .object();
    QCOMPARE(result.value("completedSteps").toInt(), 1);
    QVERIFY(result.value("completed").toBool());
    QVERIFY(!result.value("cancelled").toBool());
    QCOMPARE(result.value("answers").toObject().value("answer").toString(),
             QString("accepted in current owner"));
    QTRY_VERIFY(renderedPart(window, partId));
    QCOMPARE(renderedPart(window, partId)->property("displayState").toString(),
             QString("output-available"));
    const auto saved = stored(dir.path(), "first");
    QCOMPARE(AgentEngine::displayHistory(saved.messages),
             first->displayMessages());
    const auto bytes = QJsonDocument(saved.messages).toJson();
    QVERIFY(!bytes.contains("foreign-visible-chat"));
    QVERIFY(!bytes.contains("STAGEWISE_MCP_CALLBACK_TOKEN"));
    QVERIFY(!bytes.contains("/tools/request-user-input"));
    // Fixture's final assistant text is the response from the actual
    // self-spawned MCP helper.
    const auto delivered = QJsonDocument::fromJson(saved.messages.last()
                                                       .toObject()
                                                       .value("content")
                                                       .toString()
                                                       .toUtf8())
                               .object();
    QCOMPARE(delivered.value("structuredContent")
                 .toObject()
                 .value("answers")
                 .toObject()
                 .value("answer")
                 .toString(),
             QString("accepted in current owner"));
  }
  void mcpPartialCancelAndStopKeepCompletedSteps_data() {
    QTest::addColumn<bool>("stop");
    QTest::newRow("form-cancel") << false;
    QTest::newRow("agent-stop") << true;
  }
  void mcpPartialCancelAndStopKeepCompletedSteps() {
    QFETCH(bool, stop);
    qputenv("ADE_MCP_SCENARIO", "two-step");
    QTemporaryDir dir;
    QVERIFY(seed(dir.path()));
    MainWindow window(dir.path());
    window.resize(1100, 750);
    window.show();
    QVERIFY(select(window, "first"));
    send(window, "Two-step native form");
    QTRY_VERIFY(engine(window, "first"));
    auto *first = engine(window, "first");
    QTRY_VERIFY_WITH_TIMEOUT(!first->pendingQuestion().isEmpty(), 8000);
    auto *panel = window.findChild<QuestionPanel *>();
    QVERIFY(panel);
    const auto oldToken = panel->token();
    const auto partId = questionPart(*first).value("id").toString();
    QTRY_VERIFY(answer(panel, "answer", "completed step"));
    QTRY_VERIFY(currentButton(panel, "questionSubmit"));
    QVERIFY(click(panel, "questionSubmit"));
    QTRY_COMPARE(first->pendingQuestion().value("currentStep").toInt(), 1);
    QTRY_VERIFY(panel->token() != oldToken);
    panel->submitStep("first", oldToken, 1, {{"second", "retired token"}});
    QVERIFY(first->isBusy());
    QTRY_VERIFY(answer(panel, "second", "retained partial draft"));
    if (stop) {
      QTRY_VERIFY(currentButton(panel, "questionWriteMessage"));
      QVERIFY(click(panel, "questionWriteMessage"));
      auto *button = window.findChild<QToolButton *>("stopButton");
      QVERIFY(button);
      QTRY_VERIFY(button->isVisible() && button->isEnabled());
      QTest::mouseClick(button, Qt::LeftButton);
    } else {
      QTRY_VERIFY(currentButton(panel, "questionCancel"));
      QVERIFY(click(panel, "questionCancel"));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!first->isBusy(), 8000);
    QCOMPARE(first->lastTurnSucceeded(), !stop);
    QVERIFY(first->pendingQuestion().isEmpty());
    QVERIFY(!first->submitQuestionStep(oldToken, 1, {{"second", "late"}}));
    const auto part = questionPart(*first);
    QCOMPARE(part.value("id").toString(), partId);
    const auto result =
        QJsonDocument::fromJson(part.value("output").toString().toUtf8())
            .object();
    QCOMPARE(result.value("completedSteps").toInt(), 1);
    QVERIFY(!result.value("completed").toBool());
    QVERIFY(result.value("cancelled").toBool());
    QCOMPARE(result.value("answers").toObject().value("answer").toString(),
             QString("completed step"));
    // Upstream toolbox.cancelUserQuestion and cleanupQuestionsForAgent pass no
    // draft answers: Cancel and Stop return submitted steps only.
    QVERIFY(!result.value("answers").toObject().contains("second"));
    QVERIFY(!QJsonDocument(stored(dir.path(), "first").messages)
                 .toJson()
                 .contains("retired token"));
    QTRY_VERIFY(renderedPart(window, partId));
    QCOMPARE(renderedPart(window, partId)->property("displayState").toString(),
             stop ? QString("canceled") : QString("output-available"));
  }
  void claudeUsesActualHostTheme_data() {
    QTest::addColumn<QString>("appearance");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void claudeUsesActualHostTheme() {
    QFETCH(QString, appearance);
    QTemporaryDir dir;
    QVERIFY(seed(dir.path(), true));
    qputenv("ADE_CLAUDE_FIXTURE", "question-turn");
    MainWindow window(dir.path());
    window.resize(1100, 750);
    window.show();
    auto *settings = window.findChild<SettingsPage *>();
    QVERIFY(settings);
    QVERIFY(settings->setPreference("personalization.appearance", appearance));
    const bool dark = appearance == "dark";
    QTRY_COMPARE(qApp->palette().color(QPalette::Base),
                 dark ? QColor("#21242b") : QColor("#fdfdfd"));
    QVERIFY(select(window, "first"));
    send(window, "Show the actual themed Claude question");
    QTRY_VERIFY(engine(window, "first"));
    auto *first = engine(window, "first");
    QTRY_VERIFY_WITH_TIMEOUT(!first->pendingQuestion().isEmpty(), 8000);
    auto *panel = window.findChild<QuestionPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    auto *submit = panel->findChild<QPushButton *>("questionSubmit");
    QVERIFY(submit);
    QTRY_VERIFY(!submit->visibleRegion().isEmpty());
    QTRY_VERIFY(formTextMeasured(panel));
    QVERIFY(!window.findChild<Composer *>()->isVisible());
    const auto captureDir = qEnvironmentVariable("ADE_HARNESS_FORM_CAPTURES");
    if (!captureDir.isEmpty()) {
      QVERIFY(QDir().mkpath(captureDir));
      QVERIFY(window.grab().save(
          QDir(captureDir).filePath("claude-host-" + appearance + ".png")));
    }
    {
      ProviderDialog dialog({provider(true)}, {}, &window);
      dialog.resize(880, 680);
      dialog.show();
      QLabel *title = nullptr;
      for (auto *label : dialog.findChildren<QLabel *>())
        if (label->text() == "Models & Providers") {
          title = label;
          break;
        }
      QVERIFY(title);
      QTRY_VERIFY(title->isVisible());
      auto *fetch = dialog.findChild<QPushButton *>("connectClaude");
      QVERIFY(fetch && fetch->isEnabled());
      QTRY_VERIFY(!fetch->visibleRegion().isEmpty());
      const auto foreground = title->palette().color(QPalette::WindowText);
      const auto image = dialog.grab().toImage();
      const auto background = image.pixelColor(3, 3);
      QVERIFY2(contrast(foreground, background) >= 4.5,
               qPrintable(QString("heading=%1 background=%2")
                              .arg(foreground.name(), background.name())));
      const auto crop = title->grab().toImage();
      int visiblePixels = 0;
      for (int y = 0; y < crop.height(); ++y)
        for (int x = 0; x < crop.width(); ++x) {
          const auto pixel = crop.pixelColor(x, y);
          if (qAbs(pixel.red() - foreground.red()) < 35 &&
              qAbs(pixel.green() - foreground.green()) < 35 &&
              qAbs(pixel.blue() - foreground.blue()) < 35 &&
              contrast(pixel, background) > 3)
            ++visiblePixels;
        }
      QVERIFY(visiblePixels > 20);
      if (!captureDir.isEmpty())
        QVERIFY(image.save(QDir(captureDir)
                               .filePath("claude-provider-host-theme-" +
                                         appearance + ".png")));
    }
    QTRY_VERIFY(currentButton(panel, "questionCancel"));
    QVERIFY(click(panel, "questionCancel"));
    QTRY_VERIFY_WITH_TIMEOUT(!first->isBusy(), 8000);
  }
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--question-mcp-stdio")
    return runQuestionMcpStdio(argc, argv,
                               QuestionForm::toolDefinition()
                                   .value("function")
                                   .toObject()
                                   .value("parameters")
                                   .toObject());
  QApplication app(argc, argv);
  HarnessFormsHostTest tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "test_harness_forms_host.moc"
