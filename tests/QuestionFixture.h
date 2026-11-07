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
