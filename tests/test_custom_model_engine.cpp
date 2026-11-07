#include "core/AgentEngine.h"
#include "core/CustomModelOptions.h"
#include "core/ModelFallback.h"
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
  bool error = false, stream = false, tool = true;
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
          if (error)
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
          s->write("HTTP/1.1 " + QByteArray(error ? "400" : "200") +
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
class CustomEngineTest : public QObject {
  Q_OBJECT
private slots:
  void actualRoundTrip_data() { rows(); }
  void actualRoundTrip() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.protocol = protocol;
    auto p = provider(endpoint);
    AgentEngine engine;
    configure(engine, p);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QSignalSpy errors(&engine, &AgentEngine::failed);
    int calls = 0;
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &id, const QString &name, const QJsonObject &) {
              ++calls;
              QCOMPARE(name, QString("getBrowserTabs"));
              engine.completeExternalTool(id, "EXACT_TOOL_OUTPUT high", false);
            });
    engine.submit("One fixture tool then DONE");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors[0][0].toString()));
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(calls, 1);
    QCOMPARE(endpoint.bodies.size(), 2);
    QVERIFY(
        QJsonDocument(endpoint.bodies[1]).toJson().contains("SIGNED_BYTES"));
    QVERIFY(QJsonDocument(endpoint.bodies[1])
                .toJson()
                .contains("EXACT_TOOL_OUTPUT high"));
    for (int i = 0; i < endpoint.bodies.size(); ++i) {
      QCOMPARE(endpoint.headers[i].value("x-private"),
               QByteArray("private-header"));
      QVERIFY(!QJsonDocument(endpoint.bodies[i])
                   .toJson()
                   .contains("private-header"));
      QVERIFY(!QJsonDocument(endpoint.bodies[i])
                   .toJson()
                   .contains("customRequest"));
    }
    const auto saved = QJsonDocument(engine.messages()).toJson();
    QVERIFY(!saved.contains("private-header"));
    QVERIFY(!saved.contains("secret-option"));
    QVERIFY(saved.contains("SIGNED_BYTES"));
    QVERIFY(saved.contains("DONE"));
  }
  void noToolsCannotDispatch_data() { rows(); }
  void noToolsCannotDispatch() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.protocol = protocol;
    auto p = provider(endpoint);
    auto caps = p.customRequest.value("capabilities").toObject();
    caps["toolCalling"] = false;
    p.customRequest["capabilities"] = caps;
    AgentEngine engine;
    configure(engine, p);
    QSignalSpy done(&engine, &AgentEngine::finished),
        started(&engine, &AgentEngine::toolStarted),
        external(&engine, &AgentEngine::externalToolRequested),
        failed(&engine, &AgentEngine::failed);
    engine.submit("No tools");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY(!engine.lastTurnSucceeded());
    QCOMPARE(failed.size(), 1);
    QVERIFY(started.isEmpty());
    QVERIFY(external.isEmpty());
    QCOMPARE(endpoint.bodies.size(), 1);
    QVERIFY(!endpoint.bodies[0].contains("tools"));
    QVERIFY(QJsonDocument(engine.messages()).toJson().contains("interrupted"));
  }
  void usageOnlySseAndToolOutputSurvive() {
    Endpoint endpoint;
    endpoint.stream = true;
    AgentEngine engine;
    configure(engine, provider(endpoint));
    QSignalSpy done(&engine, &AgentEngine::finished);
    connect(&engine, &AgentEngine::externalToolRequested, &engine,
            [&](const QString &id, const QString &, const QJsonObject &) {
              engine.completeExternalTool(id, "SSE_TOOL_OUTPUT", false);
            });
    engine.submit("Stream");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(endpoint.bodies.size(), 2);
    QVERIFY(
        QJsonDocument(endpoint.bodies[1]).toJson().contains("SSE_TOOL_OUTPUT"));
    QVERIFY(QJsonDocument(engine.messages()).toJson().contains("Before tool"));
  }
  void privateErrorValuesAreRedacted_data() {
    QTest::addColumn<QString>("protocol");
    for (const auto &p : {"openai", "responses", "anthropic"})
      QTest::newRow(p) << QString(p);
  }
  void privateErrorValuesAreRedacted() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.protocol = protocol;
    endpoint.error = true;
    AgentEngine engine;
    configure(engine, provider(endpoint));
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed);
    engine.submit("Fail");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QCOMPARE(failed.size(), 1);
    const auto joined = failed[0][0].toString().toUtf8() +
                        QJsonDocument(engine.messages()).toJson() +
                        QJsonDocument(engine.displayMessages()).toJson();
    for (const auto &s : {"private-header", "secret-option", "fixture-key"})
      QVERIFY(!joined.contains(s));
    QVERIFY(joined.contains("REDACTED"));
  }
  void customHeadersStayOnSelectedOrigin_data() { rows(); }
  void customHeadersStayOnSelectedOrigin() {
    QFETCH(QString, protocol);
    Endpoint destination, source;
    source.protocol = protocol;
    source.redirectTo = destination.base();
    AgentEngine engine;
    configure(engine, provider(source));
    QSignalSpy done(&engine, &AgentEngine::finished),
        failed(&engine, &AgentEngine::failed);
    engine.submit("Reject cross-origin redirect");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QCOMPARE(source.bodies.size(), 1);
    QCOMPARE(source.headers[0].value("x-private"),
             QByteArray("private-header"));
    QCOMPARE(failed.size(), 1);
    QVERIFY(destination.bodies.isEmpty());
    QVERIFY(!destination.hasPendingConnections());
  }
  void invalidBindingNeverPostsAndObserverMayDelete() {
    Endpoint endpoint;
    auto p = provider(endpoint);
    p.customRequest["providerInstanceId"] = "foreign";
    auto *engine = new AgentEngine;
    QPointer<AgentEngine> guard(engine);
    configure(*engine, p);
    connect(engine, &AgentEngine::failed, engine,
            [engine](const QString &) { delete engine; });
    engine->submit("Reject locally");
    QVERIFY(guard.isNull());
    QVERIFY(endpoint.bodies.isEmpty());
  }
  void privateChangeInvalidatesFallback() {
    Endpoint endpoint;
    auto p = provider(endpoint);
    ModelFallback state;
    QList<ModelConfig> models{{p}, {p}};
    models[1].provider.id = "other";
    QCOMPARE(state.resolve("preset", models, 1000), 0);
    QVERIFY(state.advance(2, 1001));
    QCOMPARE(state.resolve("preset", models, 1002), 1);
    auto c = models[0].provider.customRequest.value("headers").toObject();
    c["X-Private"] = "rotated";
    models[0].provider.customRequest["headers"] = c;
    QCOMPARE(state.resolve("preset", models, 1003), 0);
    QVERIFY(state.advance(2, 1004));
    models[0].provider.requestOverrides["maxOutputTokens"] = 500;
    QCOMPARE(state.resolve("preset", models, 1005), 0);
    QVERIFY(!QJsonDocument(models[0].provider.publicJson())
                 .toJson()
                 .contains("rotated"));
  }
};
QTEST_GUILESS_MAIN(CustomEngineTest)
#include "test_custom_model_engine.moc"
