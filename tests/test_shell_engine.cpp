#include "core/AgentEngine.h"
#include "core/AgentPresentation.h"
#include "core/ProviderProtocol.h"
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
using namespace ade;
namespace {
QByteArray event(const QJsonObject &object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QByteArray chat(const QJsonObject &delta,
                const QJsonValue &finish = QJsonValue::Null) {
  return event(
      {{"choices",
        QJsonArray{QJsonObject{{"delta", delta}, {"finish_reason", finish}}}}});
}
QJsonObject call(const QString &id = "same-call",
                 const QString &arguments = "{\"x\":1}") {
  return {{"index", 0},
          {"id", id},
          {"type", "function"},
          {"function",
           QJsonObject{{"name", "inspectFixture"}, {"arguments", arguments}}}};
}
QJsonObject responseItem(const QString &type, const QString &text) {
  if (type == "reasoning")
    return {{"id", "reason-1"},
            {"type", "reasoning"},
            {"encrypted_content", "SIGNED_FIXTURE"},
            {"summary", QJsonArray{QJsonObject{{"type", "summary_text"},
                                               {"text", text}}}}};
  return {{"id", "message-1"},
          {"type", "message"},
          {"role", "assistant"},
          {"content",
           QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}}}}};
}
QJsonObject responseCall() {
  return {{"id", "item-1"},
          {"type", "function_call"},
          {"call_id", "same-call"},
          {"name", "inspectFixture"},
          {"arguments", "{\"x\":1}"}};
}
QList<QByteArray> round(const QString &protocol, bool tool = true) {
  if (protocol == "openai") {
    if (!tool)
      return {chat({{"content", "Final"}}, "stop"), "data: [DONE]\n\n"};
    auto partial = call("same-call", "{\"x\":");
    auto rest = call("", "1}");
    rest.remove("id");
    auto fn = rest["function"].toObject();
    fn.remove("name");
    rest["function"] = fn;
    return {chat({{"reasoning_content", "Think"}}),
            chat({{"content", "Before"}}),
            chat({{"tool_calls", QJsonArray{partial}}}),
            chat({{"tool_calls", QJsonArray{rest}}}, "tool_calls"),
            "data: [DONE]\n\n"};
  }
  if (protocol == "anthropic") {
    if (!tool)
      return {event({{"type", "content_block_start"},
                     {"index", 0},
                     {"content_block",
                      QJsonObject{{"type", "text"}, {"text", "Final"}}}}),
              event({{"type", "message_delta"},
                     {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}),
              event({{"type", "message_stop"}})};
    return {event({{"type", "content_block_start"},
                   {"index", 0},
                   {"content_block",
                    QJsonObject{{"type", "thinking"},
                                {"thinking", "Think"},
                                {"signature", "SIGNED_FIXTURE"}}}}),
            event({{"type", "content_block_stop"}, {"index", 0}}),
            event({{"type", "content_block_start"},
                   {"index", 1},
                   {"content_block",
                    QJsonObject{{"type", "text"}, {"text", "Before"}}}}),
            event({{"type", "content_block_stop"}, {"index", 1}}),
            event({{"type", "content_block_start"},
                   {"index", 2},
                   {"content_block", QJsonObject{{"type", "tool_use"},
                                                 {"id", "same-call"},
                                                 {"name", "inspectFixture"},
                                                 {"input", QJsonObject{}}}}}),
            event({{"type", "content_block_delta"},
                   {"index", 2},
                   {"delta", QJsonObject{{"type", "input_json_delta"},
                                         {"partial_json", "{\"x\":"}}}}),
            event({{"type", "content_block_delta"},
                   {"index", 2},
                   {"delta", QJsonObject{{"type", "input_json_delta"},
                                         {"partial_json", "1}"}}}}),
            event({{"type", "content_block_stop"}, {"index", 2}}),
            event({{"type", "message_delta"},
                   {"delta", QJsonObject{{"stop_reason", "tool_use"}}}}),
            event({{"type", "message_stop"}})};
  }
  if (protocol == "google") {
    auto chunk = [](const QJsonArray &parts, bool done = false) {
      QJsonObject candidate{
          {"content", QJsonObject{{"role", "model"}, {"parts", parts}}}};
      if (done)
        candidate["finishReason"] = "STOP";
      return event({{"candidates", QJsonArray{candidate}}});
    };
    if (!tool)
      return {chunk(QJsonArray{QJsonObject{{"text", "Final"}}}, true)};
    return {
        chunk(QJsonArray{QJsonObject{{"text", "Think"},
                                     {"thought", true},
                                     {"thoughtSignature", "SIGNED_FIXTURE"}}}),
        chunk(QJsonArray{QJsonObject{{"text", "Before"}}}),
        chunk(QJsonArray{QJsonObject{
                  {"functionCall",
                   QJsonObject{{"id", "same-call"},
                               {"name", "inspectFixture"},
                               {"args", QJsonObject{{"x", 1}}}}}}},
              true)};
  }
  if (!tool)
    return {event({{"type", "response.output_text.delta"},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"delta", "Final"}}),
            event({{"type", "response.completed"},
                   {"response",
                    QJsonObject{{"status", "completed"},
                                {"output", QJsonArray{responseItem(
                                               "message", "Final")}}}}})};
  auto started = responseCall();
  started["arguments"] = "";
  return {event({{"type", "response.reasoning_summary_text.delta"},
                 {"output_index", 0},
                 {"summary_index", 0},
                 {"delta", "Think"}}),
          event({{"type", "response.reasoning_summary_text.done"},
                 {"output_index", 0},
                 {"summary_index", 0},
                 {"text", "Think"}}),
          event({{"type", "response.output_text.delta"},
                 {"output_index", 1},
                 {"content_index", 0},
                 {"delta", "Before"}}),
          event({{"type", "response.output_item.added"},
                 {"output_index", 2},
                 {"item", started}}),
          event({{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "{\"x\":"}}),
          event({{"type", "response.function_call_arguments.delta"},
                 {"output_index", 2},
                 {"delta", "1}"}}),
          event({{"type", "response.output_item.done"},
                 {"output_index", 2},
                 {"item", responseCall()}}),
          event({{"type", "response.completed"},
                 {"response",
                  QJsonObject{
                      {"status", "completed"},
                      {"output", QJsonArray{responseItem("reasoning", "Think"),
                                            responseItem("message", "Before"),
                                            responseCall()}}}}})};
}
struct Reply {
  QList<QByteArray> chunks;
  bool hold = false;
  int status = 200;
};
class Server : public QObject {
public:
  QTcpServer server;
  QString protocol = "openai";
  QList<Reply> replies;
  QList<QJsonObject> requests;
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
          auto bytes =
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
          auto reply = replies.isEmpty() ? Reply{round(protocol, false)}
                                         : replies.takeFirst();
          int total = 0;
          for (const auto &chunk : reply.chunks)
            total += chunk.size();
          if (reply.hold)
            total += 1000000;
          socket->write(
              "HTTP/1.1 " + QByteArray::number(reply.status) +
              " Test\r\nContent-Type: text/event-stream\r\nContent-Length: " +
              QByteArray::number(total) + "\r\nConnection: close\r\n\r\n");
          const QPointer<QTcpSocket> safe(socket);
          int delay = 0;
          for (const auto &chunk : reply.chunks) {
            delay += 15;
            QTimer::singleShot(delay, socket, [safe, chunk] {
              if (safe && safe->state() == QAbstractSocket::ConnectedState)
                safe->write(chunk);
            });
          }
          if (!reply.hold)
            QTimer::singleShot(delay + 15, socket, [safe] {
              if (safe)
                safe->disconnectFromHost();
            });
        });
      }
    });
  }
  ProviderConfig provider() const {
    return {"fixture",
            "Fixture",
            QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
            "fake-token-only",
            "fixture-model",
            protocol};
  }
};
QJsonArray tools() {
  return {QJsonObject{
      {"type", "function"},
      {"function",
       QJsonObject{
           {"name", "inspectFixture"},
           {"description", "Fixture"},
           {"parameters",
            QJsonObject{
                {"type", "object"},
                {"properties",
                 QJsonObject{{"x", QJsonObject{{"type", "integer"}}}}}}}}}}};
}
QJsonObject findTool(const QJsonArray &rows) {
  for (const auto &row : rows)
    for (const auto &part : row.toObject()["parts"].toArray())
      if (part.toObject()["type"] == "tool")
        return part.toObject();
  return {};
}
void protocols() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p : {"openai", "anthropic", "google", "responses"})
    QTest::newRow(p) << QString(p);
}
} // namespace

