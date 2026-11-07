#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
void send(QJsonObject value) {
  auto b = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
  fwrite(b.data(), 1, b.size(), stdout);
  fflush(stdout);
}
void reply(QJsonValue id, QJsonObject value) {
  send({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}});
}
QJsonObject form() {
  QJsonArray fields{QJsonObject{{"questionId", "answer"},
                                {"type", "input"},
                                {"label", "Answer"},
                                {"required", true}}};
  QJsonArray steps{QJsonObject{{"fields", fields}}};
  if (qEnvironmentVariable("ADE_DIRECT_MCP_SCENARIO") == "two-step") {
    auto second = fields[0].toObject();
    second["questionId"] = "second";
    steps.append(QJsonObject{{"fields", QJsonArray{second}}});
  }
  return {{"title", "Shared native question"}, {"steps", steps}};
}
class Fixture final : public QObject {
public:
  Fixture() {
    codex = QCoreApplication::arguments().contains("app-server");
    const auto args = QCoreApplication::arguments();
    const auto at = args.indexOf("--mcp-config");
    if (at >= 0 && at + 1 < args.size())
      config = QJsonDocument::fromJson(args[at + 1].toUtf8())
                   .object()
                   .value("mcpServers")
                   .toObject()
                   .value("stagewise")
                   .toObject();
    for (const auto &arg : args)
      if (arg.startsWith("--session-id=") || arg.startsWith("--resume=")) {
        session = arg.mid(arg.indexOf('=') + 1);
        resume = arg.startsWith("--resume=");
      }
    log("launch",
        {{"resume", resume}, {"fork", args.contains("--fork-session")}});
    connect(&timer, &QTimer::timeout, this, [this] { poll(); });
    timer.start(5);
    connect(&helper, &QProcess::started, this, [this] {
      write({{"jsonrpc", "2.0"},
             {"id", 1},
             {"method", "initialize"},
             {"params", QJsonObject{{"protocolVersion", "2025-11-25"}}}});
      write({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
      write({{"jsonrpc", "2.0"},
             {"id", 2},
             {"method", "tools/call"},
             {"params", QJsonObject{{"name", "stagewise_request_user_input"},
                                    {"arguments", form()}}}});
    });
    connect(&helper, &QProcess::readyReadStandardOutput, this, [this] {
      helperBytes += helper.readAllStandardOutput();
      while (helperBytes.contains('\n')) {
        const auto n = helperBytes.indexOf('\n');
        const auto msg = QJsonDocument::fromJson(helperBytes.left(n)).object();
        helperBytes.remove(0, n + 1);
        if (msg.value("id") != 2)
          continue;
        result = msg.value("result").toObject();
        helper.closeWriteChannel();
        log("helperResult", result);
        if (scenario.startsWith("wrapper"))
          wrapper(true);
        if (scenario == "native-first")
          helperDone = true;
        else if (scenario != "hold-after-answer")
          done(QString::fromUtf8(
              QJsonDocument(result).toJson(QJsonDocument::Compact)));
      }
    });
#ifdef Q_OS_WIN
    helper.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) {
          a->flags |= CREATE_NO_WINDOW;
        });
#endif
  }
  ~Fixture() {
    helper.kill();
    helper.waitForFinished(1000);
  }

private:
  void write(QJsonObject v) {
    helper.write(QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n');
  }
  void log(const QString &method, const QJsonObject &input = {}) {
    QFile file(qEnvironmentVariable("ADE_DIRECT_MCP_LOG"));
    if (file.fileName().isEmpty() || !file.open(QIODevice::Append))
      return;
    const auto env = config.value("env").toObject();
    const QJsonObject record{
        {"protocol", codex ? "codex" : "claude"},
        {"method", method},
        {"configured", !config.isEmpty() && config.value("enabled") != false},
        {"input", input},
        {"configKeys", QJsonArray::fromStringList(config.keys())},
        {"envKeys", QJsonArray::fromStringList(env.keys())},
        {"tokenShape",
         env.value("STAGEWISE_MCP_CALLBACK_TOKEN").toString().size() == 64}};
    file.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n');
  }
  void note(const QString &method, QJsonObject params) {
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
  }
  void done(const QString &text = QString("STOPPED"),
            bool interrupted = false) {
    if (codex) {
      note("item/agentMessage/delta", {{"threadId", "thread-1"},
                                       {"turnId", turn},
                                       {"itemId", "answer"},
                                       {"delta", text}});
      note("turn/completed",
           {{"threadId", "thread-1"},
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
  void nativeQuestion() {
    if (codex) {
      QJsonObject question{{"id", "native"},
                           {"question", "Native fallback?"},
                           {"options", QJsonArray{}}};
      QJsonObject parameters{{"threadId", "thread-1"},
                             {"turnId", turn},
                             {"itemId", "native-question"},
                             {"questions", QJsonArray{question}}};
      send({{"jsonrpc", "2.0"},
            {"id", "native-question"},
            {"method", "item/tool/requestUserInput"},
            {"params", parameters}});
    } else {
      QJsonArray options{
          QJsonObject{{"label", "Yes"}, {"description", "Accept"}},
          QJsonObject{{"label", "No"}, {"description", "Reject"}}};
      QJsonObject question{{"question", "Native fallback?"},
                           {"header", "Native"},
                           {"multiSelect", false},
                           {"options", options}};
      QJsonObject request{
          {"subtype", "can_use_tool"},
          {"tool_name", "AskUserQuestion"},
          {"tool_use_id", "native-question"},
          {"input", QJsonObject{{"questions", QJsonArray{question}}}}};
      send({{"type", "control_request"},
            {"request_id", "native-question"},
            {"request", request}});
    }
  }
  void wrapper(bool completed) {
    const bool foreign = scenario == "wrapper-foreign";
    const bool similar = scenario == "wrapper-similar";
    const auto name =
        QString("stagewise_request_user_input") + (similar ? "_other" : "");
    if (codex) {
      QJsonObject item{{"id", "question-wrapper"},
                       {"type", "mcpToolCall"},
                       {"server", foreign ? "other" : "stagewise"},
                       {"tool", name},
                       {"arguments", form()},
                       {"status", completed ? "completed" : "inProgress"}};
      if (completed)
        item["result"] = result;
      note(completed ? "item/completed" : "item/started",
           {{"threadId", "thread-1"}, {"turnId", turn}, {"item", item}});
    } else if (completed) {
      const QJsonObject block{
          {"type", "tool_result"},
          {"tool_use_id", "question-wrapper"},
          {"content", QString::fromUtf8(QJsonDocument(result).toJson(
                          QJsonDocument::Compact))},
          {"is_error", false}};
      send({{"type", "user"},
            {"session_id", session},
            {"message", QJsonObject{{"content", QJsonArray{block}}}}});
    } else {
      const QJsonObject block{
          {"type", "tool_use"},
          {"id", "question-wrapper"},
          {"name",
           "mcp__" + QString(foreign ? "other" : "stagewise") + "__" + name},
          {"input", form()}};
      send({{"type", "assistant"},
            {"session_id", session},
            {"message", QJsonObject{{"id", "wrapper-message"},
                                    {"content", QJsonArray{block}}}}});
    }
  }
  void launch() {
    if (scenario.startsWith("wrapper"))
      wrapper(false);
    if (config.isEmpty() || config.value("enabled") == false) {
      done("NO_MCP");
      return;
    }
    auto env = QProcessEnvironment::systemEnvironment();
    const auto variables = config.value("env").toObject();
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it)
      env.insert(it.key(), it.value().toString());
    QStringList args;
    for (const auto &v : config.value("args").toArray())
      args.append(v.toString());
    helper.setProcessEnvironment(env);
    helper.start(config.value("command").toString(), args);
    if (scenario == "process-loss")
      QTimer::singleShot(400, [] { QCoreApplication::exit(9); });
    if (scenario == "helper-loss")
      QTimer::singleShot(400, this, [this] { helper.kill(); });
  }
  void prompt(QJsonObject input) {
    if (!codex && resume && scenario == "resume-missing") {
      send({{"type", "result"},
            {"subtype", "error_during_execution"},
            {"is_error", true},
            {"session_id", session},
            {"errors",
             QJsonArray{"No conversation found with session ID: " + session}}});
      return;
    }
    ++count;
    turn = "turn-" + QString::number(count);
    log("prompt", input);
    if (!codex)
      send({{"type", "system"}, {"subtype", "init"}, {"session_id", session}});
    if (scenario == "native" || scenario == "native-first")
      nativeQuestion();
    if (scenario == "native")
      return;
    if (scenario == "approval" || scenario == "wrapper-approval") {
      if (codex)
        send({{"jsonrpc", "2.0"},
              {"id", "approval"},
              {"method", "item/commandExecution/requestApproval"},
              {"params",
               QJsonObject{{"threadId", "thread-1"},
                           {"turnId", turn},
                           {"itemId", "question-wrapper"},
                           {"command", "synthetic wrapper approval"}}}});
      else
        send({{"type", "control_request"},
              {"request_id", "approval"},
              {"request",
               QJsonObject{{"subtype", "can_use_tool"},
                           {"tool_name",
                            "mcp__stagewise__stagewise_request_user_input"},
                           {"tool_use_id", "question-wrapper"},
                           {"input", form()}}}});
      return;
    }
    QTimer::singleShot(scenario == "native-first" ? 60 : 0, this,
                       [this] { launch(); });
  }
  void receive(const QJsonObject &m) {
    const auto method = m.value("method").toString();
    const auto id = m.value("id");
    const auto p = m.value("params").toObject();
    if (codex) {
      if (method == "initialize")
        reply(id, {{"userAgent", "fixture"}});
      else if (method == "account/read")
        reply(id, {{"requiresOpenaiAuth", true},
                   {"account", QJsonObject{{"type", "chatgpt"},
                                           {"planType", "fixture"}}}});
      else if (method.startsWith("thread/")) {
        config = p.value("config")
                     .toObject()
                     .value("mcp_servers.stagewise")
                     .toObject();
        log(method);
        if (method == "thread/resume" && scenario == "resume-missing") {
          send({{"jsonrpc", "2.0"},
                {"id", id},
                {"error",
                 QJsonObject{{"code", -32000},
                             {"message", "Synthetic session missing"}}}});
          return;
        }
        reply(id, {{"thread", QJsonObject{{"id", "thread-1"}}}});
      } else if (method == "turn/start") {
        reply(id,
              {{"turn",
                QJsonObject{{"id", "turn-" + QString::number(count + 1)}}}});
        prompt({{"input", p.value("input")}});
      } else if (method == "turn/interrupt") {
        helper.kill();
        reply(id, {});
        done("STOPPED", true);
      } else if (id == "native-question")
        done("NATIVE_ANSWER");
      else if (id == "approval") {
        if (m.value("result").toObject().value("decision") == "accept")
          launch();
        else
          done("DENIED");
      }
    } else if (m.value("type") == "control_request") {
      const auto request = m.value("request").toObject();
      const auto requestId = m.value("request_id");
      send({{"type", "control_response"},
            {"response", QJsonObject{{"subtype", "success"},
                                     {"request_id", requestId},
                                     {"response", QJsonObject{}}}}});
      if (request.value("subtype") == "interrupt") {
        helper.kill();
        done();
      }
    } else if (m.value("type") == "user")
      prompt(m.value("message").toObject());
    else if (m.value("type") == "control_response") {
      const auto response = m.value("response").toObject();
      if (response.value("request_id") == "native-question")
        done("NATIVE_ANSWER");
      else if (response.value("request_id") == "approval") {
        if (response.value("response").toObject().value("behavior") == "allow")
          launch();
        else
          done("DENIED");
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
    if (!ReadFile(input, buffer, qMin<DWORD>(available, sizeof buffer), &count,
                  nullptr)) {
      QCoreApplication::quit();
      return;
    }
    bytes.append(buffer, int(count));
    while (bytes.contains('\n')) {
      auto n = bytes.indexOf('\n');
      auto m = QJsonDocument::fromJson(bytes.left(n)).object();
      bytes.remove(0, n + 1);
      receive(m);
    }
#endif
  }
  QTimer timer;
  QProcess helper;
  QByteArray bytes, helperBytes;
  QJsonObject config, result;
  QString turn, session = "68d1dc54-a3ce-46de-b463-956a7950f512",
                scenario = qEnvironmentVariable("ADE_DIRECT_MCP_SCENARIO");
  int count = 0;
  bool codex = false, helperDone = false, resume = false;
};
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  Fixture fixture;
  return app.exec();
}
