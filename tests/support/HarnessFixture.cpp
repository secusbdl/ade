#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>
#include <cstdio>
#include <iostream>

namespace {
void output(const QJsonObject &object, bool split = false) {
  auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
  if (split) {
    for (int i = 0; i < bytes.size(); i += 7) {
      const auto part = bytes.mid(i, 7);
      fwrite(part.constData(), 1, size_t(part.size()), stdout);
      fflush(stdout);
    }
  } else {
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
  }
}
void result(const QJsonValue &id, const QJsonValue &value) {
  output({{"jsonrpc", "2.0"}, {"id", id}, {"result", value}}, true);
}
void notification(const QString &method, const QJsonObject &params) {
  output({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}, true);
}
} // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--child")) {
    QThread::sleep(120);
    return 0;
  }
  const auto scenario = qEnvironmentVariable("ADE_HARNESS_FIXTURE_SCENARIO");
  if (scenario == "malformed") {
    fputs("not JSON\n", stdout);
    fflush(stdout);
    return 0;
  }
  if (scenario == "oversize") {
    QByteArray bytes(9 * 1024 * 1024, 'x');
    fwrite(bytes.data(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
  }
  QProcess child;
  if (scenario == "child") {
    child.start(QCoreApplication::applicationFilePath(), {"--child"});
    child.waitForStarted();
    QFile pid(qEnvironmentVariable("ADE_HARNESS_FIXTURE_PID"));
    if (pid.open(QIODevice::WriteOnly)) {
      pid.write(QByteArray::number(child.processId()));
      pid.close();
    }
  }
  QString thread = "thread-1", turn;
  int turnNumber = 0;
  auto params = [&] {
    return QJsonObject{{"threadId", thread}, {"turnId", turn}};
  };
  auto item = [&](const QJsonObject &value, bool done) {
    auto p = params();
    p["item"] = value;
    notification(done ? "item/completed" : "item/started", p);
  };
  auto done = [&](const QString &status = "completed") {
    notification("turn/completed",
                 {{"threadId", thread},
                  {"turn", QJsonObject{{"id", turn},
                                       {"status", status},
                                       {"items", QJsonArray{}},
                                       {"error", QJsonValue()}}}});
  };
  auto text = [&] {
    item({{"id", "answer"}, {"type", "agentMessage"}, {"text", ""}}, false);
    auto p = params();
    p["itemId"] = "answer";
    p["delta"] = "HARNESS_OK";
    notification("item/agentMessage/delta", p);
    item({{"id", "answer"}, {"type", "agentMessage"}, {"text", "HARNESS_OK"}},
         true);
    done();
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto object =
        QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    const auto method = object.value("method").toString();
    const auto id = object.value("id");
    const auto p = object.value("params").toObject();
    const auto requestsPath =
        qEnvironmentVariable("ADE_HARNESS_FIXTURE_REQUESTS");
    if (!requestsPath.isEmpty() && !method.isEmpty()) {
      QFile requests(requestsPath);
      if (requests.open(QIODevice::WriteOnly | QIODevice::Append)) {
        requests.write(
            QJsonDocument(QJsonObject{{"method", method}, {"params", p}})
                .toJson(QJsonDocument::Compact) +
            '\n');
        requests.close();
      }
    }
    if (scenario == "timeout" && method != "initialize")
      continue;
    if (method == "initialize")
      result(id, QJsonObject{{"userAgent", "native-fixture"}});
    else if (method == "account/read")
      result(id, QJsonObject{
                     {"requiresOpenaiAuth", true},
                     {"account", scenario == "signed-out"
                                     ? QJsonValue()
                                     : QJsonValue(QJsonObject{
                                           {"type", "chatgpt"},
                                           {"email", "private@example.test"},
                                           {"planType", "plus"}})}});
    else if (method == "account/login/start") {
      result(
          id,
          QJsonObject{
              {"type", "chatgpt"},
              {"loginId", "login-1"},
              {"authUrl",
               scenario == "bad-url"
                   ? "https://evil.test/login"
                   : "https://auth.openai.com/oauth/authorize?state=fixture"}});
      notification("account/login/completed",
                   {{"loginId", "wrong-login"}, {"success", true}});
      if (scenario != "hold-login")
        notification("account/login/completed",
                     {{"loginId", "login-1"}, {"success", true}});
    } else if (method == "account/login/cancel")
      result(id, QJsonObject{{"status", "canceled"}});
    else if (method == "model/list") {
      const bool second = p.contains("cursor");
      result(id,
             QJsonObject{
                 {"data",
                  QJsonArray{QJsonObject{
                      {"id", second ? "model-b" : "model-a"},
                      {"model", second ? "model-b" : "model-a"},
                      {"displayName", "Fixture"},
                      {"isDefault", !second},
                      {"supportedReasoningEfforts",
                       QJsonArray{QJsonObject{{"reasoningEffort", "low"}}}}}}},
                 {"nextCursor", second ? QJsonValue() : QJsonValue("page2")}});
    } else if (method == "account/rateLimits/read")
      result(id, QJsonObject{
                     {"rateLimits",
                      QJsonObject{{"primary", QJsonObject{{"usedPercent", 1},
                                                          {"windowDurationMins",
                                                           300}}}}}});
    else if (method == "thread/start" || method == "thread/resume" ||
             method == "thread/fork") {
      if ((method == "thread/resume" && scenario == "resume-error") ||
          scenario == "thread-error") {
        output({{"id", id},
                {"error", QJsonObject{{"code", -1},
                                      {"message", "secret-fixture-error"}}}});
        continue;
      }
      thread = method == "thread/fork"
                   ? "thread-fork"
                   : p.value("threadId").toString("thread-1");
      result(id, QJsonObject{{"thread", QJsonObject{{"id", thread}}},
                             {"model", "model-a"}});
    } else if (method == "turn/start") {
      turn = QString("turn-%1").arg(++turnNumber);
      notification(
          "turn/started",
          {{"threadId", thread},
           {"turn", QJsonObject{{"id", turn}, {"status", "inProgress"}}}});
      auto reasoning = params();
      reasoning["itemId"] = "reason";
      reasoning["summaryIndex"] = 0;
      reasoning["delta"] = "Checking ";
      notification("item/reasoning/summaryTextDelta",
                   reasoning); // Deliberately before the response.
      result(id, QJsonObject{{"turn", QJsonObject{{"id", turn},
                                                  {"status", "inProgress"}}}});
      item({{"id", "reason"},
            {"type", "reasoning"},
            {"summary", QJsonArray{"Checking "}}},
           true);
      if (scenario == "hold" || scenario == "child")
        continue;
      if (scenario == "approval" || scenario == "approval-settled") {
        item({{"id", "command"},
              {"type", "commandExecution"},
              {"command", "echo fixture"},
              {"cwd", QDir::currentPath()},
              {"status", "inProgress"}},
             false);
        auto request = params();
        request["itemId"] = "command";
        request["command"] = "echo fixture";
        output({{"id", "server-approval"},
                {"method", "item/commandExecution/requestApproval"},
                {"params", request}});
        // The server resolves the approval itself and completes the turn.
        if (scenario == "approval-settled")
          text();
      } else if (scenario == "question") {
        auto request = params();
        request["itemId"] = "question";
        request["isBlocking"] = true;
        request["questions"] = QJsonArray{QJsonObject{
            {"id", "choice"},
            {"header", "Choice"},
            {"question", "Pick one"},
            {"options",
             QJsonArray{
                 QJsonObject{{"label", "One"}, {"description", "First"}},
                 QJsonObject{{"label", "Two"}, {"description", "Second"}}}}}};
        output({{"id", "server-question"},
                {"method", "item/tool/requestUserInput"},
                {"params", request}});
      } else
        text();
    } else if (method == "turn/interrupt") {
      result(id, QJsonObject{});
      done("interrupted");
    } else if (method.isEmpty() && id == "server-approval" &&
               scenario != "approval-settled") {
      const auto decision =
          object.value("result").toObject().value("decision").toString();
      item({{"id", "command"},
            {"type", "commandExecution"},
            {"command", "echo fixture"},
            {"cwd", QDir::currentPath()},
            {"status", decision == "accept" ? "completed" : "declined"},
            {"aggregatedOutput", decision == "accept" ? "fixture\n" : ""}},
           true);
      text();
    } else if (method.isEmpty() && id == "server-question")
      text();
    else if (!method.isEmpty() && method != "initialized")
      result(id, QJsonObject{});
  }
  return 0;
}
