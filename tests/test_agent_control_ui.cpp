#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/MessageQueuePanel.h"
#include "ui/QuestionPanel.h"
#include "ui/SessionStore.h"
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
QByteArray chunk(const QJsonObject &delta, const QString &reason = {}) {
  const QJsonObject choice{
      {"delta", delta},
      {"finish_reason", reason.isEmpty() ? QJsonValue() : QJsonValue(reason)}};
  return "data: " +
         QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}})
             .toJson(QJsonDocument::Compact) +
         "\n\n";
}
class Provider : public QObject {
public:
  QTcpServer server;
  QList<QJsonObject> requests;
  QList<QPointer<QTcpSocket>> sockets;
  Provider() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          if (socket->property("done").toBool()) {
            socket->readAll();
            return;
          }
          const auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          const int split = bytes.indexOf("\r\n\r\n");
          if (split < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(split).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < split + 4 + length)
            return;
          socket->setProperty("done", true);
          requests.append(
              QJsonDocument::fromJson(bytes.mid(split + 4, length)).object());
          sockets.append(socket);
        });
      }
    });
  }
  bool seed(const QString &directory) {
    ProviderConfig provider{
        "fixture",
        "Fixture",
        QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
        "UI-FAKE-TOKEN",
        "fixture-model",
        "openai"};
    if (!ProviderStore::save(directory + "/providers.json", {provider}))
      return false;
    QList<Conversation> chats;
    for (const auto &id : {"first", "second"}) {
      Conversation chat;
      chat.id = id;
      chat.title = id;
      chat.titleLocked = true;
      chat.providerId = provider.id;
      chat.model = provider.model;
      chats.append(chat);
    }
    return SessionStore(directory).save(chats);
  }
  void reply(int index, const QByteArray &body) {
    auto socket = sockets.value(index);
    if (!socket)
      return;
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                  "text/event-stream\r\nConnection: close\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
  }
  void finish(int index) {
    reply(index, chunk({{"content", "DONE"}}, "stop") + "data: [DONE]\n\n");
  }
  void question(int index) {
    const QJsonObject field{{"questionId", "name"},
                            {"type", "input"},
                            {"label", "Project name"},
                            {"required", true}};
    const QJsonObject form{
        {"title", "Project details"},
        {"steps", QJsonArray{QJsonObject{{"fields", QJsonArray{field}}}}}};
    const QJsonObject call{
        {"index", 0},
        {"id", "question-call"},
        {"type", "function"},
        {"function",
         QJsonObject{{"name", "askUserQuestions"},
                     {"arguments", QString::fromUtf8(QJsonDocument(form).toJson(
                                       QJsonDocument::Compact))}}}};
    reply(index, chunk({{"tool_calls", QJsonArray{call}}}, "tool_calls") +
                     "data: [DONE]\n\n");
  }
};
bool select(MainWindow &window, const QString &id) {
  auto *list = window.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole).toString() == id) {
      list->setCurrentItem(list->item(i));
      return window.currentSessionId() == id;
    }
  return false;
}
Conversation saved(const QString &path, const QString &id) {
  for (const auto &chat : SessionStore(path).load())
    if (chat.id == id)
      return chat;
  return {};
}
} // namespace
class AgentControlUiTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { initializeAppStyle(*qApp); }
  void busySendPersistsQueueAndStopRestartRequiresResume() {
    QTemporaryDir directory;
    Provider provider;
    QVERIFY(provider.seed(directory.path()));
    {
      MainWindow window(directory.path());
      window.show();
      QVERIFY(select(window, "first"));
      window.submitPrompt("first turn");
      QTRY_COMPARE_WITH_TIMEOUT(provider.requests.size(), 1, 5000);
      window.submitPrompt("queued turn");
      auto *composer = window.findChild<Composer *>();
      QCOMPARE(composer->toPlainText(), QString());
      QCOMPARE(saved(directory.path(), "first").queuedMessages.size(), 1);
      QVERIFY(window.findChild<MessageQueuePanel *>()->isVisible());
      window.findChild<QToolButton *>("stopButton")->click();
      QTRY_VERIFY(!window.findChild<AgentEngine *>()->isBusy());
      QCOMPARE(saved(directory.path(), "first").queuedMessages.size(), 1);
    }
    MainWindow restored(directory.path());
    restored.show();
    QVERIFY(select(restored, "first"));
    QTest::qWait(50);
    QCOMPARE(provider.requests.size(), 1);
    auto *resume = restored.findChild<QPushButton *>("queueResume");
    QVERIFY(resume);
    resume->click();
    QTRY_COMPARE_WITH_TIMEOUT(provider.requests.size(), 2, 5000);
    QVERIFY(saved(directory.path(), "first").queuedMessages.isEmpty());
    QVERIFY(
        QJsonDocument(provider.requests[1]).toJson().contains("queued turn"));
    provider.finish(1);
    QTRY_VERIFY(!restored.findChild<AgentEngine *>()->isBusy());
  }
  void nativeQuestionCanBeAnsweredAndCannotCrossChat() {
    QTemporaryDir directory;
    Provider provider;
    QVERIFY(provider.seed(directory.path()));
    MainWindow window(directory.path());
    window.show();
    QVERIFY(select(window, "first"));
    window.submitPrompt("Ask details");
    QTRY_COMPARE(provider.requests.size(), 1);
    provider.question(0);
    auto *panel = window.findChild<QuestionPanel *>();
    QVERIFY(panel);
    QTRY_VERIFY(panel->isVisible());
    const auto token = panel->token();
    auto *engine = window.findChild<AgentEngine *>();
    QVERIFY(engine);
    QVERIFY(select(window, "second"));
    panel->submitStep("first", token, 0, {{"name", "foreign action"}});
    QCOMPARE(engine->pendingQuestion()["id"].toString(), token);
    QCOMPARE(provider.requests.size(), 1);
    QVERIFY(select(window, "first"));
    QTRY_VERIFY(panel->isVisible());
    panel->findChild<QLineEdit *>("answer_name")->setText("Native form answer");
    panel->findChild<QPushButton *>("questionSubmit")->click();
    QTRY_COMPARE(provider.requests.size(), 2);
    QVERIFY(QJsonDocument(provider.requests[1])
                .toJson()
                .contains("Native form answer"));
    QVERIFY(engine->pendingQuestion().isEmpty());
    provider.finish(1);
    QTRY_VERIFY(!engine->isBusy());
  }
  void writeMessageInsteadPreservesPartialAnswersAndFitsNarrowWindow() {
    QTemporaryDir directory;
    Provider provider;
    QVERIFY(provider.seed(directory.path()));
    MainWindow window(directory.path());
    window.resize(350, 250);
    window.show();
    QVERIFY(select(window, "first"));
    window.submitPrompt("Ask details");
    QTRY_COMPARE(provider.requests.size(), 1);
    provider.question(0);
    auto *panel = window.findChild<QuestionPanel *>();
    QTRY_VERIFY(panel->isVisible());
    panel->findChild<QLineEdit *>("answer_name")->setText("partial value");
    auto *scroll = window.findChild<QScrollArea *>("agentControlsScroll");
    QVERIFY(scroll);
    QVERIFY(scroll->height() <= window.height());
    QVERIFY(window.width() <= 360);
    QVERIFY(window.height() <= 260);
    panel->findChild<QPushButton *>("questionWriteMessage")->click();
    QVERIFY(window.findChild<Composer *>()->isVisible());
    window.submitPrompt("Use defaults instead");
    QTRY_COMPARE(provider.requests.size(), 2);
    const auto request = QJsonDocument(provider.requests[1]).toJson();
    QVERIFY(request.contains("partial value"));
    QVERIFY(request.contains("Use defaults instead"));
    QVERIFY(request.contains("User sent a message"));
    provider.finish(1);
    QTRY_VERIFY(!window.findChild<AgentEngine *>()->isBusy());
    const auto render = qEnvironmentVariable("ADE_CONTROL_RENDER");
    if (!render.isEmpty())
      QVERIFY(window.grab().save(render));
  }
  void questionInputAndSubmissionUseAvailableHeight() {
    QTemporaryDir directory;
    Provider provider;
    QVERIFY(provider.seed(directory.path()));
    MainWindow window(directory.path());
    window.resize(350, 750);
    window.show();
    QVERIFY(select(window, "first"));
    window.submitPrompt("Ask details");
    QTRY_COMPARE(provider.requests.size(), 1);
    provider.question(0);
    auto *panel = window.findChild<QuestionPanel *>();
    QTRY_VERIFY(panel->isVisible());
    auto *scroll = window.findChild<QScrollArea *>("agentControlsScroll");
    auto *answer = panel->findChild<QLineEdit *>("answer_name");
    auto *submit = panel->findChild<QPushButton *>("questionSubmit");
    QVERIFY(scroll && answer && submit);
    auto visible = [&](QWidget *control) {
      return scroll->viewport()->rect().contains(
          control->mapTo(scroll->viewport(), control->rect().center()));
    };
    QTRY_VERIFY(visible(answer));
    QTRY_VERIFY(visible(submit));
    window.resize(350, 250);
    QTRY_COMPARE(window.height(), 250);
    QVERIFY(scroll->height() <= 150);
    QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
    scroll->ensureWidgetVisible(submit);
    QTRY_VERIFY(visible(submit));
    answer->setText("Usable compact form");
    submit->click();
    QTRY_COMPARE(provider.requests.size(), 2);
    QVERIFY(QJsonDocument(provider.requests[1]).toJson().contains("Usable compact form"));
    provider.finish(1);
    QTRY_VERIFY(!window.findChild<AgentEngine *>()->isBusy());
  }
  void saveConflictKeepsDraftAndDoesNotEnqueue() {
    QTemporaryDir directory;
    Provider provider;
    QVERIFY(provider.seed(directory.path()));
    MainWindow window(directory.path());
    QVERIFY(select(window, "first"));
    window.submitPrompt("first");
    QTRY_COMPARE(provider.requests.size(), 1);
    SessionStore external(directory.path());
    auto chats = external.load();
    chats[0].title = "External update";
    QVERIFY(external.save(chats));
    window.submitPrompt("must remain draft");
    QCOMPARE(window.findChild<Composer *>()->toPlainText(),
             "must remain draft");
    auto *engine = window.findChild<AgentEngine *>();
    QVERIFY(engine->queuedMessages().isEmpty());
    QCOMPARE(provider.requests.size(), 1);
    engine->cancel();
  }
};
QTEST_MAIN(AgentControlUiTest)
#include "test_agent_control_ui.moc"
