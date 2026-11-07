#include "core/AgentEngine.h"
#include "core/QuestionForm.h"
#include "core/QueuedMessages.h"
#include "ui/SessionStore.h"
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray sseEvent(const QJsonObject &object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QJsonObject form(bool multiple = false) {
  QJsonArray steps{
      QJsonObject{{"fields", QJsonArray{QJsonObject{{"type", "input"},
                                                    {"questionId", "name"},
                                                    {"label", "Project name"},
                                                    {"required", true},
                                                    {"minLength", 2}}}}}};
  if (multiple)
    steps.append(QJsonObject{
        {"fields",
         QJsonArray{QJsonObject{
             {"type", "radio-group"},
             {"questionId", "mode"},
             {"label", "Mode"},
             {"required", true},
             {"options", QJsonArray{QJsonObject{{"value", "fast"},
                                                {"label", "Fast"}}}}}}}});
  return {{"title", "Project details"}, {"steps", steps}};
}
class Server : public QObject {
public:
  QTcpServer server;
  QString protocol = "openai";
  QList<QJsonObject> requests;
  QList<QPointer<QTcpSocket>> sockets;
  Server() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          if (socket->property("handled").toBool()) {
            socket->readAll();
            return;
          }
          const auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          const int end = bytes.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(end).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < end + 4 + length)
            return;
          socket->setProperty("handled", true);
          requests.append(
              QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
          sockets.append(socket);
        });
      }
    });
  }
  ProviderConfig provider() const {
    return {"fixture",
            "Fixture",
            QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
            "FAKE-KEY",
            "fixture-model",
            protocol};
  }
  void reply(int index, const QByteArray &body, int status = 200) {
    auto socket = sockets.value(index);
    if (!socket)
      return;
    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                  " Fixture\r\nContent-Type: text/event-stream\r\nConnection: "
                  "close\r\nContent-Length: " +
                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
    socket->disconnectFromHost();
  }
  void finish(int index) {
    QByteArray body;
    if (protocol == "google")
      body = sseEvent(
          {{"candidates",
            QJsonArray{QJsonObject{
                {"content", QJsonObject{{"role", "model"},
                                        {"parts", QJsonArray{QJsonObject{
                                                      {"text", "DONE"}}}}}},
                {"finishReason", "STOP"}}}}});
    else if (protocol == "anthropic")
      body = sseEvent({{"type", "content_block_start"},
                       {"index", 0},
                       {"content_block",
                        QJsonObject{{"type", "text"}, {"text", "DONE"}}}}) +
             sseEvent({{"type", "message_delta"},
                       {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}) +
             sseEvent({{"type", "message_stop"}});
    else if (protocol == "responses")
      body = sseEvent(
          {{"type", "response.completed"},
           {"response",
            QJsonObject{{"status", "completed"},
                        {"output",
                         QJsonArray{QJsonObject{
                             {"type", "message"},
                             {"role", "assistant"},
                             {"content",
                              QJsonArray{QJsonObject{{"type", "output_text"},
                                                     {"text", "DONE"}}}}}}}}}});
    else
      body =
          sseEvent({{"choices", QJsonArray{QJsonObject{
                                    {"delta", QJsonObject{{"content", "DONE"}}},
                                    {"finish_reason", "stop"}}}}}) +
          "data: [DONE]\n\n";
    reply(index, body);
  }
  void question(int index, const QJsonObject &definition = form(),
                const QString &callId = "same-call") {
    const auto args = QString::fromUtf8(
        QJsonDocument(definition).toJson(QJsonDocument::Compact));
    QByteArray body;
    if (protocol == "google")
      body = sseEvent(
          {{"candidates",
            QJsonArray{QJsonObject{
                {"content",
                 QJsonObject{
                     {"role", "model"},
                     {"parts", QJsonArray{QJsonObject{
                                   {"functionCall",
                                    QJsonObject{{"id", callId},
                                                {"name", "askUserQuestions"},
                                                {"args", definition}}},
                                   {"thoughtSignature", "signed-fixture"}}}}}},
                {"finishReason", "STOP"}}}}});
    else if (protocol == "anthropic")
      body =
          sseEvent({{"type", "content_block_start"},
                    {"index", 0},
                    {"content_block", QJsonObject{{"type", "tool_use"},
                                                  {"id", callId},
                                                  {"name", "askUserQuestions"},
                                                  {"input", QJsonObject{}}}}}) +
          sseEvent({{"type", "content_block_delta"},
                    {"index", 0},
                    {"delta", QJsonObject{{"type", "input_json_delta"},
                                          {"partial_json",
                                           args.left(args.size() / 2)}}}}) +
          sseEvent({{"type", "content_block_delta"},
                    {"index", 0},
                    {"delta", QJsonObject{{"type", "input_json_delta"},
                                          {"partial_json",
                                           args.mid(args.size() / 2)}}}}) +
          sseEvent({{"type", "content_block_stop"}, {"index", 0}}) +
          sseEvent({{"type", "message_delta"},
                    {"delta", QJsonObject{{"stop_reason", "tool_use"}}}}) +
          sseEvent({{"type", "message_stop"}});
    else if (protocol == "responses")
      body = sseEvent({{"type", "response.output_item.added"},
                       {"output_index", 0},
                       {"item", QJsonObject{{"type", "function_call"},
                                            {"id", "native-item"},
                                            {"call_id", callId},
                                            {"name", "askUserQuestions"},
                                            {"arguments", ""}}}}) +
             sseEvent({{"type", "response.function_call_arguments.delta"},
                       {"output_index", 0},
                       {"delta", args.left(args.size() / 2)}}) +
             sseEvent({{"type", "response.function_call_arguments.delta"},
                       {"output_index", 0},
                       {"delta", args.mid(args.size() / 2)}}) +
             sseEvent({{"type", "response.completed"},
                       {"response",
                        QJsonObject{{"status", "completed"},
                                    {"output", QJsonArray{QJsonObject{
                                                   {"type", "function_call"},
                                                   {"id", "native-item"},
                                                   {"call_id", callId},
                                                   {"name", "askUserQuestions"},
                                                   {"arguments", args}}}}}}});
    else {
      for (int i = 0; i < 2; ++i) {
        QJsonObject fn{{"arguments", i ? args.mid(args.size() / 2)
                                       : args.left(args.size() / 2)}};
        QJsonObject call{{"index", 0}, {"function", fn}};
        if (!i) {
          fn["name"] = "askUserQuestions";
          call["function"] = fn;
          call["id"] = callId;
          call["type"] = "function";
        }
        const QJsonObject choice{
            {"delta", QJsonObject{{"tool_calls", QJsonArray{call}}}},
            {"finish_reason", i ? QJsonValue("tool_calls") : QJsonValue()}};
        body += sseEvent({{"choices", QJsonArray{choice}}});
      }
      body += "data: [DONE]\n\n";
    }
    reply(index, body);
  }
};
QJsonObject toolResult(const AgentEngine &engine) {
  const auto history = engine.messages();
  for (int i = history.size() - 1; i >= 0; --i)
    if (history[i].toObject()["role"] == "tool")
      return QJsonDocument::fromJson(
                 history[i].toObject()["content"].toString().toUtf8())
          .object();
  return {};
}
QStringList userTexts(const QJsonArray &history) {
  QStringList out;
  for (const auto &value : history)
    if (value.toObject()["role"] == "user")
      out.append(value.toObject()["content"].toString());
  return out;
}
} // namespace
class ConversationControlTest : public QObject {
  Q_OBJECT
private slots:
  void queueEditsAreDurableAndDrainInOrder() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QTemporaryDir dir;
    SessionStore store(dir.path());
    Conversation chat;
    chat.id = "chat";
    chat.title = "Fixture";
    QList<Conversation> chats{chat};
    QVERIFY(store.save(chats));
    engine.setQueueCommitter([&](const QJsonArray &history,
                                 const QJsonArray &queue, QString *error) {
      auto candidate = chats;
      candidate[0].messages = history;
      candidate[0].queuedMessages = queue;
      if (!store.save(candidate, error))
        return false;
      chats = candidate;
      return true;
    });
    QVERIFY(engine.sendUserMessage("first"));
    QTRY_COMPARE(server.requests.size(), 1);
    const QJsonArray refs{QJsonObject{{"path", "att/blob.txt"},
                                      {"originalFileName", "original.txt"}}};
    const QJsonObject refsMeta{{"att/blob.txt", QJsonArray{1, 4}}},
        pathMeta{{"att/blob.txt", QJsonObject{{"mtime", 123}}}};
    QVERIFY(engine.sendUserMessage("second", refs, refsMeta, pathMeta));
    QVERIFY(engine.sendUserMessage("third"));
    const auto secondId =
                   engine.queuedMessages()[0].toObject()["id"].toString(),
               thirdId = engine.queuedMessages()[1].toObject()["id"].toString();
    QVERIFY(engine.editQueuedMessage(secondId, "second edited"));
    QVERIFY(engine.moveQueuedMessage(thirdId, 0));
    QCOMPARE(SessionStore(dir.path()).load()[0].queuedMessages,
             engine.queuedMessages());
    server.finish(0);
    QTRY_COMPARE(server.requests.size(), 2);
    QCOMPARE(userTexts(engine.messages()), QStringList({"first", "third"}));
    server.finish(1);
    QTRY_COMPARE(server.requests.size(), 3);
    QCOMPARE(userTexts(engine.messages()),
             QStringList({"first", "third", "second edited"}));
    const auto last = engine.messages().last().toObject();
    QCOMPARE(last["metadata"].toObject()["attachments"].toArray(), refs);
    QCOMPARE(last["metadata"].toObject()["pathReferences"].toObject(),
             refsMeta);
    QCOMPARE(last["metadata"].toObject()["pathMetadata"].toObject(), pathMeta);
    QVERIFY(!last.contains("id"));
    QVERIFY(!QJsonDocument(server.requests.last())
                 .toJson()
                 .contains(secondId.toUtf8()));
    server.finish(2);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.queuedMessages().isEmpty());
    QCOMPARE(SessionStore(dir.path()).load()[0].queuedMessages, QJsonArray{});
  }
  void stopAndRestartKeepQueuePaused() {
    Server server;
    AgentEngine original;
    original.setProvider(server.provider());
    QVERIFY(original.sendUserMessage("working"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(original.sendUserMessage("kept"));
    original.cancel();
    AgentEngine restored;
    restored.setProvider(server.provider());
    restored.setMessages(original.messages());
    QVERIFY(restored.setQueuedMessages(original.queuedMessages()));
    QTest::qWait(40);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(restored.resumeQueue());
    QTRY_COMPARE(server.requests.size(), 2);
    QCOMPARE(userTexts(restored.messages()).last(), "kept");
    server.finish(1);
    QTRY_VERIFY(!restored.isBusy());
  }
  void failurePreservesQueueAndDoesNotRetryIt() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("working"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("later"));
    QSignalSpy failed(&engine, &AgentEngine::failed);
    server.reply(0, "{\"error\":{\"message\":\"fixture unauthorized\"}}", 401);
    QTRY_COMPARE(failed.size(), 1);
    QVERIFY(!engine.isBusy());
    QCOMPARE(engine.queuedMessages().size(), 1);
    QTest::qWait(40);
    QCOMPARE(server.requests.size(), 1);
  }
  void failedSaveDoesNotDropOrSend() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    bool deny = false;
    engine.setQueueCommitter(
        [&](const QJsonArray &, const QJsonArray &, QString *error) {
          if (deny) {
            *error = "fixture conflict";
            return false;
          }
          return true;
        });
    QVERIFY(engine.sendUserMessage("first"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("second"));
    const auto queue = engine.queuedMessages();
    deny = true;
    QSignalSpy errors(&engine, &AgentEngine::queueError);
    QVERIFY(!engine.removeQueuedMessage(queue[0].toObject()["id"].toString()));
    QCOMPARE(engine.queuedMessages(), queue);
    server.finish(0);
    QTRY_VERIFY(errors.size() >= 2);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(engine.queuedMessages(), queue);
    QCOMPARE(userTexts(engine.messages()), QStringList{"first"});
  }
  void idleInvalidHistoryAndFailedCommitDoNotAcceptOrQueue() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    const QJsonArray malformed{QJsonObject{
        {"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "old"}}};
    engine.setMessages(malformed);
    int commits = 0;
    engine.setQueueCommitter(
        [&](const QJsonArray &, const QJsonArray &, QString *) {
          ++commits;
          return true;
        });
    QVERIFY(!engine.sendUserMessage("draft"));
    QCOMPARE(engine.messages(), malformed);
    QVERIFY(engine.queuedMessages().isEmpty());
    QCOMPARE(commits, 0);
    engine.setMessages({});
    engine.setQueueCommitter(
        [](const QJsonArray &, const QJsonArray &, QString *error) {
          *error = "save denied";
          return false;
        });
    QVERIFY(!engine.sendUserMessage("draft"));
    QVERIFY(engine.messages().isEmpty());
    QVERIFY(engine.queuedMessages().isEmpty());
    QTest::qWait(25);
    QCOMPARE(server.requests.size(), 0);
  }
  void sendNowCancelsCurrentAndPrioritizesSelected() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("working"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("second"));
    QVERIFY(engine.sendUserMessage("third"));
    const auto id = engine.queuedMessages()[1].toObject()["id"].toString();
    QVERIFY(engine.flushQueuedMessage(id));
    QTRY_COMPARE(server.requests.size(), 2);
    QCOMPARE(userTexts(engine.messages()).last(), "third");
    QCOMPARE(engine.queuedMessages().size(), 1);
    engine.cancel();
  }
  void stoppedAtFinishedCancelsScheduledDrain() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("first"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("second"));
    connect(&engine, &AgentEngine::finished, &engine, &AgentEngine::cancel);
    server.finish(0);
    QTRY_VERIFY(!engine.isBusy());
    QTest::qWait(40);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(engine.queuedMessages().size(), 1);
  }
  void contextHookRunsForEachAcceptedTurn() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    connect(&engine, &AgentEngine::turnAboutToStart, &engine,
            [&](const QJsonObject &row) {
              engine.setSystemContext("CONTEXT:" + row["content"].toString());
            });
    QVERIFY(engine.sendUserMessage("one"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("two"));
    server.finish(0);
    QTRY_COMPARE(server.requests.size(), 2);
    QVERIFY(QJsonDocument(server.requests[0]).toJson().contains("CONTEXT:one"));
    QVERIFY(QJsonDocument(server.requests[1]).toJson().contains("CONTEXT:two"));
    QVERIFY(
        !QJsonDocument(server.requests[1]).toJson().contains("CONTEXT:one"));
    engine.cancel();
  }
  void appMountMayRefreshOnlyInsideActualTurnHook() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QTemporaryDir apps, other;
    bool configured = false;
    connect(&engine, &AgentEngine::turnAboutToStart, &engine,
            [&](const QJsonObject &) {
              configured = engine.setAppsDirectory(apps.path());
            });
    QVERIFY(engine.sendUserMessage("first"));
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(configured);
    QVERIFY(QJsonDocument(server.requests[0]).toJson().contains("write"));
    QVERIFY(!engine.setAppsDirectory(other.path()));
    configured = false;
    QVERIFY(engine.sendUserMessage("queued"));
    server.finish(0);
    QTRY_COMPARE(server.requests.size(), 2);
    QVERIFY(configured);
    engine.cancel();
  }
  void queueObserversCanCancelOrDelete() {
    Server server;
    auto *engine = new AgentEngine;
    engine->setProvider(server.provider());
    QPointer<AgentEngine> guard(engine);
    engine->setQueueCommitter(
        [&](const QJsonArray &, const QJsonArray &, QString *) {
          delete engine;
          return true;
        });
    QVERIFY(!engine->sendUserMessage("deleted"));
    QVERIFY(!guard);
    QCOMPARE(server.requests.size(), 0);
    AgentEngine other;
    other.setProvider(server.provider());
    connect(&other, &AgentEngine::turnAboutToStart, &other,
            &AgentEngine::cancel);
    QVERIFY(other.sendUserMessage("cancel accepted"));
    QVERIFY(!other.isBusy());
    QCOMPARE(userTexts(other.messages()), QStringList{"cancel accepted"});
    QTest::qWait(30);
    QCOMPARE(server.requests.size(), 0);
  }
  void headlessQuestionsAreNotAdvertisedOrWaitedOn_data() {
    QTest::addColumn<QString>("protocol");
    for (const auto &p :
         QStringList{"openai", "anthropic", "google", "responses"})
      QTest::newRow(qPrintable(p)) << p;
  }
  void headlessQuestionsAreNotAdvertisedOrWaitedOn() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QSignalSpy changed(&engine, &AgentEngine::questionChanged);
    QSignalSpy completed(&engine, &AgentEngine::toolFinished);
    engine.submit("Headless request");
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(!QJsonDocument(server.requests[0])
                 .toJson()
                 .contains("askUserQuestions"));
    // A late UI opt-in cannot change capabilities within an accepted turn.
    engine.setQuestionToolEnabled(true);
    server.question(0);
    QTRY_COMPARE(server.requests.size(), 2);
    QCOMPARE(completed.size(), 1);
    QVERIFY(completed[0][2].toBool());
    QVERIFY(completed[0][1].toString().contains("unavailable"));
    QVERIFY(engine.pendingQuestion().isEmpty());
    QVERIFY(changed.isEmpty());
    server.finish(1);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.lastTurnSucceeded());
  }
  void questionCompletesWithExactProtocolAssociation_data() {
    QTest::addColumn<QString>("protocol");
    for (const auto &p :
         QStringList{"openai", "anthropic", "google", "responses"})
      QTest::newRow(qPrintable(p)) << p;
  }
  void questionCompletesWithExactProtocolAssociation() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    AgentEngine engine;
    engine.setQuestionToolEnabled(true);
    engine.setProvider(server.provider());
    QSignalSpy approval(&engine, &AgentEngine::approvalRequired);
    engine.submit("Ask me");
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(QJsonDocument(server.requests[0])
                .toJson()
                .contains("askUserQuestions"));
    server.question(0);
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    QVERIFY(approval.isEmpty());
    const auto before = engine.messages();
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(!engine.submitQuestionStep("wrong", 0, {{"name", "Ada"}}));
    QVERIFY(engine.submitQuestionStep(token, 0, {{"name", "Ada"}}));
    QTRY_COMPARE(server.requests.size(), 2);
    const auto result = toolResult(engine);
    QCOMPARE(result["completed"], true);
    QCOMPARE(result["cancelled"], false);
    QCOMPARE(result["answers"].toObject()["name"], "Ada");
    QCOMPARE(result["completedSteps"], 1);
    QVERIFY(!result.contains("cancelReason"));
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"name", "duplicate"}}));
    auto retained = engine.messages()[1].toObject(),
         original = before[1].toObject();
    QCOMPARE(retained["metadata"].toObject()["display"].toObject()["id"],
             original["metadata"].toObject()["display"].toObject()["id"]);
    QCOMPARE(retained["metadata"]
                 .toObject()["display"]
                 .toObject()["parts"]
                 .toArray()[0]
                 .toObject()["state"],
             "output-available");
    retained.remove("metadata");
    original.remove("metadata");
    QCOMPARE(retained, original);
    const auto bytes = QJsonDocument(server.requests[1]).toJson();
    QVERIFY(bytes.contains("Ada"));
    if (protocol == "openai" || protocol == "responses" ||
        protocol == "anthropic")
      QVERIFY(bytes.contains("same-call"));
    if (protocol == "google")
      QVERIFY(bytes.contains("signed-fixture"));
    server.finish(1);
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(engine.lastTurnSucceeded());
  }
  void questionsValidateBacktrackingAndStaleStepTokens() {
    Server server;
    AgentEngine engine;
    engine.setQuestionToolEnabled(true);
    engine.setProvider(server.provider());
    engine.submit("Ask");
    QTRY_COMPARE(server.requests.size(), 1);
    server.question(0, form(true));
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    const auto first = engine.pendingQuestion()["id"].toString();
    QVERIFY(!engine.submitQuestionStep(first, 0, {{"name", "x"}}));
    QVERIFY(!engine.submitQuestionStep(first, 0,
                                       {{"name", "Ada"}, {"foreign", true}}));
    QVERIFY(engine.submitQuestionStep(first, 0, {{"name", "Ada"}}));
    const auto second = engine.pendingQuestion()["id"].toString();
    QVERIFY(second != first);
    QVERIFY(!engine.submitQuestionStep(first, 1, {{"mode", "fast"}}));
    QVERIFY(engine.goBackQuestion(second, 1));
    const auto back = engine.pendingQuestion()["id"].toString();
    QVERIFY(back != first && back != second);
    QVERIFY(!engine.submitQuestionStep(first, 0, {{"name", "stale"}}));
    QCOMPARE(engine.pendingQuestion()["answers"].toObject()["name"], "Ada");
    QVERIFY(engine.submitQuestionStep(back, 0, {{"name", "Updated"}}));
    const auto final = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.cancelQuestion(final, {{"mode", "__other__:custom"}}));
    QTRY_COMPARE(server.requests.size(), 2);
    const auto result = toolResult(engine);
    QCOMPARE(result["cancelReason"], "user_cancelled");
    QCOMPARE(result["completedSteps"], 1);
    QVERIFY(!result["answers"].toObject().contains("mode"));
    engine.cancel();
  }
  void normalMessageInterruptsQuestionBeforeNextRequest() {
    Server server;
    AgentEngine engine;
    engine.setQuestionToolEnabled(true);
    engine.setProvider(server.provider());
    engine.submit("Ask");
    QTRY_COMPARE(server.requests.size(), 1);
    server.question(0);
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.updateQuestionDraft(token, 0, {{"name", "partial"}}));
    QVERIFY(engine.sendUserMessage("Use defaults instead"));
    QTRY_COMPARE(server.requests.size(), 2);
    const auto result = toolResult(engine);
    QCOMPARE(result["completed"], false);
    QCOMPARE(result["cancelled"], false);
    QVERIFY(!result.contains("cancelReason"));
    QCOMPARE(result["answers"].toObject()["name"], "partial");
    QVERIFY(result["notice"].toString().contains("user message"));
    const auto history = engine.messages();
    QCOMPARE(history[2].toObject()["role"], "tool");
    QCOMPARE(history[3].toObject()["content"], "Use defaults instead");
    QVERIFY(!engine.cancelQuestion(token));
    engine.cancel();
  }
  void stopQuestionUsesStructuredResultAndKeepsQueue() {
    Server server;
    AgentEngine engine;
    engine.setQuestionToolEnabled(true);
    engine.setProvider(server.provider());
    engine.submit("Ask");
    QTRY_COMPARE(server.requests.size(), 1);
    QVERIFY(engine.sendUserMessage("queued"));
    server.question(0);
    QTRY_VERIFY(!engine.pendingQuestion().isEmpty());
    const auto token = engine.pendingQuestion()["id"].toString();
    QVERIFY(engine.updateQuestionDraft(token, 0, {{"name", "unfinished"}}));
    engine.cancel();
    QCOMPARE(toolResult(engine)["cancelReason"], "agent_stopped");
    QCOMPARE(engine.queuedMessages().size(), 1);
    QVERIFY(engine.pendingQuestion().isEmpty());
    QVERIFY(!engine.submitQuestionStep(token, 0, {{"name", "late"}}));
  }
  void reusedToolIdGetsFreshQuestionTokenAndObserversMayDelete() {
    Server server;
    auto *engine = new AgentEngine;
    engine->setQuestionToolEnabled(true);
    QPointer<AgentEngine> guard(engine);
    engine->setProvider(server.provider());
    engine->submit("Ask");
    QTRY_COMPARE(server.requests.size(), 1);
    server.question(0);
    QTRY_VERIFY(!engine->pendingQuestion().isEmpty());
    const auto old = engine->pendingQuestion()["id"].toString();
    QVERIFY(engine->cancelQuestion(old));
    QTRY_COMPARE(server.requests.size(), 2);
    server.question(1);
    QTRY_VERIFY(!engine->pendingQuestion().isEmpty());
    QVERIFY(engine->pendingQuestion()["id"] != old);
    QVERIFY(!engine->cancelQuestion(old));
    connect(engine, &AgentEngine::questionChanged, engine,
            [engine] { delete engine; });
    QVERIFY(engine->cancelQuestion(engine->pendingQuestion()["id"].toString()));
    QVERIFY(!guard);
    QTest::qWait(30);
    QCOMPARE(server.requests.size(), 2);
  }
  void sessionQueueAndOpaqueLinkProvenanceRoundTrip() {
    QTemporaryDir dir;
    SessionStore store(dir.path());
    Conversation chat;
    chat.id = "persisted";
    chat.title = "Persisted";
    chat.queuedMessages = {QueuedMessages::create("queued")};
    chat.transcriptLinkProvenance = {
        {"row-1",
         QJsonObject{{"version", 1},
                     {"mountBindings", QJsonObject{{"ws1", "fixture-root"}}}}}};
    QVERIFY(store.save({chat}));
    const auto loaded = SessionStore(dir.path()).load();
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded[0].queuedMessages, chat.queuedMessages);
    QCOMPARE(loaded[0].transcriptLinkProvenance, chat.transcriptLinkProvenance);
    QFile file(dir.filePath("sessions.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll();
    file.close();
    chat.queuedMessages.append(chat.queuedMessages.first());
    QString error;
    QVERIFY(!store.save({chat}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    chat.queuedMessages = {};
    chat.transcriptLinkProvenance = {{QString(129, 'x'), QJsonObject{}}};
    QVERIFY(!store.save({chat}));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    auto document = QJsonDocument::fromJson(before).object();
    auto sessions = document["sessions"].toArray();
    auto row = sessions[0].toObject();
    row["queuedMessages"] = QJsonArray{QJsonObject{
        {"id", "x"}, {"role", "assistant"}, {"content", "invalid"}}};
    sessions[0] = row;
    document["sessions"] = sessions;
    file.write(QJsonDocument(document).toJson());
    file.close();
    SessionStore corrupt(dir.path());
    QVERIFY(corrupt.load(&error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!corrupt.save({}, &error));
  }
  void questionSchemaBoundsAndAnswerTypes() {
    auto definition = form();
    QString error;
    QVERIFY(QuestionForm::validate(definition, &error));
    auto steps = definition["steps"].toArray();
    auto step = steps[0].toObject();
    auto fields = step["fields"].toArray();
    fields.append(fields.first());
    step["fields"] = fields;
    steps[0] = step;
    definition["steps"] = steps;
    QVERIFY(!QuestionForm::validate(definition, &error));
    const QJsonArray schema{
        QJsonObject{{"type", "input"},
                    {"questionId", "n"},
                    {"inputType", "number"},
                    {"min", 2},
                    {"max", 4},
                    {"required", true}},
        QJsonObject{{"type", "checkbox"}, {"questionId", "b"}}};
    QVERIFY(
        QuestionForm::validateAnswers(schema, {{"n", 3}, {"b", false}}, true));
    QVERIFY(!QuestionForm::validateAnswers(schema, {{"n", "NaN"}}, true));
    QVERIFY(!QuestionForm::validateAnswers(schema, {{"n", 5}}, true));
    QVERIFY(!QuestionForm::validateAnswers(schema, {{"n", 3}, {"b", "false"}},
                                           true));
  }
};
QTEST_GUILESS_MAIN(ConversationControlTest)
#include "test_conversation_control.moc"