class ShellEngineTest : public QObject {
  Q_OBJECT
private slots:
  void snapshotsAndDynamicContext_data() { protocols(); }
  void snapshotsAndDynamicContext() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    server.replies = {{round(protocol)}};
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setExternalTools(tools());
    engine.setAlwaysAllow(true);
    int contexts = 0;
    engine.setDynamicSystemContext([&] {
      return "<shell-fixture>ROUND_" + QString::number(++contexts) +
             "</shell-fixture>";
    });
    QString token;
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &id) { token = id; });
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.sendUserMessage("Fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!token.isEmpty(), 4000);
    const auto canonical = engine.messages();
    const auto wire = ProviderProtocol::prepare(server.provider(), canonical,
                                                "fixed", tools())
                          .body;
    QString expected;
    for (int i = 0; i < 100; ++i) {
      const auto chunk = QString::number(i) + ",";
      expected += chunk;
      QVERIFY(engine.appendExternalToolOutput(token, chunk));
    }
    QCOMPARE(findTool(engine.displayMessages())["output"].toString(), expected);
    QVERIFY(engine.replaceExternalToolOutput(token, "grid first"));
    QVERIFY(
        engine.replaceExternalToolOutput(token, "grid second fake-token-only"));
    QCOMPARE(findTool(engine.displayMessages())["output"].toString(),
             QString("grid second [REDACTED]"));
    QVERIFY(engine.replaceExternalToolOutput(token,
                                             QString(100000, QChar(0x03bb))));
    QVERIFY(findTool(engine.displayMessages())["output"]
                .toString()
                .toUtf8()
                .size() <= 65536);
    QVERIFY(engine.replaceExternalToolOutput(token, "small again"));
    QVERIFY(engine.appendExternalToolOutput(token, " plus"));
    QCOMPARE(findTool(engine.displayMessages())["output"].toString(),
             QString("small again plus"));
    QCOMPARE(ProviderProtocol::prepare(server.provider(), engine.messages(),
                                       "fixed", tools())
                 .body,
             wire);
    QVERIFY(!engine.replaceExternalToolOutput("foreign", "NO"));
    engine.completeExternalTool(token, "FINAL", false);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(contexts, 2);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(QJsonDocument(server.requests[0]).toJson().contains("ROUND_1"));
    QVERIFY(QJsonDocument(server.requests[1]).toJson().contains("ROUND_2"));
    QVERIFY(
        !QJsonDocument(engine.messages()).toJson().contains("shell-fixture"));
    QCOMPARE(findTool(engine.displayMessages())["output"].toString(),
             QString("FINAL"));
    QVERIFY(!engine.replaceExternalToolOutput(token, "STALE"));
  }
  void replacementObserver_data() {
    QTest::addColumn<QString>("action");
    for (const auto *action : {"cancel", "replace", "destroy"})
      QTest::newRow(action) << QString(action);
  }
  void replacementObserver() {
    QFETCH(QString, action);
    Server server;
    server.replies = {{round("openai")}};
    QPointer<AgentEngine> engine = new AgentEngine;
    engine->setProvider(server.provider());
    engine->setExternalTools(tools());
    engine->setAlwaysAllow(true);
    QString token;
    connect(engine, &AgentEngine::externalToolRequested, this,
            [&](const QString &id) { token = id; });
    engine->sendUserMessage("Start");
    QTRY_VERIFY_WITH_TIMEOUT(!token.isEmpty(), 4000);
    bool acted = false;
    connect(engine, &AgentEngine::displayChanged, this, [&] {
      if (acted || findTool(engine->displayMessages())["output"] != "LIVE")
        return;
      acted = true;
      if (action == "destroy")
        delete engine.data();
      else {
        engine->cancel();
        if (action == "replace")
          engine->sendUserMessage("replacement");
      }
    });
    QVERIFY(engine->replaceExternalToolOutput(token, "LIVE"));
    QVERIFY(acted);
    if (engine) {
      QVERIFY(!engine->replaceExternalToolOutput(token, "STALE"));
      QTRY_VERIFY_WITH_TIMEOUT(!engine->isBusy(), 4000);
      delete engine.data();
    }
  }
  void dynamicContextObserver_data() {
    QTest::addColumn<bool>("destroy");
    QTest::newRow("cancel") << false;
    QTest::newRow("destroy") << true;
  }
  void dynamicContextObserver() {
    QFETCH(bool, destroy);
    Server server;
    QPointer<AgentEngine> engine = new AgentEngine;
    engine->setProvider(server.provider());
    engine->setDynamicSystemContext([&] {
      if (destroy)
        delete engine.data();
      else
        engine->cancel();
      return QString("never sent");
    });
    engine->sendUserMessage("Start");
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 0);
    if (engine) {
      QVERIFY(!engine->isBusy());
      delete engine.data();
    }
  }
  void backgroundWakeHonorsPauseAndApproval() {
    Server server;
    server.replies = {{round("openai")}};
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setExternalTools(tools());
    QSignalSpy approval(&engine, &AgentEngine::approvalRequired);
    engine.sendUserMessage("Start");
    QTRY_COMPARE_WITH_TIMEOUT(approval.size(), 1, 4000);
    QVERIFY(engine.sendUserMessage("Watcher event", {}, {}, {}, true));
    QCOMPARE(engine.queuedMessages().size(), 1);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(engine.isBusy());
    engine.cancel();
    QVERIFY(engine.sendUserMessage("Later watcher", {}, {}, {}, true));
    QCOMPARE(engine.queuedMessages().size(), 2);
    QTest::qWait(100);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!engine.isBusy());
    engine.resumeQueue();
    QTRY_VERIFY_WITH_TIMEOUT(
        !engine.isBusy() && engine.queuedMessages().isEmpty(), 5000);
    QCOMPARE(server.requests.size(), 3);
    QVERIFY(
        engine.sendUserMessage("Idle successful watcher", {}, {}, {}, true));
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 4000);
  }
  void backgroundWakeAfterFailureAndRestart() {
    Server server;
    server.replies = {{round("openai", false), false, 400}};
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.sendUserMessage("Fail");
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 4000);
    QVERIFY(engine.sendUserMessage("Wake", {}, {}, {}, true));
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(engine.queuedMessages().size(), 1);
    AgentEngine restored;
    restored.setProvider(server.provider());
    QVERIFY(restored.setQueuedMessages(engine.queuedMessages()));
    QVERIFY(restored.sendUserMessage("Second wake", {}, {}, {}, true));
    QCOMPARE(restored.queuedMessages().size(), 2);
    QTest::qWait(50);
    QCOMPARE(server.requests.size(), 1);
  }
  void backgroundWakeDoesNotInterruptQuestion() {
    Server server;
    const QJsonObject definition{
        {"title", "Question"},
        {"steps",
         QJsonArray{QJsonObject{
             {"fields", QJsonArray{QJsonObject{{"type", "input"},
                                               {"questionId", "name"},
                                               {"label", "Name"}}}}}}}};
    auto c =
        call("question",
             QString::fromUtf8(
                 QJsonDocument(definition).toJson(QJsonDocument::Compact)));
    auto fn = c["function"].toObject();
    fn["name"] = "askUserQuestions";
    c["function"] = fn;
    server.replies = {{{chat({{"tool_calls", QJsonArray{c}}}, "tool_calls"),
                        "data: [DONE]\n\n"}}};
    AgentEngine engine;
    engine.setQuestionToolEnabled(true);
    engine.setProvider(server.provider());
    engine.sendUserMessage("Start");
    QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(), 4000);
    const auto before = engine.pendingQuestion();
    QVERIFY(engine.sendUserMessage("Watcher condition", {}, {}, {}, true));
    QCOMPARE(engine.pendingQuestion(), before);
    QCOMPARE(engine.queuedMessages().size(), 1);
    QCOMPARE(server.requests.size(), 1);
    engine.cancel();
  }
  void backgroundWakeSurvivesDispatchCommitDenial() {
    Server server;
    AgentEngine engine;
    engine.setProvider(server.provider());
    QVERIFY(engine.sendUserMessage("First"));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 4000);
    QJsonArray savedHistory = engine.messages(), savedQueue;
    bool blockDispatch = true;
    int enqueues = 0;
    engine.setQueueCommitter([&](const QJsonArray &history,
                                 const QJsonArray &queue, QString *error) {
      if (blockDispatch && history != savedHistory) {
        *error = "Workspace operation is in progress.";
        return false;
      }
      if (queue.size() > savedQueue.size())
        ++enqueues;
      savedHistory = history;
      savedQueue = queue;
      return true;
    });
    QVERIFY(engine.sendUserMessage("Durable watcher event", {}, {}, {}, true));
    QCOMPARE(enqueues, 1);
    QCOMPARE(savedQueue.size(), 1);
    QCOMPARE(engine.queuedMessages(), savedQueue);
    QCOMPARE(engine.messages(), savedHistory);
    QTest::qWait(100);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!engine.isBusy());
    blockDispatch = false;
    QVERIFY(engine.resumeQueue());
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 4000);
    QVERIFY(savedQueue.isEmpty());
    QVERIFY(
        QJsonDocument(savedHistory).toJson().contains("Durable watcher event"));
  }
  void externalTimeoutIsBoundedAndIdleConfigured() {
    Server server;
    server.replies = {{round("openai")}};
    AgentEngine engine;
    engine.setProvider(server.provider());
    engine.setAlwaysAllow(true);
    engine.setExternalTools(tools());
    engine.setExternalToolTimeout("inspectFixture", 1000);
    engine.setExternalToolTimeout("inspectFixture", 310001);
    QSignalSpy requests(&engine, &AgentEngine::externalToolRequested);
    QSignalSpy finished(&engine, &AgentEngine::finished);
    QSignalSpy results(&engine, &AgentEngine::toolFinished);
    engine.sendUserMessage("Deadline fixture");
    QTRY_COMPARE_WITH_TIMEOUT(requests.size(), 1, 4000);
    engine.setExternalToolTimeout("inspectFixture", 310000);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 4000);
    QCOMPARE(results.size(), 1);
    QVERIFY(results[0][2].toBool());
    QCOMPARE(results[0][1].toString(),
             QString("External tool exceeded its 1-second deadline."));
    QVERIFY(
        !engine.replaceExternalToolOutput(requests[0][0].toString(), "STALE"));
  }
};
QTEST_GUILESS_MAIN(ShellEngineTest)
#include "test_shell_engine.moc"
