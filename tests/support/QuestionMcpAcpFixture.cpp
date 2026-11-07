#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace {
void send(const QJsonObject &value) {
  const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
  fwrite(bytes.constData(), 1, bytes.size(), stdout);
  fflush(stdout);
}
void reply(QJsonValue id, QJsonObject value) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
QJsonObject singleForm() {
  return {
      {"title", "Native fallback"},
      {"steps", QJsonArray{QJsonObject{
                    {"fields", QJsonArray{QJsonObject{{"questionId", "answer"},
                                                      {"type", "input"},
                                                      {"label", "Answer"},
                                                      {"required", true}}}}}}}};
}
QJsonObject form() {
  auto value = singleForm();
  const auto scenario = qEnvironmentVariable("ADE_MCP_SCENARIO");
  if (scenario == "invalid-form")
    value["steps"] = QJsonArray{};
  if (scenario == "two-step") {
    auto steps = value["steps"].toArray();
    steps.append(
        QJsonObject{{"fields", QJsonArray{QJsonObject{{"questionId", "second"},
                                                      {"type", "input"},
                                                      {"label", "Second"},
                                                      {"required", true}}}}});
    value["steps"] = steps;
  }
  return value;
}
class Fixture : public QObject {
public:
  Fixture() {
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    timer.start(5);
    connect(&helper, &QProcess::started, this, [this] {
      writeHelper({{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "initialize"},
                   {"params", QJsonObject{{"protocolVersion", "2025-11-25"}}}});
      writeHelper(
          {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
      writeHelper(
          {{"jsonrpc", "2.0"},
           {"id", 2},
           {"method", "tools/call"},
           {"params", QJsonObject{{"name", "stagewise_request_user_input"},
                                  {"arguments", form()}}}});
    });
    connect(&helper, &QProcess::readyReadStandardOutput, this, [this] {
      helperBytes += helper.readAllStandardOutput();
      while (helperBytes.contains('\n')) {
        const auto n = helperBytes.indexOf('\n');
        const auto message =
            QJsonDocument::fromJson(helperBytes.left(n)).object();
        helperBytes.remove(0, n + 1);
        if (message["id"] != 2)
          continue;
        const auto result = message["result"].toObject();
        if(scenario.startsWith("wrapper"))wrapper("completed",true);
        // The layout-only scenario uses an ordinary acknowledgement. Other
        // scenarios preserve the raw echo for protocol/result regression checks.
        update(scenario == "wrapper-render"
                   ? QStringLiteral("Thanks, I have your answer.")
                   : QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        helper.closeWriteChannel();
        if (scenario != "elicitation" && scenario != "hold-after-answer")
          done();
        else
          helperFinished = true;
      }
    });
  }
  ~Fixture() {
    helper.kill();
    helper.waitForFinished(1000);
  }

private:
  void writeHelper(const QJsonObject &v) {
    helper.write(QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n');
  }
  void update(QString text) {
    send({{"jsonrpc", "2.0"},
          {"method", "session/update"},
          {"params",
           QJsonObject{
               {"sessionId", "mcp-fixture-session"},
               {"update",
                QJsonObject{{"sessionUpdate", "agent_message_chunk"},
                            {"content", QJsonObject{{"type", "text"},
                                                    {"text", text}}}}}}}});
  }
  void wrapper(const QString& status,bool identified) {
    QJsonObject tool{{"sessionUpdate",status=="pending"?"tool_call":"tool_call_update"},{"toolCallId","question-wrapper"},{"kind","other"},{"status",status},{"title",identified?"stagewise_request_user_input":"Ordinary MCP wrapper"}};
    if(identified)tool["rawInput"]=QJsonObject{{"tool","stagewise_request_user_input"},{"arguments",form()}};
    if(status=="completed")tool["content"]=QJsonArray{QJsonObject{{"type","content"},{"content",QJsonObject{{"type","text"},{"text","wrapper settled"}}}}};
    send({{"jsonrpc","2.0"},{"method","session/update"},{"params",QJsonObject{{"sessionId","mcp-fixture-session"},{"update",tool}}}});
  }
  void done(QString reason = "end_turn") {
    if (prompt.isUndefined())
      return;
    const auto id = prompt;
    prompt = QJsonValue::Undefined;
    reply(id, {{"stopReason", reason}});
  }
  void launch() {
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &value : config["env"].toArray()) {
      const auto v = value.toObject();
      env.insert(v["name"].toString(), v["value"].toString());
    }
    QStringList args;
    for (const auto &value : config["args"].toArray())
      args.append(value.toString());
    helper.setProcessEnvironment(env);
    helper.start(config["command"].toString(), args);
  }
  void receive(const QJsonObject &message) {
    const auto method = message.value("method").toString();
    const QJsonValue id = message.value("id");
    const auto params = message.value("params").toObject();
    if (method == "initialize") {
      reply(id, {{"protocolVersion", 1},
                 {"agentCapabilities",
                  QJsonObject{{"sessionCapabilities",
                               QJsonObject{{"resume", QJsonObject{}}}}}}});
      return;
    }
    if (method == "session/new" || method == "session/resume") {
      const auto servers = params.value("mcpServers").toArray();
      config = servers.isEmpty() ? QJsonObject{} : servers[0].toObject();
      reply(id, {{"sessionId", "mcp-fixture-session"},
                 {"configOptions", QJsonArray{}}});
      return;
    }
    if (method == "session/prompt") {
      prompt = id;
      ++promptCount;
      const auto log=qEnvironmentVariable("ADE_MCP_PROMPT_LOG");
      if(!log.isEmpty()){QFile file(log);if(file.open(QIODevice::WriteOnly|QIODevice::Append))file.write(QJsonDocument(params).toJson(QJsonDocument::Compact)+'\n');}
      if(scenario=="wrapper-approval-first" && promptCount>1){update("continued after decision");done();return;}
      if(scenario.startsWith("wrapper") && scenario!="wrapper-approval-first"){
        wrapper("pending",scenario!="wrapper-late");
        if(scenario=="wrapper-late")QTimer::singleShot(50,this,[this]{wrapper("in_progress",true);});
      }
      if (config.isEmpty()) {
        update("NO_MCP");
        done();
        return;
      }
      if (scenario == "wrapper-approval-first") {
        const QJsonObject tool{{"toolCallId","question-wrapper"},{"name","vendor-tool"},{"title","Ordinary approval"},{"kind","execute"},{"rawInput",QJsonObject{{"command","synthetic command, not executed"}}}};
        send({{"jsonrpc","2.0"},{"id","native-approval"},{"method","session/request_permission"},{"params",QJsonObject{{"sessionId","mcp-fixture-session"},{"toolCall",tool},{"options",QJsonArray{QJsonObject{{"optionId","yes"},{"kind","allow_once"},{"name","Allow"}},QJsonObject{{"optionId","no"},{"kind","reject_once"},{"name","Deny"}}}}}}});
        QTimer::singleShot(30,this,[this]{wrapper("in_progress",true);});
        return;
      }
      if (scenario == "approval-only") {
        const QJsonObject tool{{"toolCallId", "approval-tool"},
                               {"title", "Fixture approval"},
                               {"kind", "execute"}};
        const QJsonArray choices{QJsonObject{
            {"optionId", "yes"}, {"kind", "allow_once"}, {"name", "Allow"}}};
        const QJsonObject request{{"sessionId", "mcp-fixture-session"},
                                  {"toolCall", tool},
                                  {"options", choices}};
        send({{"jsonrpc", "2.0"},
              {"id", "native-approval"},
              {"method", "session/request_permission"},
              {"params", request}});
        return;
      }
      if (scenario == "elicitation") {
        const QJsonObject schema{
            {"type", "object"},
            {"properties",
             QJsonObject{{"name", QJsonObject{{"type", "string"}}}}}};
        const QJsonObject request{{"sessionId", "mcp-fixture-session"},
                                  {"mode", "form"},
                                  {"message", "Native question first"},
                                  {"requestedSchema", schema}};
        send({{"jsonrpc", "2.0"},
              {"id", "native-question"},
              {"method", "elicitation/create"},
              {"params", request}});
      }
      launch();
      if (scenario == "process-loss")
        QTimer::singleShot(400, [] { QCoreApplication::exit(9); });
      if (scenario == "helper-loss")
        QTimer::singleShot(400, this, [this] { helper.kill(); });
      return;
    }
    if (method == "session/cancel") {
      helper.kill();
      done("cancelled");
      return;
    }
    if (id == "native-approval") {
      if(scenario=="wrapper-approval-first"){
        const auto selected=message["result"].toObject()["outcome"].toObject()["optionId"].toString();
        const auto log=qEnvironmentVariable("ADE_MCP_DECISION_LOG");if(!log.isEmpty()){QFile file(log);if(file.open(QIODevice::WriteOnly|QIODevice::Append))file.write(selected.toUtf8()+'\n');}
        wrapper(selected=="yes"?"completed":"failed",true);done();return;
      }
      update("APPROVED");
      done();
      return;
    }
    if (id == "native-question") {
      if (helperFinished)
        done();
      else {
        nativeAnswered = true;
        QTimer::singleShot(100, this, [this] {
          if (nativeAnswered)
            done();
        });
      }
    }
  }
  void poll() {
#ifdef Q_OS_WIN
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
      QCoreApplication::quit();
      return;
    }
    if (!available)
      return;
    char buffer[16384];
    DWORD count = 0;
    if (!ReadFile(input, buffer, qMin<DWORD>(available, sizeof(buffer)), &count,
                  nullptr)) {
      QCoreApplication::quit();
      return;
    }
    bytes.append(buffer, int(count));
    while (bytes.contains('\n')) {
      const auto n = bytes.indexOf('\n');
      const auto message = QJsonDocument::fromJson(bytes.left(n)).object();
      bytes.remove(0, n + 1);
      receive(message);
    }
#else
    QCoreApplication::exit(2);
#endif
  }
  QTimer timer;
  QProcess helper;
  QByteArray bytes, helperBytes;
  QJsonObject config;
  QJsonValue prompt = QJsonValue::Undefined;
  QString scenario = qEnvironmentVariable("ADE_MCP_SCENARIO");
  int promptCount=0;
  bool helperFinished = false, nativeAnswered = false;
};
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  Fixture fixture;
  return app.exec();
}
