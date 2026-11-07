#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <iostream>
void send(QJsonObject o) {
  const auto b = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
  for (int i = 0; i < b.size(); i += 7) {
    const auto c = b.mid(i, 7);
    fwrite(c.constData(), 1, size_t(c.size()), stdout);
    fflush(stdout);
  }
}
void reply(QJsonValue id, QJsonObject o) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", o}});
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto scenario = qEnvironmentVariable("ADE_CURSOR_SCENARIO");
  const auto log = qEnvironmentVariable("ADE_CURSOR_LOG");
  QString session = "cursor-session";
  QJsonValue prompt;
  bool authenticated = false;
  QJsonArray config{
      QJsonObject{
          {"id", "model"},
          {"category", "model"},
          {"type", "select"},
          {"currentValue", "model-a"},
          {"options",
           QJsonArray{QJsonObject{{"value", "model-a"}, {"name", "A"}},
                      QJsonObject{{"value", "model-b"}, {"name", "B"}}}}},
      QJsonObject{{"id", "mode"},
                  {"category", "mode"},
                  {"type", "select"},
                  {"currentValue", "agent"},
                  {"options", QJsonArray{QJsonObject{{"value", "agent"}},
                                         QJsonObject{{"value", "plan"}},
                                         QJsonObject{{"value", "ask"}}}}}};
  auto update = [&](QJsonObject u, QString owner = QString{}) {
    send({{"jsonrpc", "2.0"},
          {"method", "session/update"},
          {"params",
           QJsonObject{{"sessionId", owner.isEmpty() ? session : owner},
                       {"update", u}}}});
  };
  auto done = [&] {
    update({{"sessionUpdate", "tool_call_update"},
            {"toolCallId", "tool-1"},
            {"status", "completed"}});
    reply(prompt, {{"stopReason", "end_turn"}});
  };
  for (std::string line; std::getline(std::cin, line);) {
    const auto o =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    if (!log.isEmpty()) {
      QFile f(log);
      if (f.open(QIODevice::Append)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
        f.flush();
      }
    }
    const auto method = o["method"].toString();
    const auto p = o["params"].toObject();
    const auto id = o["id"];
    if (method == "initialize") {
      reply(
          id,
          {{"protocolVersion", scenario == "bad-version" ? 2 : 1},
           {"agentCapabilities",
            QJsonObject{{"loadSession", true},
                        {"promptCapabilities",
                         QJsonObject{{"image", scenario != "no-images"}}}}},
           {"authMethods", QJsonArray{QJsonObject{{"id", "cursor_login"},
                                                  {"name", "Cursor Login"}}}}});
    } else if (method == "authenticate") {
      if (scenario == "auth-error")
        send({{"jsonrpc", "2.0"},
              {"id", id},
              {"error",
               QJsonObject{{"code", -32000},
                           {"message", "SYNTHETIC_PRIVATE_AUTH_ERROR"}}}});
      else {
        authenticated = true;
        reply(id, {});
      }
    } else if (method == "session/new" || method == "session/load") {
      if (!authenticated)
        return 9;
      if (scenario == "load-error" && method == "session/load") {
        send({{"jsonrpc", "2.0"},
              {"id", id},
              {"error",
               QJsonObject{{"code", -32602}, {"message", "unknown session"}}}});
        continue;
      }
      if (method == "session/load")
        update(
            {{"sessionUpdate", "agent_message_chunk"},
             {"content", QJsonObject{{"type", "text"},
                                     {"text", "REPLAY_MUST_NOT_DUPLICATE"}}}});
      reply(id, {{"sessionId", session},
                 {"configOptions",
                  scenario == "missing-model" ? QJsonArray{} : config}});
    } else if (method == "session/set_config_option") {
      for (int i = 0; i < config.size(); ++i) {
        auto c = config[i].toObject();
        if (c["id"] == p["configId"]) {
          c["currentValue"] = p["value"];
          config[i] = c;
        }
      }
      reply(id, {{"configOptions", config}});
    } else if (method == "session/set_mode")
      reply(id, {});
    else if (method == "session/prompt") {
      prompt = id;
      update({{"sessionUpdate", "agent_message_chunk"},
              {"content", QJsonObject{{"type", "text"}, {"text", "FOREIGN"}}}},
             "other-session");
      update({{"sessionUpdate", "agent_thought_chunk"},
              {"content", QJsonObject{{"type", "text"}, {"text", "THINK"}}}});
      update({{"sessionUpdate", "agent_message_chunk"},
              {"content",
               QJsonObject{
                   {"type", "text"},
                   {"text", QString::fromUtf8("Hello \xF0\x9F\x8C\x8D")}}}});
      if (scenario == "hold" || scenario == "timeout")
        continue;
      const QJsonObject tool{
          {"toolCallId", "tool-1"},
          {"title", "Read probe.txt"},
          {"kind", "read"},
          {"status", "pending"},
          {"locations", QJsonArray{QJsonObject{{"path", "probe.txt"}}}}};
      auto toolUpdate = tool;
      toolUpdate["sessionUpdate"] = "tool_call";
      if (scenario == "ordinary-input")
        toolUpdate["name"] = "request_user_input";
      update(toolUpdate);
      if (scenario.startsWith("permission")) {
        QJsonObject params{
            {"sessionId", session},
            {"toolCall", tool},
            {"options", QJsonArray{QJsonObject{{"optionId", "allow-once"},
                                               {"kind", "allow_once"},
                                               {"name", "Allow once"}},
                                   QJsonObject{{"optionId", "reject-once"},
                                               {"kind", "reject_once"},
                                               {"name", "Reject once"}}}}};
        send({{"jsonrpc", "2.0"},
              {"id", "permission"},
              {"method", "session/request_permission"},
              {"params", params}});
        if (scenario == "permission-early-return")
          reply(prompt, {{"stopReason", "end_turn"}});
      } else if (scenario.startsWith("question")) {
        QJsonObject params{
            {"toolCallId", "tool-1"},
            {"title", "Pick a mode"},
            {"questions",
             QJsonArray{QJsonObject{
                 {"id", "mode"},
                 {"prompt", "Which mode?"},
                 {"options",
                  QJsonArray{QJsonObject{{"id", "a"}, {"label", "A"}},
                             QJsonObject{{"id", "b"}, {"label", "B"}}}},
                 {"allowMultiple", scenario == "question-multiple"}}}}};
        if (scenario == "question-invalid") {
          auto qs = params["questions"].toArray();
          auto q = qs[0].toObject();
          q["allowMultiple"] = "true";
          qs[0] = q;
          params["questions"] = qs;
        }
        send({{"jsonrpc", "2.0"},
              {"id", "question"},
              {"method", "cursor/ask_question"},
              {"params", params}});
      } else if (scenario == "plan")
        send({{"jsonrpc", "2.0"},
              {"id", "plan"},
              {"method", "cursor/create_plan"},
              {"params", QJsonObject{{"toolCallId", "tool-1"},
                                     {"name", "Review plan"},
                                     {"plan", "# Safe plan\nReview the file."},
                                     {"todos", QJsonArray{QJsonObject{
                                                   {"id", "one"},
                                                   {"content", "Review"},
                                                   {"status", "pending"}}}}}}});
      else if (scenario == "extension") {
        send({{"jsonrpc", "2.0"},
              {"method", "cursor/update_todos"},
              {"params", QJsonObject{{"toolCallId", "tool-1"},
                                     {"todos", QJsonArray{}},
                                     {"merge", false}}}});
        done();
      } else
        done();
    } else if (method == "session/cancel") {
      reply(prompt, {{"stopReason", "cancelled"}});
    } else if (method.isEmpty() &&
               (id == "permission" || id == "question" || id == "plan")) {
      if (scenario == "permission-early-return")
        update({{"sessionUpdate", "tool_call_update"},
                {"toolCallId", "tool-1"},
                {"status", "completed"}});
      else
        done();
    }
  }
  return 0;
}
