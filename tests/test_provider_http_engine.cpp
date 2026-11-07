#include "core/AgentEngine.h"
#include "core/CustomModelOptions.h"
#include "core/ModelFallback.h"
#include "ui/ModelSelection.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
using namespace ade;
namespace {
QJsonArray definitions() {
  return {QJsonObject{
      {"type", "function"},
      {"function",
       QJsonObject{{"name", "getBrowserTabs"},
                   {"description", "Fixture only"},
                   {"parameters", QJsonObject{{"type", "object"}}}}}}};
}
QJsonObject answer(const QString &protocol, bool tool) {
  const auto args = QJsonObject{};
  const auto name = QString("getBrowserTabs");
  const auto id = QString("fixture-call");
  if (protocol == "responses")
    return {
        {"status", "completed"},
        {"output",
         tool ? QJsonArray{QJsonObject{{"type", "reasoning"},
                                       {"id", "reason"},
                                       {"encrypted_content", "SIGNED_BYTES"},
                                       {"summary", QJsonArray{}}},
                           QJsonObject{{"type", "function_call"},
                                       {"call_id", id},
                                       {"name", name},
                                       {"arguments", "{}"}}}
              : QJsonArray{QJsonObject{
                    {"type", "message"},
                    {"role", "assistant"},
                    {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                       {"text", "DONE"}}}}}}}};
  if (protocol == "anthropic")
    return {
        {"stop_reason", tool ? "tool_use" : "end_turn"},
        {"content",
         tool ? QJsonArray{QJsonObject{{"type", "thinking"},
                                       {"thinking", "Reasoning"},
                                       {"signature", "SIGNED_BYTES"}},
                           QJsonObject{{"type", "tool_use"},
                                       {"id", id},
                                       {"name", name},
                                       {"input", args}}}
              : QJsonArray{QJsonObject{{"type", "text"}, {"text", "DONE"}}}}};
  if (protocol == "google")
    return {
        {"candidates",
         QJsonArray{QJsonObject{
             {"finishReason", "STOP"},
             {"content",
              QJsonObject{
                  {"role", "model"},
                  {"parts",
                   tool ? QJsonArray{QJsonObject{
                                         {"thought", true},
                                         {"text", "Reasoning"},
                                         {"thoughtSignature", "SIGNED_BYTES"}},
                                     QJsonObject{{"functionCall",
                                                  QJsonObject{{"id", id},
                                                              {"name", name},
                                                              {"args", args}}}}}
                        : QJsonArray{QJsonObject{{"text", "DONE"}}}}}}}}}};
  QJsonObject message{{"role", "assistant"},
                      {"content", tool ? "Before tool" : "DONE"}};
  if (tool) {
    message["reasoning_content"] = "SIGNED_BYTES";
    message["tool_calls"] = QJsonArray{QJsonObject{
        {"id", id},
        {"type", "function"},
        {"function", QJsonObject{{"name", name}, {"arguments", "{}"}}}}};
  }
  return {{"choices", QJsonArray{QJsonObject{
                          {"message", message},
                          {"finish_reason", tool ? "tool_calls" : "stop"}}}}};
}
QByteArray sseEvent(const QJsonObject &o) {
  return "data: " + QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n\n";
}
class Endpoint : public QTcpServer {
public:
  QString protocol = "openai", redirectTo;
  bool error = false, stream = false, tool = true, rejectFirst = false;
  QList<QJsonObject> bodies;
  QList<QMap<QByteArray, QByteArray>> headers;
  Endpoint() {
    listen(QHostAddress::LocalHost, 0);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto *s = nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        connect(s, &QTcpSocket::readyRead, this, [this, s, bytes] {
          *bytes += s->readAll();
          auto at = bytes->indexOf("\r\n\r\n");
          if (at < 0)
            return;
          QMap<QByteArray, QByteArray> h;
          for (const auto &l : bytes->left(at).split('\n')) {
            auto c = l.indexOf(':');
            if (c > 0)
              h.insert(l.left(c).trimmed().toLower(), l.mid(c + 1).trimmed());
          }
          auto length = h.value("content-length").toInt();
          if (bytes->size() < at + 4 + length)
            return;
          bodies.append(
              QJsonDocument::fromJson(bytes->mid(at + 4, length)).object());
          headers.append(h);
          s->disconnect(this);
          if (!redirectTo.isEmpty()) {
            s->write("HTTP/1.1 307 Temporary Redirect\r\nLocation: " +
                     redirectTo.toUtf8() +
                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
          }
          QByteArray data, type = "application/json";
          const bool rejected = error || (rejectFirst && bodies.size() == 1);
          if (rejected)
            data = QJsonDocument(
                       QJsonObject{
                           {"error",
                            QJsonObject{
                                {"message",
                                 "private-header secret-option fixture-key"}}}})
                       .toJson(QJsonDocument::Compact);
          else if (stream && bodies.size() == 1) {
            type = "text/event-stream";
            const auto m = answer("openai", true)
                               .value("choices")
                               .toArray()[0]
                               .toObject()
                               .value("message")
                               .toObject();
            auto delta = m;
            auto calls = delta.value("tool_calls").toArray();
            auto call = calls[0].toObject();
            call["index"] = 0;
            calls[0] = call;
            delta["tool_calls"] = calls;
            data =
                sseEvent({{"choices", QJsonArray{QJsonObject{
                                          {"index", 0},
                                          {"delta", delta},
                                          {"finish_reason", "tool_calls"}}}}}) +
                sseEvent({{"choices", QJsonArray{}},
                          {"usage", QJsonObject{{"prompt_tokens", 5},
                                                {"completion_tokens", 3},
                                                {"total_tokens", 8}}}}) +
                "data: [DONE]\n\n";
          } else
            data = QJsonDocument(answer(protocol, tool && bodies.size() == 1))
                       .toJson(QJsonDocument::Compact);
          s->write(
              "HTTP/1.1 " +
              QByteArray(rejected ? (rejectFirst ? "503" : "400") : "200") +
              " Fixture\r\nContent-Type: " + type +
              "\r\nContent-Length: " + QByteArray::number(data.size()) +
              "\r\nConnection: close\r\n\r\n" + data);
          s->disconnectFromHost();
        });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
      }
    });
  }
  QString base() const {
    return "http://127.0.0.1:" + QString::number(serverPort()) + "/v1";
  }
};
ProviderConfig provider(Endpoint &e) {
  ProviderConfig p{"owner",       "Name",        e.base(),
                   "fixture-key", "custom-name", e.protocol};
  QJsonObject options =
      e.protocol == "google" ? QJsonObject{{"serviceTier", "standard"}}
      : e.protocol == "anthropic"
          ? QJsonObject{{"metadata", QJsonObject{{"userId", "secret-option"}}}}
          : QJsonObject{{"user", "secret-option"}};
  p.customRequest = {{"providerInstanceId", p.id},
                     {"modelId", p.model},
                     {"providerOptions", options},
                     {"headers", QJsonObject{{"X-Private", "private-header"}}},
                     {"capabilities", defaultCustomModelCapabilities()}};
  return p;
}
void rows() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p : {"openai", "responses", "anthropic", "google"})
    QTest::newRow(p) << QString(p);
}
void configure(AgentEngine &engine, const ProviderConfig &p) {
  engine.setProvider(p);
  engine.setExternalTools(definitions());
  engine.setAlwaysAllow(true);
}
} // namespace
class ProviderHttpEngineTest : public QObject {
  Q_OBJECT
  QJsonArray captured;
  void record(const Endpoint &endpoint) {
    for (const auto &body : endpoint.bodies)
      captured.append(QJsonObject{
          {"case", QString::fromLatin1(QTest::currentTestFunction()) + "/" +
                       QString::fromLatin1(QTest::currentDataTag())},
          {"body", body}});
  }
private slots:
  void ordinarySelectedModelRoundTrip_data() { rows(); }
  void ordinarySelectedModelRoundTrip() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.protocol = protocol;
    auto p = provider(endpoint);
    p.customRequest = {};
    p.model = "default-model";
    p.reasoningEffort = "high";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 10000;
    p.maxOutputTokens = 32000;
    const auto model = protocol == "anthropic"
                           ? QString("claude-3-haiku-20240307")
                       : protocol == "google" ? QString("gemini-2.0-flash")
                                              : QString("gpt-4.1");
    ProviderConfig selected;
    QVERIFY((ModelSelection{p.id, model, {}, {}, {}}.resolve({p}, &selected)));
    AgentEngine engine;
    configure(engine, selected);
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed);
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &id, const QString &, const QJsonObject &) {
              engine.completeExternalTool(id, "OWNED_TOOL_OUTPUT", false);
            });
    engine.submit("One local tool then done");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    record(endpoint);
    QVERIFY2(failed.isEmpty(),
             failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(endpoint.bodies.size(), 2);
    QVERIFY(
        QJsonDocument(endpoint.bodies[1]).toJson().contains("SIGNED_BYTES"));
    QVERIFY(QJsonDocument(endpoint.bodies[1])
                .toJson()
                .contains("OWNED_TOOL_OUTPUT"));
    for (const auto &body : endpoint.bodies) {
      QVERIFY(!body.contains("reasoning"));
      QVERIFY(!body.contains("reasoning_effort"));
      QVERIFY(!body.contains("output_config"));
      QVERIFY(!body.contains("thinking"));
      if (protocol == "anthropic")
        QCOMPARE(body.value("max_tokens").toInt(), 4096);
    }
  }
  void ordinaryAnthropicBudgetSurvivesToolRoundTrip() {
    Endpoint endpoint;
    endpoint.protocol = "anthropic";
    auto p = provider(endpoint);
    p.customRequest = {};
    p.model = "claude-sonnet-4-5";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 2048;
    p.maxOutputTokens = 100;
    AgentEngine engine;
    configure(engine, p);
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed);
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &id, const QString &, const QJsonObject &) {
              engine.completeExternalTool(id, "OWNED_TOOL_OUTPUT", false);
            });
    engine.submit("One local tool then done");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    record(endpoint);
    QVERIFY(failed.isEmpty());
    QCOMPARE(endpoint.bodies.size(), 2);
    for (const auto &body : endpoint.bodies) {
      QCOMPARE(body.value("max_tokens").toInt(), 2148);
      QCOMPARE(body.value("thinking").toObject().value("budget_tokens").toInt(),
               2048);
    }
    QVERIFY(
        QJsonDocument(endpoint.bodies[1]).toJson().contains("SIGNED_BYTES"));
  }
  void impossibleOrdinaryBudgetNeverPosts_data() {
    QTest::addColumn<int>("budget");
    QTest::newRow("below-api-minimum") << 512;
    QTest::newRow("above-known-output-limit") << 64000;
  }
  void impossibleOrdinaryBudgetNeverPosts() {
    QFETCH(int, budget);
    Endpoint endpoint;
    endpoint.protocol = "anthropic";
    auto p = provider(endpoint);
    p.customRequest = {};
    p.model = "claude-sonnet-4-5";
    p.thinkingMode = "enabled";
    p.thinkingBudget = budget;
    p.maxOutputTokens = 100;
    AgentEngine engine;
    configure(engine, p);
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed);
    engine.submit("Fail locally");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000);
    QCOMPARE(failed.size(), 1);
    QVERIFY(endpoint.bodies.isEmpty());
    QVERIFY(!engine.lastTurnSucceeded());
    QVERIFY(failed[0][0].toString().contains("thinking budget"));
  }

  void fallbackUsesNewModelSettingsOnly() {
    Endpoint endpoint;
    endpoint.protocol = "openai";
    endpoint.rejectFirst = true;
    endpoint.tool = false;
    auto p = provider(endpoint);
    p.customRequest = {};
    p.model = "gpt-5";
    p.reasoningEffort = "high";
    p.maxOutputTokens = 999;
    ProviderConfig selected;
    QVERIFY(
        (ModelSelection{p.id, "gpt-4.1", {}, {}, {}}.resolve({p}, &selected)));
    AgentEngine engine;
    engine.setModelChain({ModelConfig{p}, ModelConfig{selected}},
                         "explicit-fallback");
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed),
        switched(&engine, &AgentEngine::effectiveProviderChanged);
    engine.submit("Local fallback");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    record(endpoint);
    QVERIFY(failed.isEmpty());
    QCOMPARE(endpoint.bodies.size(), 2);
    QCOMPARE(switched.size(), 2);
    QCOMPARE(switched[0][1].toString(), QString("gpt-5"));
    QCOMPARE(switched[1][0].toString(), p.id);
    QCOMPARE(switched[1][1].toString(), QString("gpt-4.1"));
    QCOMPARE(switched[1][2].toInt(), 1);
    QCOMPARE(endpoint.bodies[0].value("reasoning_effort").toString(),
             QString("high"));
    QCOMPARE(endpoint.bodies[1].value("model").toString(), QString("gpt-4.1"));
    QVERIFY(!endpoint.bodies[1].contains("reasoning_effort"));
    QVERIFY(!endpoint.bodies[1].contains("max_tokens"));
    QVERIFY(!endpoint.bodies[1].contains("max_completion_tokens"));
    QCOMPARE(engine.provider().model, QString("gpt-4.1"));
  }
  void cleanupTestCase() {
    const auto path = qEnvironmentVariable("ADE_PROVIDER_HTTP_ENGINE_CAPTURE");
    if (path.isEmpty())
      return;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(captured).toJson()) > 0);
  }
};
QTEST_GUILESS_MAIN(ProviderHttpEngineTest)
#include "test_provider_http_engine.moc"
