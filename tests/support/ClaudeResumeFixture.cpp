#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <iostream>
namespace {
void send(QJsonObject v) {
  const auto b = QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n';
  fwrite(b.constData(), 1, size_t(b.size()), stdout);
  fflush(stdout);
}
void log(QJsonObject v) {
  QFile f(qEnvironmentVariable("ADE_CLAUDE_RESUME_LOG"));
  if (f.open(QIODevice::Append))
    f.write(QJsonDocument(v).toJson(QJsonDocument::Compact) + '\n');
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QString session;
  bool resume = false;
  for (const auto &arg : app.arguments()) {
    if (arg.startsWith("--resume=")) {
      session = arg.mid(9);
      resume = true;
    } else if (arg.startsWith("--session-id="))
      session = arg.mid(13);
  }
  const auto mode = qEnvironmentVariable("ADE_CLAUDE_RESUME_MODE");
  log({{"type", "launch"},
       {"resume", resume},
       {"session", session},
       {"cwd", QDir::currentPath()},
       {"args", QJsonArray::fromStringList(app.arguments().mid(1))}});
  auto missing = [&] {
    send({{"type", "result"},
          {"subtype", "error_during_execution"},
          {"is_error", true},
          {"session_id", session},
          {"errors",
           QJsonArray{"No conversation found with session ID: " + session}}});
  };
  auto text = [&](QString value) {
    send({{"type", "assistant"},
          {"session_id", session},
          {"message",
           QJsonObject{{"id", "msg"},
                       {"content", QJsonArray{QJsonObject{
                                       {"type", "text"}, {"text", value}}}}}}});
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto v =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    log(v);
    if (v.value("type") == "control_request") {
      const auto subtype = v.value("request").toObject().value("subtype");
      if (subtype == "initialize" && resume && mode == "missing-before-init") {
        missing();
        continue;
      }
      send({{"type", "control_response"},
            {"response", QJsonObject{{"subtype", "success"},
                                     {"request_id", v.value("request_id")},
                                     {"response", QJsonObject{}}}}});
      if (subtype == "interrupt")
        send({{"type", "result"},
              {"subtype", "success"},
              {"session_id", session},
              {"result", "INTERRUPTED"}});
    } else if (v.value("type") == "user") {
      if (mode == "missing-deadline") {
        if (resume) {
          QThread::msleep(1000);
          missing();
        }
        continue;
      }
      if (mode == "transport" && resume)
        return 1;
      if ((resume && mode.startsWith("missing")) || mode == "missing-always") {
        missing();
        continue;
      }
      if (resume && mode == "partial") {
        text("PARTIAL_KEEP");
        missing();
        continue;
      }
      if (resume && mode == "permission") {
        send({{"type", "control_request"},
              {"request_id", "permission"},
              {"request",
               QJsonObject{
                   {"subtype", "can_use_tool"},
                   {"tool_name", "Read"},
                   {"tool_use_id", "owned-tool"},
                   {"input", QJsonObject{{"file_path", "fixture-only"}}}}}});
        missing();
        continue;
      }
      if (resume && mode != "ordinary") {
        if (mode == "auth" || mode == "quota" || mode == "billing") {
          send({{"type", "assistant"},
                {"session_id", session},
                {"error", mode == "auth"    ? "authentication_failed"
                          : mode == "quota" ? "rate_limit"
                                            : "billing_error"}});
          continue;
        }
        QJsonObject error{
            {"type", "result"},
            {"subtype", "error_during_execution"},
            {"is_error", true},
            {"session_id", session},
            {"errors",
             QJsonArray{"No conversation found with session ID: " + session}}};
        if (mode == "network")
          error["errors"] = QJsonArray{"Network unavailable"};
        if (mode == "arbitrary")
          error["errors"] = QJsonArray{"Unrelated failure"};
        if (mode == "wrong-id")
          error["errors"] = QJsonArray{"No conversation found with session ID: "
                                       "11111111-1111-1111-1111-111111111111"};
        if (mode == "extra-error")
          error["errors"] =
              QJsonArray{"No conversation found with session ID: " + session,
                         "Network unavailable"};
        if (mode == "string-errors")
          error["errors"] = "No conversation found with session ID: " + session;
        if (mode == "wrong-subtype")
          error["subtype"] = "success";
        if (mode == "false-error")
          error["is_error"] = false;
        if (mode == "nested-origin")
          error["parent_tool_use_id"] = "child-tool";
        if (mode == "malformed-origin")
          error["origin"] = "human";
        if (mode == "result-content")
          error["result"] = "UNTRUSTED_REAL_OUTPUT";
        send(error);
        continue;
      }
      text(resume ? "RESUMED_SUCCESS" : "FRESH_SUCCESS");
      send({{"type", "result"},
            {"subtype", "success"},
            {"session_id", session},
            {"result", "DONE"}});
    }
  }
}
