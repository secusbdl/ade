#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
void send(const QJsonObject &object) {
  const auto bytes =
      QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
  fwrite(bytes.constData(), 1, bytes.size(), stdout);
  fflush(stdout);
}
void reply(const QJsonValue &id, const QJsonObject &value) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
class Fixture final : public QObject {
public:
  Fixture() {
    const auto args = QCoreApplication::arguments();
    codex = args.contains("app-server");
    acp = args.contains("acp");
    scenario = qEnvironmentVariable(acp ? "ADE_MCP_SCENARIO"
                                        : "ADE_DIRECT_MCP_SCENARIO");
    const int index = args.indexOf("--mcp-config");
    if (index >= 0 && index + 1 < args.size())
      config = QJsonDocument::fromJson(args[index + 1].toUtf8())
                   .object()["mcpServers"]
                   .toObject()["stagewise"]
                   .toObject();
    for (const auto &arg : args)
      if (arg.startsWith("--session-id=") || arg.startsWith("--resume="))
        session = arg.mid(arg.indexOf('=') + 1);
#ifdef Q_OS_WIN
    helper.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) {
          a->flags |= CREATE_NO_WINDOW;
        });
#endif
    connect(&helper, &QProcess::started, this, [this] {
      write({{"jsonrpc", "2.0"},
             {"id", 1},
             {"method", "initialize"},
             {"params", QJsonObject{{"protocolVersion", "2025-11-25"}}}});
      write({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
      QJsonArray steps;
      for (const auto *id : {"answer", "second"})
        steps.append(QJsonObject{
            {"fields",
             QJsonArray{QJsonObject{
                 {"questionId", id},
                 {"type", "input"},
                 {"label", id},
                 {"inputType", QString(id) == "second" ? "password" : "text"},
                 {"required", true}}}}});
      write({{"jsonrpc", "2.0"},
             {"id", 2},
             {"method", "tools/call"},
             {"params",
              QJsonObject{
                  {"name", "stagewise_request_user_input"},
                  {"arguments", QJsonObject{{"title", "Synthetic native form"},
                                            {"steps", steps}}}}}});
    });
    connect(&helper, &QProcess::readyReadStandardOutput, this, [this] {
      helperBytes += helper.readAllStandardOutput();
      while (helperBytes.contains('\n')) {
        const int end = helperBytes.indexOf('\n');
        const auto result =
            QJsonDocument::fromJson(helperBytes.left(end)).object();
        helperBytes.remove(0, end + 1);
        if (result["id"] == 2) {
          helper.closeWriteChannel();
          finish(QString::fromUtf8(QJsonDocument(result["result"].toObject())
                                       .toJson(QJsonDocument::Compact)));
        }
      }
    });
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    timer.start(5);
  }
  ~Fixture() {
    helper.kill();
    helper.waitForFinished(1000);
  }

private:
  void write(const QJsonObject &object) {
    helper.write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
  }
  void note(const QString &method, const QJsonObject &params) {
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
  }
  void launch() {
    auto env = QProcessEnvironment::systemEnvironment();
    if (acp)
      for (const auto &value : config["env"].toArray()) {
        const auto row = value.toObject();
        env.insert(row["name"].toString(), row["value"].toString());
      }
    else {
      const auto values = config["env"].toObject();
      for (auto it = values.begin(); it != values.end(); ++it)
        env.insert(it.key(), it.value().toString());
    }
    QStringList args;
    for (const auto &arg : config["args"].toArray())
      args.append(arg.toString());
    helper.setProcessEnvironment(env);
    helper.start(config["command"].toString(), args);
  }
  void finish(const QString &text = "Synthetic request ended",
              bool interrupted = false) {
    if (finished)
      return;
    finished = true;
    if (acp) {
      note("session/update",
           {{"sessionId", "session"},
            {"update", QJsonObject{{"sessionUpdate", "agent_message_chunk"},
                                   {"content", QJsonObject{{"type", "text"},
                                                           {"text", text}}}}}});
      reply(prompt, {{"stopReason", interrupted ? "cancelled" : "end_turn"}});
    } else if (codex) {
      note("item/agentMessage/delta", {{"threadId", "thread"},
                                       {"turnId", turn},
                                       {"itemId", "text"},
                                       {"delta", text}});
      note("turn/completed",
           {{"threadId", "thread"},
            {"turn",
             QJsonObject{{"id", turn},
                         {"status", interrupted ? "interrupted" : "completed"},
                         {"items", QJsonArray{}},
                         {"error", QJsonValue::Null}}}});
    } else
      send({{"type", "result"},
            {"subtype", "success"},
            {"session_id", session},
            {"is_error", false},
            {"result", text}});
  }
  void begin() {
    finished = false;
    released = false;
    ++turnCount;
    turn = "turn-" + QString::number(turnCount);
    if (!acp && !codex)
      send({{"type", "system"}, {"subtype", "init"}, {"session_id", session}});
    launch();
  }
  void receive(const QJsonObject &message) {
    const auto method = message["method"].toString();
    const QJsonValue id = message["id"];
    const auto params = message["params"].toObject();
    if (acp) {
      if (method == "initialize")
        reply(id,
              {{"protocolVersion", 1}, {"agentCapabilities", QJsonObject{}}});
      else if (method == "session/new" || method == "session/resume") {
        config = params["mcpServers"].toArray().at(0).toObject();
        reply(id, {{"sessionId", "session"}, {"configOptions", QJsonArray{}}});
      } else if (method == "session/prompt") {
        prompt = id;
        begin();
      } else if (method == "session/cancel") {
        helper.kill();
        finish("Stopped", true);
      }
    } else if (codex) {
      if (method == "initialize")
        reply(id, {{"userAgent", "fixture"}});
      else if (method == "account/read")
        reply(id, {{"requiresOpenaiAuth", true},
                   {"account", QJsonObject{{"type", "chatgpt"},
                                           {"planType", "fixture"}}}});
      else if (method.startsWith("thread/")) {
        config =
            params["config"].toObject()["mcp_servers.stagewise"].toObject();
        reply(id, {{"thread", QJsonObject{{"id", "thread"}}}});
      } else if (method == "turn/start") {
        reply(id,
              {{"turn", QJsonObject{{"id", "turn-" + QString::number(turnCount +
                                                                     1)}}}});
        begin();
      } else if (method == "turn/interrupt") {
        helper.kill();
        reply(id, {});
        finish("Stopped", true);
      }
    } else if (message["type"] == "control_request") {
      send({{"type", "control_response"},
            {"response", QJsonObject{{"subtype", "success"},
                                     {"request_id", message["request_id"]},
                                     {"response", QJsonObject{}}}}});
      if (message["request"].toObject()["subtype"] == "interrupt") {
        helper.kill();
        finish("Stopped", true);
      }
    } else if (message["type"] == "user")
      begin();
  }
  void poll() {
    const auto release = qEnvironmentVariable("ADE_QUESTION_RELEASE");
    if (scenario == "controlled-loss" && !released && !release.isEmpty() &&
        QFile::exists(release)) {
      released = true;
      helper.kill();
      QTimer::singleShot(150, this, [this] { finish(); });
    }
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
    if (!ReadFile(input, buffer, qMin<DWORD>(available, sizeof buffer), &count,
                  nullptr)) {
      QCoreApplication::quit();
      return;
    }
    bytes.append(buffer, int(count));
    while (bytes.contains('\n')) {
      const int end = bytes.indexOf('\n');
      const auto message = QJsonDocument::fromJson(bytes.left(end)).object();
      bytes.remove(0, end + 1);
      receive(message);
    }
#endif
  }
  QTimer timer;
  QProcess helper;
  QByteArray bytes, helperBytes;
  QJsonObject config;
  QJsonValue prompt;
  QString session = "5ca575e5-a472-48cd-87fd-89a08987dec1", turn, scenario;
  bool codex = false, acp = false, finished = false, released = false;
  int turnCount = 0;
};
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  Fixture fixture;
  return app.exec();
}
