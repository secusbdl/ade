#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <iostream>
namespace {
void send(const QJsonObject &v) {
  const auto b = QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n';
  for (int i = 0; i < b.size(); i += 11) {
    auto part = b.mid(i, 11);
    fwrite(part.constData(), 1, size_t(part.size()), stdout);
    fflush(stdout);
  }
}
void reply(const QJsonValue &id, const QJsonObject &value) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto scenario = qEnvironmentVariable("ADE_ACP_SCENARIO");
  if (app.arguments().contains("models")) {
    if (scenario == "models-hold") {
      QThread::sleep(60);
      return 0;
    }
    if (scenario == "models-error") {
      fputs("SYNTHETIC_SECRET_ERROR", stderr);
      return 2;
    }
    if (scenario == "models-large") {
      const QByteArray large(17 * 1024 * 1024, 'x');
      fwrite(large.constData(), 1, size_t(large.size()), stdout);
      return 0;
    }
    const QJsonObject metadata{
        {"providerID", "opencode"},
        {"name", "Fixture Model"},
        {"limit", QJsonObject{{"context", 128000}}},
        {"variants",
         QJsonObject{{"medium", QJsonObject{}}, {"high", QJsonObject{}}}},
        {"capabilities",
         QJsonObject{{"reasoning", true},
                     {"input", QJsonObject{{"text", true}, {"image", true}}}}},
        {"unknownSecret", "SYNTHETIC_SECRET_CATALOG"}};
    fputs("opencode/fixture\n", stdout);
    const auto bytes = QJsonDocument(metadata).toJson();
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    return 0;
  }
  QString session = "session-one";
  QJsonValue promptId;
  QString workspace;
  QJsonArray config{QJsonObject{
      {"id", "model"},
      {"type", "select"},
      {"category", "model"},
      {"currentValue", "model-a"},
      {"options",
       QJsonArray{QJsonObject{{"value", "model-a"}, {"name", "A"}},
                  QJsonObject{{"value", "model-b"}, {"name", "B"}}}}}};
  if (scenario == "thinking-off")
    config.append(QJsonObject{
        {"id", "thought_level"},
        {"type", "select"},
        {"currentValue", "high"},
        {"options",
         QJsonArray{QJsonObject{
             {"group", "Effort"},
             {"options",
              QJsonArray{
                  QJsonObject{{"value", "none"}, {"name", "None"}},
                  QJsonObject{{"value", "high"}, {"name", "High"}}}}}}}});
  auto update = [&](const QJsonObject &v, const QString &owner = QString{}) {
    send({{"jsonrpc", "2.0"},
          {"method", "session/update"},
          {"params",
           QJsonObject{{"sessionId", owner.isEmpty() ? session : owner},
                       {"update", v}}}});
  };
  auto done = [&](const QString &reason = "end_turn") {
    reply(promptId, {{"stopReason", reason}});
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto row =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    const auto method = row["method"].toString();
    const auto id = row["id"];
    const auto p = row["params"].toObject();
    QFile log(qEnvironmentVariable("ADE_ACP_LOG"));
    if (!log.fileName().isEmpty() && log.open(QIODevice::Append)) {
      log.write(QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n');
      log.close();
    }
    if (method == "initialize")
      reply(
          id,
          {{"protocolVersion", scenario == "bad-version" ? 2 : 1},
           {"agentCapabilities",
            QJsonObject{{"sessionCapabilities",
                         scenario == "no-resume"
                             ? QJsonObject{}
                             : QJsonObject{{"resume", QJsonObject{}}}},
                        {"promptCapabilities", QJsonObject{{"image", false}}}}},
           {"authMethods",
            QJsonArray{
                QJsonObject{{"id", "login"}, {"name", "Use CLI sign-in"}}}}});
    else if (method == "session/new" || method == "session/resume") {
      workspace=p["cwd"].toString();
      if (scenario == "auth-required" ||
          (scenario == "resume-error" && method == "session/resume") ||
          scenario == "session-error") {
        send({{"id", id},
              {"error",
               QJsonObject{
                   {"code", scenario == "auth-required" ? -32000 : -32603},
                   {"message", "SYNTHETIC_SECRET_ERROR"}}}});
        continue;
      }
      if (method == "session/resume")
        session = p["sessionId"].toString();
      reply(id, {{"sessionId", session}, {"configOptions", config}});
    } else if (method == "session/set_config_option") {
      for (qsizetype i = 0; i < config.size(); ++i) {
        auto option = config[i].toObject();
        if (option["id"] == p["configId"]) {
          option["currentValue"] = p["value"];
          config[i] = option;
        }
      }
      reply(id, {{"configOptions", config}});
    } else if (method == "session/prompt") {
      promptId = id;
      update({{"sessionUpdate", "agent_message_chunk"},
              {"content", QJsonObject{{"type", "text"}, {"text", "FOREIGN"}}}},
             "foreign-session");
      update(
          {{"sessionUpdate", "agent_thought_chunk"},
           {"content", QJsonObject{{"type", "text"}, {"text", "Thinking"}}}});
      if (scenario == "presentation") {
        const QJsonArray tools{
            QJsonObject{{"toolCallId","read-id"},{"name","vendor-read"},{"kind","read"},{"title","Read fixture"},{"locations",QJsonArray{QJsonObject{{"path",workspace+"/probe.txt"}}}},{"rawInput",QJsonObject{{"rawSentinel","retain me"}}},{"rawOutput","read body"}},
            QJsonObject{{"toolCallId","list-id"},{"kind","read"},{"title","List 'src'"}},
            QJsonObject{{"toolCallId","search-id"},{"kind","search"},{"title","Search 'needle'"},{"rawOutput","a\nb\n"}},
            QJsonObject{{"toolCallId","execute-id"},{"kind","execute"},{"title","Check fixture"},{"rawInput",QJsonObject{{"command","echo"},{"args",QJsonArray{"test",3}}}},{"rawOutput",QJsonObject{{"output","OUTPUT"},{"exit_code",0}}}},
            QJsonObject{{"toolCallId","diff-id"},{"kind","edit"},{"content",QJsonArray{QJsonObject{{"type","diff"},{"path",workspace+"/a.cpp"},{"oldText","old\n"},{"newText","new\n"}},QJsonObject{{"type","diff"},{"path",workspace+"/b.cpp"},{"newText","added\n"}}}}}
        };
        for(const auto &value:tools) {
          auto tool=value.toObject();tool["sessionUpdate"]="tool_call";tool["status"]="in_progress";update(tool);
          update({{"sessionUpdate","tool_call_update"},{"toolCallId",tool["toolCallId"]},{"status","completed"}});
        }
        update({{"sessionUpdate","agent_message_chunk"},{"content",QJsonObject{{"type","text"},{"text","READY"}}}});done();continue;
      }
      if (scenario == "presentation-approval-first") {
        const QJsonObject call{{"toolCallId","tool-1"},{"name","vendor-read"},{"kind","read"},{"title","Read fixture"},{"status","pending"},{"locations",QJsonArray{QJsonObject{{"path",workspace+"/probe.txt"}}}}};
        send({{"id","approval-1"},{"method","session/request_permission"},{"params",QJsonObject{{"sessionId",session},{"toolCall",call},{"options",QJsonArray{QJsonObject{{"optionId","yes"},{"kind","allow_once"},{"name","Allow"}},QJsonObject{{"optionId","no"},{"kind","reject_once"},{"name","Deny"}}}}}}});
        auto updated=call;updated["sessionUpdate"]="tool_call_update";updated["rawOutput"]="partial while permission pending";update(updated);continue;
      }
      if (scenario == "updates") {
        update({{"sessionUpdate", "agent_message_chunk"},
                {"messageId", "message-a"},
                {"content", QJsonObject{{"type", "text"}, {"text", "A"}}}});
        update({{"sessionUpdate", "agent_thought_chunk"},
                {"messageId", "reason-b"},
                {"content", QJsonObject{{"type", "text"}, {"text", "R"}}}});
        update({{"sessionUpdate", "agent_message_chunk"},
                {"messageId", "message-a"},
                {"content", QJsonObject{{"type", "text"}, {"text", "B"}}}});
        update({{"sessionUpdate", "tool_call"},
                {"toolCallId", "tool-shared"},
                {"kind", "execute"},
                {"title", "Fixture command"},
                {"rawInput", QJsonObject{{"command", "fixture"}}},
                {"status", "pending"},
                {"_meta", QJsonObject{{"terminal_output",
                                       QJsonObject{{"data", "OUTPUT"}}}}}});
        update({{"sessionUpdate", "tool_call_update"},
                {"toolCallId", "tool-shared"},
                {"_meta", QJsonObject{{"terminal_exit",
                                       QJsonObject{{"exit_code", 0}}}}}});
        update({{"sessionUpdate", "plan"},
                {"entries", QJsonArray{QJsonObject{{"content", "step"},
                                                   {"status", "pending"}}}}});
        update({{"sessionUpdate", "plan_removed"}, {"planId", "fixture-plan"}});
        update(
            {{"sessionUpdate", "usage_update"}, {"used", 42}, {"size", 1000}});
        update(
            {{"sessionUpdate", "usage_update"}, {"used", 50}, {"size", 1000}});
        update(
            {{"sessionUpdate", "usage_update"}, {"used", 2.5}, {"size", 1000}});
        update({{"sessionUpdate", "usage_update"},
                {"used", "untrusted"},
                {"size", 1000}});
        done();
        continue;
      }
      if (scenario == "hold" || scenario == "ignore-cancel")
        continue;
      if (scenario == "approval" || scenario == "duplicate-approval") {
        const QJsonObject call{{"toolCallId", "tool-1"},
                               {"title", "Read fixture"},
                               {"kind", "read"},
                               {"status", "pending"}};
        auto initial = call;
        initial["sessionUpdate"] = "tool_call";
        update(initial);
        send({{"id", "approval-1"},
              {"method", "session/request_permission"},
              {"params",
               QJsonObject{
                   {"sessionId", session},
                   {"toolCall", call},
                   {"options", QJsonArray{QJsonObject{{"optionId", "yes"},
                                                      {"kind", "allow_once"},
                                                      {"name", "Allow"}},
                                          QJsonObject{{"optionId", "no"},
                                                      {"kind", "reject_once"},
                                                      {"name", "Deny"}}}}}}});
        if (scenario == "duplicate-approval") {
          send({{"id", "approval-2"},
                {"method", "session/request_permission"},
                {"params", QJsonObject{{"sessionId", session},
                                       {"toolCall", call},
                                       {"options", QJsonArray{QJsonObject{
                                                       {"optionId", "yes"},
                                                       {"kind", "allow_once"},
                                                       {"name", "Allow"}}}}}}});
          update(
              {{"sessionUpdate", "agent_message_chunk"},
               {"content", QJsonObject{{"type", "text"},
                                       {"text", "DUPLICATE_SENT_SENTINEL"}}}});
        }
        continue;
      }
      if (scenario == "question" || scenario == "integer-question" ||
          scenario == "sessionless-question") {
        const QJsonObject field = scenario == "integer-question"
                                      ? QJsonObject{{"type", "integer"},
                                                    {"minimum", 1},
                                                    {"maximum", 3}}
                                      : QJsonObject{{"type", "string"},
                                                    {"minLength", 1},
                                                    {"maxLength", 10}};
        const QJsonObject schema{{"type", "object"},
                                 {"required", QJsonArray{"name"}},
                                 {"properties", QJsonObject{{"name", field}}}};
        QJsonObject question{{"sessionId", session},
                             {"mode", "form"},
                             {"message", "Fixture question"},
                             {"requestedSchema", schema}};
        if (scenario == "sessionless-question")
          question.remove("sessionId");
        send({{"id", "question-1"},
              {"method", "elicitation/create"},
              {"params", question}});
        continue;
      }
      update({{"sessionUpdate", "agent_message_chunk"},
              {"content", QJsonObject{{"type", "text"}, {"text", "ACP_OK"}}}});
      update({{"sessionUpdate", "usage_update"}, {"used", 42}, {"size", 1000}});
      done();
    } else if (method == "session/cancel") {
      if (scenario != "ignore-cancel")
        done("cancelled");
    } else if (id == "approval-1") {
      if (scenario == "duplicate-approval") {
        send({{"id", "approval-3"},
              {"method", "session/request_permission"},
              {"params",
               QJsonObject{
                   {"sessionId", session},
                   {"toolCall", QJsonObject{{"toolCallId", "tool-1"},
                                            {"title", "Repeat fixture"}}},
                   {"options", QJsonArray{QJsonObject{{"optionId", "yes"},
                                                      {"kind", "allow_once"},
                                                      {"name", "Allow"}}}}}}});
        continue;
      }
      update(
          {{"sessionUpdate", "tool_call_update"},
           {"toolCallId", "tool-1"},
           {"status",
            row["result"].toObject()["outcome"].toObject()["optionId"] == "yes"
                ? "completed"
                : "failed"}});
      done();
    } else if (id == "question-1" || id == "approval-3")
      done();
  }
}
