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
  const auto bytes = QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n';
  fwrite(bytes.data(), 1, size_t(bytes.size()), stdout);
  fflush(stdout);
}
void reply(const QJsonValue &id, const QJsonObject &v) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", v}});
}
void update(const QJsonObject &v) {
  send(
      {{"jsonrpc", "2.0"},
       {"method", "session/update"},
       {"params", QJsonObject{{"sessionId", "owned-session"}, {"update", v}}}});
}
void text(const QString &value) {
  update({{"sessionUpdate", "agent_message_chunk"},
          {"content", QJsonObject{{"type", "text"}, {"text", value}}}});
}
void tool(const QString &id, const QString &state, const QString &output = {}) {
  update(
      {{"sessionUpdate", "tool_call_update"},
       {"toolCallId", id},
       {"kind", "execute"},
       {"title", "Fixture command"},
       {"status", state},
       {"rawInput", QJsonObject{{"command", "fixture-only-no-side-effects"}}},
       {"rawOutput", output}});
}
void permission(const QString &id) {
  send({{"jsonrpc", "2.0"},
        {"id", id},
        {"method", "session/request_permission"},
        {"params",
         QJsonObject{
             {"sessionId", "owned-session"},
             {"toolCall", QJsonObject{{"toolCallId", "denied-command"},
                                      {"kind", "execute"},
                                      {"title", "Fixture denied command"}}},
             {"options", QJsonArray{QJsonObject{{"optionId", "yes"},
                                                {"kind", "allow_once"}},
                                    QJsonObject{{"optionId", "no"},
                                                {"kind", "reject_once"}}}}}}});
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto scenario = qEnvironmentVariable("ADE_ACP_SETTLEMENT");
  QJsonValue prompt;
  int round = 0;
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto row =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    QFile log(qEnvironmentVariable("ADE_ACP_LOG"));
    if (log.open(QIODevice::Append)) {
      log.write(QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n');
      log.close();
    }
    const auto method = row.value("method").toString();
    const auto id = row.value("id");
    if (method == "initialize")
      reply(id, {{"protocolVersion", 1},
                 {"agentCapabilities",
                  QJsonObject{{"sessionCapabilities",
                               QJsonObject{{"resume", QJsonObject{}}}}}}});
    else if (method == "session/new" || method == "session/resume")
      reply(id,
            {{"sessionId", "owned-session"}, {"configOptions", QJsonArray{}}});
    else if (method == "session/prompt") {
      prompt = id;
      ++round;
      if (scenario == "deny" || scenario == "deny-loop" ||
          scenario == "pending-early") {
        if (round == 1 || scenario == "deny-loop") {
          tool("denied-command", "pending");
          permission("permission-" + QString::number(round));
          if (scenario == "pending-early")
            reply(prompt, {{"stopReason", "end_turn"}});
        } else {
          text("CONTINUED_TASK");
          reply(prompt, {{"stopReason", "end_turn"}});
        }
      } else if (scenario == "two-stale" && round == 1) {
        tool("command-a", "in_progress", "EARLY_A");
        tool("command-b", "in_progress", "EARLY_B");
        reply(prompt, {{"stopReason", "end_turn"}});
      } else if (scenario == "late" || scenario == "stale" ||
                 scenario == "progress-forever") {
        const auto key = "command-" + QString::number(round);
        tool(key, "in_progress", "EARLY_OUTPUT");
        reply(prompt, {{"stopReason", "end_turn"}});
        if (scenario == "late") {
          QThread::msleep(150);
          tool(key, "completed", "LATE_FINAL_OUTPUT");
        }
        if (scenario == "progress-forever")
          for (int i = 0; i < 30; ++i) {
            QThread::msleep(50);
            tool(key, "in_progress", "PROGRESS_" + QString::number(i));
          }
      } else {
        text("ORDINARY_FINISH");
        reply(prompt, {{"stopReason", "end_turn"}});
      }
    } else if (method == "session/cancel")
      reply(prompt, {{"stopReason", "cancelled"}});
    else if (id.isString() && id.toString().startsWith("permission-")) {
      const bool allow =
          row.value("result").toObject().value("outcome").toObject().value(
              "optionId") == "yes";
      tool("denied-command", allow ? "completed" : "failed",
           "DECISION_RECORDED");
      if (scenario != "pending-early")
        reply(prompt, {{"stopReason", "end_turn"}});
    }
  }
  return 0;
}
