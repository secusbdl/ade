#include "HarnessAttachmentEvidence.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cstdio>
#include <iostream>

namespace {
void send(const QJsonObject &value) {
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
  for (qsizetype i = 0; i < bytes.size(); i += 3) {
    auto part = bytes.mid(i, 3);
    fwrite(part.data(), 1, size_t(part.size()), stdout);
    fflush(stdout);
  }
}
void response(const QString &id, const QJsonObject &value) {
  send({{"type", "control_response"},
        {"response", QJsonObject{{"subtype", "success"},
                                 {"request_id", id},
                                 {"response", value}}}});
}
void request(const QString &id) {
  send({{"type", "control_request"},
        {"request_id", id},
        {"request",
         QJsonObject{{"subtype", "can_use_tool"},
                     {"tool_name", "Read"},
                     {"tool_use_id", "tool-fixture"},
                     {"input", QJsonObject{{"file_path", "fixture.txt"}}}}}});
}
void question() {
  const QJsonArray choices{
      QJsonObject{{"label", "One"}, {"description", "First"}},
      QJsonObject{{"label", "Two"}, {"description", "Second"}}};
  send({{"type", "control_request"},
        {"request_id", "question-1"},
        {"request",
         QJsonObject{
             {"subtype", "can_use_tool"},
             {"tool_name", "AskUserQuestion"},
             {"tool_use_id", "question-tool"},
             {"input",
              QJsonObject{
                  {"questions",
                   QJsonArray{QJsonObject{{"question", "Choose one?"},
                                          {"header", "Choice"},
                                          {"multiSelect", false},
                                          {"options", choices}},
                              QJsonObject{{"question", "Choose several?"},
                                          {"multiSelect", true},
                                          {"options", choices}}}}}}}}});
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QFile argumentLog(qEnvironmentVariable("ADE_CLAUDE_ARGS_LOG"));
  if (!argumentLog.fileName().isEmpty() && argumentLog.open(QIODevice::Append))
    argumentLog.write(
        QJsonDocument(QJsonArray::fromStringList(app.arguments().mid(1)))
            .toJson(QJsonDocument::Compact) +
        '\n');
  argumentLog.close();
  const auto mode = qEnvironmentVariable("ADE_CLAUDE_FIXTURE");
  QString sessionId = "session-fixture";
  const bool sessionMode = app.arguments().contains("--print");
  for (const auto &argument : app.arguments())
    if (argument.startsWith("--session-id=") ||
        argument.startsWith("--resume="))
      sessionId = argument.mid(argument.indexOf('=') + 1);
  if (app.arguments().contains("--fork-session"))
    sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto value =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    recordAttachmentEvidence("claude",value);
    QFile log(qEnvironmentVariable("ADE_CLAUDE_LOG"));
    if (!log.fileName().isEmpty() && log.open(QIODevice::Append))
      log.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
    if (value["type"] == "control_request") {
      const auto id = value["request_id"].toString();
      const auto req = value["request"].toObject();
      if (mode == "hold")
        continue;
      if (mode == "invalid-json") {
        fputs("broken-json\n", stdout);
        fflush(stdout);
        continue;
      }
      if (mode == "invalid-utf8") {
        const QByteArray bytes("{\"type\":\"system\",\"x\":\"\xff\"}\n");
        fwrite(bytes.data(), 1, size_t(bytes.size()), stdout);
        fflush(stdout);
        continue;
      }
      if (mode == "oversize") {
        const QByteArray bytes(8 * 1024 * 1024 + 1, 'x');
        fwrite(bytes.data(), 1, size_t(bytes.size()), stdout);
        fflush(stdout);
        continue;
      }
      if (mode == "error") {
        send(
            {{"type", "control_response"},
             {"response", QJsonObject{{"request_id", id},
                                      {"subtype", "error"},
                                      {"error", "SYNTHETIC_PRIVATE_DETAIL"}}}});
        continue;
      }
      if (mode == "null-success") {
        send({{"type", "control_response"},
              {"response", QJsonObject{{"subtype", "success"},
                                       {"request_id", id},
                                       {"response", QJsonValue::Null}}}});
        continue;
      }
      if (mode == "unknown-id")
        response("unowned", {{"ignored", true}});
      response(id,
               {{"models", QJsonArray{QJsonObject{{"value", "fixture-model"},
                                                  {"displayName", "Fixture"}}}},
                {"requestSubtype", req["subtype"]}});
      if (mode == "permission" || mode == "cancel-permission" ||
          mode == "duplicate-permission") {
        request("permission-1");
        if (mode == "cancel-permission")
          send({{"type", "control_cancel_request"},
                {"request_id", "permission-1"}});
        if (mode == "duplicate-permission")
          request("permission-1");
      }
    } else if (value["type"] == "user") {
      if (sessionMode)
        send({{"type", "system"},
              {"subtype", "init"},
              {"session_id", sessionId}});
      if (mode == "hold-turn")
        continue;
      if (mode == "authentication-error") {
        send({{"type", "assistant"},
              {"session_id", sessionId},
              {"error", "authentication_failed"},
              {"message",
               QJsonObject{
                   {"content",
                    QJsonArray{QJsonObject{
                        {"type", "text"},
                        {"text", "SYNTHETIC_ACCOUNT_PRIVATE_ERROR"}}}}}}});
        continue;
      }
      if (mode == "permission-turn") {
        send({{"type", "assistant"},
              {"session_id", sessionId},
              {"message",
               QJsonObject{{"id", "permission-message"},
                           {"content",
                            QJsonArray{QJsonObject{
                                {"type", "tool_use"},
                                {"id", "tool-fixture"},
                                {"name", "Read"},
                                {"input", QJsonObject{{"file_path",
                                                       "fixture.txt"}}}}}}}}});
        request("permission-1");
        continue;
      }
      if (mode == "question-turn" || mode == "cancel-question") {
        question();
        if (mode == "cancel-question")
          send({{"type", "control_cancel_request"},
                {"request_id", "question-1"}});
        continue;
      }
      if (mode == "background")
        send({{"type", "system"},
              {"subtype", "task_started"},
              {"session_id", sessionId},
              {"task_id", "owned-task"}});

      if (mode == "background-result")
        send({{"type", "result"},
              {"subtype", "success"},
              {"session_id", sessionId},
              {"origin", QJsonObject{{"kind", "task-notification"}}},
              {"result", "UNOWNED_BACKGROUND_RESULT"}});
      const auto messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
      send({{"type", "stream_event"},
            {"session_id", sessionId},
            {"event",
             QJsonObject{{"type", "message_start"},
                         {"message", QJsonObject{{"id", messageId}}}}}});
      send({{"type", "stream_event"},
            {"session_id", sessionId},
            {"event",
             QJsonObject{{"type", "content_block_start"},
                         {"index", 0},
                         {"content_block",
                          QJsonObject{{"type", "text"}, {"text", ""}}}}}});

      send({{"type", "stream_event"},
            {"session_id", sessionId},
            {"event",
             QJsonObject{
                 {"type", "content_block_delta"},
                 {"index", 0},
                 {"delta",
                  QJsonObject{{"type", "text_delta"},
                              {"text", QString::fromUtf8(
                                           "TEXT_\xf0\x9f\x98\x80")}}}}}});
      if (mode != "partial-result")
        send({{"type", "assistant"},
              {"session_id", sessionId},
              {"message",
               QJsonObject{{"id", messageId},
                           {"content",
                            QJsonArray{QJsonObject{
                                {"type", "text"},
                                {"text", QString::fromUtf8(
                                             "TEXT_\xf0\x9f\x98\x80")}}}}}}});
      send({{"type", "result"},
            {"subtype", "success"},
            {"session_id", sessionId},
            {"result", "COMPLETE"}});
    } else if (value["type"] == "control_response") {
      if (mode == "permission-turn" || mode == "question-turn") {
        if (mode == "permission-turn") {
          const bool allow = value.value("response")
                                 .toObject()
                                 .value("response")
                                 .toObject()
                                 .value("behavior") == "allow";
          send({{"type", "user"},
                {"session_id", sessionId},
                {"message",
                 QJsonObject{
                     {"content", QJsonArray{QJsonObject{
                                     {"type", "tool_result"},
                                     {"tool_use_id", "tool-fixture"},
                                     {"content", allow ? "FILE_MARKER"
                                                       : "Permission denied"},
                                     {"is_error", !allow}}}}}}});
        }
        send({{"type", "result"},
              {"subtype", "success"},
              {"session_id", sessionId},
              {"result", "DECISION_DONE"},
              {"usage",
               QJsonObject{{"input_tokens", 7}, {"output_tokens", 3}}}});
        continue;
      }

      send({{"type", "system"},
            {"subtype", "fixture_decision"},
            {"decision", value["response"]}});
    }
  }
}
