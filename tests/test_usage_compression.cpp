#include "core/AgentEngine.h"
#include "core/ProviderProtocol.h"
#include "core/ProviderStore.h"
#include "core/ProviderUsage.h"
#include "core/UtilityService.h"
#include "ui/ModelSelection.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray sseEvent(const QJsonObject &value) {
  return "data: " + QJsonDocument(value).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QJsonObject usage(const QString &protocol, int total) {
  if (protocol == "google")
    return {{"promptTokenCount", total - 10},
            {"candidatesTokenCount", 8},
            {"thoughtsTokenCount", 2},
            {"totalTokenCount", total},
            {"cachedContentTokenCount", 3}};
  if (protocol == "anthropic")
    return {{"input_tokens", total - 15},
            {"cache_read_input_tokens", 3},
            {"cache_creation_input_tokens", 2},
            {"output_tokens", 10}};
  const bool responses = protocol == "responses";
  return {{responses ? "input_tokens" : "prompt_tokens", total - 10},
          {responses ? "output_tokens" : "completion_tokens", 10},
          {"total_tokens", total},
          {responses ? "input_tokens_details" : "prompt_tokens_details",
           QJsonObject{{"cached_tokens", 3}}},
          {responses ? "output_tokens_details" : "completion_tokens_details",
           QJsonObject{{"reasoning_tokens", 2}}}};
}
QJsonObject response(const QString &protocol, const QString &text, bool tool,
                     int total) {
  if (protocol == "responses") {
    QJsonArray output{QJsonObject{{"type", "reasoning"},
                                  {"id", "signed-item"},
                                  {"encrypted_content", "SIGNED_BYTES"},
                                  {"summary", QJsonArray{}}}};
    if (tool)
      output.append(QJsonObject{{"type", "function_call"},
                                {"id", "fn-item"},
                                {"call_id", "call-reused"},
                                {"name", "getBrowserTabs"},
                                {"arguments", "{}"}});
    else
      output.append(QJsonObject{
          {"type", "message"},
          {"role", "assistant"},
          {"content",
           QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}}}}});
    return {{"status", "completed"},
            {"model", "actual-model"},
            {"output", output},
            {"usage", usage(protocol, total)}};
  }
  if (protocol == "google") {
    QJsonArray parts{QJsonObject{{"text", "private reasoning"},
                                 {"thought", true},
                                 {"thoughtSignature", "SIGNED_BYTES"}}};
    parts.append(tool ? QJsonObject{{"functionCall",
                                     QJsonObject{{"id", "call-reused"},
                                                 {"name", "getBrowserTabs"},
                                                 {"args", QJsonObject{}}}}}
                      : QJsonObject{{"text", text}});
    return {{"candidates",
             QJsonArray{QJsonObject{
                 {"content", QJsonObject{{"role", "model"}, {"parts", parts}}},
                 {"finishReason", "STOP"}}}},
            {"usageMetadata", usage(protocol, total)}};
  }
  if (protocol == "anthropic") {
    QJsonArray content{QJsonObject{{"type", "thinking"},
                                   {"thinking", "private reasoning"},
                                   {"signature", "SIGNED_BYTES"}}};
    content.append(tool ? QJsonObject{{"type", "tool_use"},
                                      {"id", "call-reused"},
                                      {"name", "getBrowserTabs"},
                                      {"input", QJsonObject{}}}
                        : QJsonObject{{"type", "text"}, {"text", text}});
    return {{"model", "actual-model"},
            {"content", content},
            {"stop_reason", tool ? "tool_use" : "end_turn"},
            {"usage", usage(protocol, total)}};
  }
  QJsonObject message{{"role", "assistant"},
                      {"content", text},
                      {"reasoning_content", "SIGNED_BYTES"}};
  if (tool)
    message["tool_calls"] = QJsonArray{
        QJsonObject{{"id", "call-reused"},
                    {"type", "function"},
                    {"function", QJsonObject{{"name", "getBrowserTabs"},
                                             {"arguments", "{}"}}}}};
  return {{"model", "actual-model"},
          {"choices", QJsonArray{QJsonObject{
                          {"message", message},
                          {"finish_reason", tool ? "tool_calls" : "stop"}}}},
          {"usage", usage(protocol, total)}};
}
QByteArray streamed(const QString &protocol, bool tool, int total) {
  const auto full =
      response(protocol, tool ? QString() : QString("FINAL"), tool, total);
  if (protocol == "responses")
    return sseEvent({{"type", "response.completed"}, {"response", full}});
  if (protocol == "google")
    return sseEvent(full);
  if (protocol == "anthropic") {
    auto initial = usage(protocol, total);
    initial["output_tokens"] = 1;
    QByteArray body =
        sseEvent({{"type", "message_start"},
                  {"message", QJsonObject{{"model", "actual-model"},
                                          {"usage", initial}}}});
    int index = 0;
    for (const auto &block : full["content"].toArray()) {
      body += sseEvent({{"type", "content_block_start"},
                        {"index", index},
                        {"content_block", block}});
      if (block.toObject()["type"] == "tool_use")
        body += sseEvent({{"type", "content_block_delta"},
                          {"index", index},
                          {"delta", QJsonObject{{"type", "input_json_delta"},
                                                {"partial_json", "{}"}}}});
      body += sseEvent({{"type", "content_block_stop"}, {"index", index++}});
    }
    body += sseEvent({{"type", "message_delta"},
                      {"delta", QJsonObject{{"stop_reason",
                                             tool ? "tool_use" : "end_turn"}}},
                      {"usage", QJsonObject{{"output_tokens", 10}}}});
    body += sseEvent({{"type", "message_stop"}});
    return body;
  }
  auto message =
      full["choices"].toArray().first().toObject()["message"].toObject();
  message.remove("role");
  QByteArray body = sseEvent(
      {{"model", "actual-model"},
       {"choices", QJsonArray{QJsonObject{
                       {"delta", message},
                       {"finish_reason", tool ? "tool_calls" : "stop"}}}}});
  const QJsonObject counted{{"choices", QJsonArray{}},
                            {"usage", usage(protocol, total)}};
  return body + sseEvent(counted) + sseEvent(counted) + "data: [DONE]\n\n";
}
struct Reply {
  QByteArray body;
  QByteArray type = "text/event-stream";
  int status = 200;
  bool hold = false;
};
class Server : public QObject {
public:
  QTcpServer server;
  QString protocol = "openai";
  QList<QJsonObject> requests;
  QStringList phases;
  QList<QPointer<QTcpSocket>> held;
  std::function<Reply(bool, int)> respond;
  int mainCount = 0;
  Server() {
    server.listen(QHostAddress::LocalHost, 0);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          if (socket->property("handled").toBool()) {
            socket->readAll();
            return;
          }
          const auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          const int end = bytes.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(end).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < end + 4 + length)
            return;
          socket->setProperty("handled", true);
          const auto body =
              QJsonDocument::fromJson(bytes.mid(end + 4, length)).object();
          requests.append(body);
          const bool utility = body["stream"] == false ||
                               bytes.left(end).contains(":generateContent");
          phases.append(utility ? "utility" : "main");
          const auto result =
              respond ? respond(utility, utility ? mainCount : ++mainCount)
                      : Reply{streamed(protocol, false, 100)};
          if (result.hold) {
            held.append(socket);
            return;
          }
          socket->write(
              "HTTP/1.1 " + QByteArray::number(result.status) +
              " Fixture\r\nContent-Type: " + result.type +
              "\r\nContent-Length: " + QByteArray::number(result.body.size()) +
              "\r\nConnection: close\r\n\r\n" + result.body);
          socket->disconnectFromHost();
        });
      }
    });
  }
  ProviderConfig provider(int context = 0) const {
    auto p = ProviderConfig{
        "fixture",
        "Fixture",
        QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
        "fixture-only-key",
        "fixture-model",
        protocol};
    p.contextWindowTokens = context;
    return p;
  }
  Reply summary(bool fail = false) const {
    return fail
               ? Reply{"{\"error\":{\"message\":\"fixture unavailable\"}}",
                       "application/json", 400}
               : Reply{
                     QJsonDocument(
                         response(protocol,
                                  "Summary preserves decisions, relevant paths "
                                  "and completed work for the next agent step.",
                                  false, 100))
                         .toJson(QJsonDocument::Compact),
                     "application/json"};
  }
};
QJsonArray history() {
  QJsonArray out;
  for (int i = 0; i < 16; ++i) {
    out.append(QJsonObject{{"role", "user"},
                           {"content", QString("OLD_USER_%1").arg(i)}});
    out.append(QJsonObject{{"role", "assistant"},
                           {"content", QString("OLD_ANSWER_%1").arg(i)}});
  }
  return out;
}
int boundary(const QJsonArray &rows) {
  for (int i = rows.size() - 1; i > 0; --i)
    if (!rows[i].toObject()["ade_compressedHistory"].toString().isEmpty())
      return i;
  return -1;
}
void tools(AgentEngine &engine) {
  engine.setExternalTools({QJsonObject{
      {"type", "function"},
      {"function",
       QJsonObject{{"name", "getBrowserTabs"},
                   {"parameters", QJsonObject{{"type", "object"}}}}}}});
  QObject::connect(
      &engine, &AgentEngine::externalToolRequested, &engine,
      [&engine](const QString &id, const QString &, const QJsonObject &) {
        engine.completeExternalTool(id, "EXACT_TOOL_RESULT", false);
      });
}
void protocols() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p :
       QStringList{"openai", "responses", "anthropic", "google"})
    QTest::newRow(qPrintable(p)) << p;
}
} // namespace
class UsageCompressionTest : public QObject {
  Q_OBJECT
private slots:
  void countersAreCumulativeAndCacheAware() {
    ProviderUsage a;
    a.consume(
        "anthropic",
        {{"type", "message_start"},
         {"message",
          QJsonObject{{"usage", QJsonObject{{"input_tokens", 100},
                                            {"cache_read_input_tokens", 20},
                                            {"cache_creation_input_tokens", 30},
                                            {"output_tokens", 1}}}}}});
    a.consume("anthropic", {{"usage", QJsonObject{{"output_tokens", 8}}}});
    a.consume("anthropic", {{"usage", QJsonObject{{"output_tokens", 8}}}});
    QCOMPARE(a.counters()["inputTokens"], 150);
    QCOMPARE(a.counters()["outputTokens"], 8);
    QCOMPARE(a.counters()["totalTokens"], 158);
    ProviderUsage g;
    g.consume("google",
              {{"usageMetadata", QJsonObject{{"promptTokenCount", 100},
                                             {"cachedContentTokenCount", 20},
                                             {"candidatesTokenCount", 8},
                                             {"thoughtsTokenCount", 2},
                                             {"totalTokenCount", 110}}}});
    QCOMPARE(g.counters()["inputTokens"], 100);
    QCOMPARE(g.counters()["outputTokens"], 10);
    QCOMPARE(g.counters()["totalTokens"], 110);
    ProviderUsage o;
    o.consume("openai", {{"usage", usage("openai", 110)}});
    o.consume("openai", {{"usage", usage("openai", 110)}});
    QCOMPARE(o.counters()["totalTokens"], 110);
    QCOMPARE(o.counters()["outputTokens"], 10);
    QCOMPARE(o.counters()["reasoningTokens"], 2);
  }
  void invalidAndMissingCountersStayUnknown() {
    ProviderUsage p;
    p.consume("openai", {{"usage", QJsonObject{{"prompt_tokens", -1},
                                               {"completion_tokens", 1.5},
                                               {"total_tokens", "100"}}}});
    QVERIFY(p.empty());
    p.consume("openai", {{"usage", QJsonObject{{"total_tokens", 2000000001.0},
                                               {"prompt_tokens", 20}}}});
    QVERIFY(!p.counters().contains("totalTokens"));
    p.consume("openai", {{"usage", QJsonObject{{"completion_tokens", 5}}}});
    QCOMPARE(p.counters()["totalTokens"], 25);
    QVERIFY(!ProviderUsage::validCounters({{"arbitrary", 4}}));
    QVERIFY(!ProviderUsage::validCounters({{"totalTokens", true}}));
    p.consume("google",
              {{"usageMetadata", QJsonObject{{"promptTokenCount", 10}}}});
    QVERIFY(!p.counters().contains("totalTokens"));
    QCOMPARE(p.counters()["inputTokens"], 10);
  }
  void usageRoundTripsAndNeverEntersWire_data() { protocols(); }
  void usageRoundTripsAndNeverEntersWire() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    server.respond = [&](bool, int i) {
      return Reply{streamed(protocol, i == 1, i == 1 ? 6000 : 100)};
    };
    AgentEngine engine;
    engine.setProvider(server.provider());
    tools(engine);
    QSignalSpy done(&engine, &AgentEngine::finished),
        used(&engine, &AgentEngine::usageChanged),
        failed(&engine, &AgentEngine::failed);
    engine.submit("Inspect tabs");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(used.size(), 2);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(used[0][0].toJsonObject()["counters"].toObject()["totalTokens"],
             6000);
    QCOMPARE(engine.latestUsage()["counters"].toObject()["totalTokens"], 100);
    const auto saved = engine.messages();
    QCOMPARE(saved.size(), 4);
    QCOMPARE(saved[2].toObject()["content"], "EXACT_TOOL_RESULT");
    const auto wire = QJsonDocument(server.requests[1]).toJson();
    QVERIFY(wire.contains("SIGNED_BYTES"));
    QVERIFY(wire.contains("EXACT_TOOL_RESULT"));
    QVERIFY(!wire.contains("cachedInputTokens"));
    QVERIFY(!wire.contains("actualModel"));
    auto clean = saved;
    for (int i = 0; i < clean.size(); ++i) {
      auto row = clean[i].toObject();
      auto meta = row["metadata"].toObject();
      meta.remove("usage");
      row["metadata"] = meta;
      clean[i] = row;
    }
    QCOMPARE(
        ProviderProtocol::prepare(server.provider(), saved, "system", {}).body,
        ProviderProtocol::prepare(server.provider(), clean, "system", {}).body);
    AgentEngine restored;
    restored.setProvider(server.provider());
    restored.setMessages(saved);
    QCOMPARE(restored.latestUsage(), engine.latestUsage());
    auto replacement = server.provider();
    replacement.model = "other";
    restored.setProvider(replacement);
    QVERIFY(restored.latestUsage().isEmpty());
  }
  void automaticCompressionPreservesToolsAndOriginalHistory_data() {
    protocols();
  }
  void automaticCompressionPreservesToolsAndOriginalHistory() {
    QFETCH(QString, protocol);
    Server server;
    server.protocol = protocol;
    server.respond = [&](bool utility, int i) {
      return utility ? server.summary()
                     : Reply{streamed(protocol, i == 1, i == 1 ? 6000 : 100)};
    };
    AgentEngine engine;
    engine.setProvider(server.provider(10000));
    const auto original = history();
    engine.setMessages(original);
    tools(engine);
    QSignalSpy done(&engine, &AgentEngine::finished),
        compressed(&engine, &AgentEngine::compressionFinished),
        failed(&engine, &AgentEngine::failed);
    engine.submit("Read tabs");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(server.phases, QStringList({"main", "utility", "main"}));
    QCOMPARE(compressed.size(), 1);
    QVERIFY(compressed[0][0].toBool());
    const auto saved = engine.messages();
    const int b = boundary(saved);
    QVERIFY(b > 0);
    QCOMPARE(saved[b].toObject()["role"], "user");
    for (int i = 0; i < original.size(); ++i) {
      auto actual = saved[i].toObject();
      actual.remove("ade_compressedHistory");
      auto meta = actual["metadata"].toObject();
      meta.remove("compression");
      if (meta.isEmpty())
        actual.remove("metadata");
      else
        actual["metadata"] = meta;
      QCOMPARE(actual, original[i].toObject());
    }
    const auto wire = QJsonDocument(server.requests.last()).toJson();
    QVERIFY(wire.contains("Summary preserves decisions"));
    QVERIFY(!wire.contains("OLD_USER_0\""));
    QVERIFY(wire.contains("SIGNED_BYTES"));
    QVERIFY(wire.contains("EXACT_TOOL_RESULT"));
    QVERIFY(!wire.contains("usageId"));
    QVERIFY(!wire.contains("totalTokens"));
  }
  void unknownLimitsAndThresholdEqualityDoNotCompress() {
    for (int limit : {0, 12000}) {
      Server s;
      s.respond = [&](bool, int) {
        return Reply{streamed("openai", false, 6000)};
      };
      AgentEngine e;
      e.setProvider(s.provider(limit));
      e.setMessages(history());
      QSignalSpy done(&e, &AgentEngine::finished);
      e.submit("Continue");
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
      QCOMPARE(s.requests.size(), 1);
      QCOMPARE(boundary(e.messages()), -1);
    }
  }
  void failedCompressionContinuesWithFullHistory() {
    Server s;
    s.respond = [&](bool utility, int i) {
      return utility ? s.summary(true)
                     : Reply{streamed("openai", i == 1, i == 1 ? 6000 : 100)};
    };
    AgentEngine e;
    e.setProvider(s.provider(10000));
    e.setMessages(history());
    tools(e);
    QSignalSpy done(&e, &AgentEngine::finished),
        compressed(&e, &AgentEngine::compressionFinished);
    e.submit("Continue");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QCOMPARE(compressed.size(), 1);
    QVERIFY(!compressed[0][0].toBool());
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(boundary(e.messages()), -1);
    QVERIFY(QJsonDocument(s.requests.last()).toJson().contains("OLD_USER_0"));
  }
  void cancellationDuringUtilityCannotCommitOrResume() {
    Server s;
    s.respond = [&](bool utility, int) {
      return utility ? Reply{{}, "application/json", 200, true}
                     : Reply{streamed("openai", false, 6000)};
    };
    AgentEngine e;
    e.setProvider(s.provider(10000));
    e.setMessages(history());
    QSignalSpy done(&e, &AgentEngine::finished),
        compressed(&e, &AgentEngine::compressionFinished);
    e.submit("Continue");
    QTRY_COMPARE_WITH_TIMEOUT(s.held.size(), 1, 3000);
    QVERIFY(e.isCompressing());
    const auto snapshot = e.messages();
    e.cancel();
    QCOMPARE(done.size(), 1);
    QVERIFY(!e.isCompressing());
    QCOMPARE(e.messages(), snapshot);
    QCOMPARE(boundary(e.messages()), -1);
    QTest::qWait(50);
    QCOMPARE(compressed.size(), 0);
    QCOMPARE(s.requests.size(), 2);
    s.respond = [&](bool, int) {
      return Reply{streamed("openai", false, 100)};
    };
    auto p = s.provider(0);
    p.model = "replacement";
    e.setProvider(p);
    e.submit("New turn");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 3000);
    QCOMPARE(e.messages().last().toObject()["content"], "FINAL");
    QCOMPARE(boundary(e.messages()), -1);
  }
  void completedCompressionRestoresWithoutDuplicateUtility() {
    Server s;
    s.respond = [&](bool utility, int) {
      return utility ? s.summary() : Reply{streamed("openai", false, 6000)};
    };
    AgentEngine e;
    e.setProvider(s.provider(10000));
    e.setMessages(history());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("Continue");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    const auto saved = e.messages();
    QVERIFY(boundary(saved) > 0);
    AgentEngine restored;
    restored.setProvider(s.provider(10000));
    restored.setMessages(saved);
    s.respond = [&](bool, int) {
      return Reply{streamed("openai", false, 100)};
    };
    QSignalSpy finished(&restored, &AgentEngine::finished);
    restored.submit("Restarted");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(s.phases, QStringList({"main", "utility", "main"}));
  }
  void observersCanCancelDeleteAndReplace_data() {
    QTest::addColumn<QString>("signal");
    QTest::addColumn<QString>("action");
    for (const auto &s : QStringList{"usage", "start", "finish"})
      for (const auto &a : QStringList{"cancel", "delete", "replace"})
        QTest::newRow(qPrintable(s + "-" + a)) << s << a;
  }
  void observersCanCancelDeleteAndReplace() {
    QFETCH(QString, signal);
    QFETCH(QString, action);
    Server s;
    s.respond = [&](bool utility, int i) {
      return utility ? s.summary()
                     : Reply{streamed("openai", false, i == 1 ? 6000 : 100)};
    };
    QPointer<AgentEngine> e = new AgentEngine;
    e->setProvider(s.provider(10000));
    e->setMessages(history());
    int fired = 0;
    auto observe = [&] {
      if (fired++)
        return;
      if (action == "delete") {
        delete e.data();
        return;
      }
      e->cancel();
      if (action == "replace") {
        e->setProvider(s.provider());
        e->submit("Replacement");
      }
    };
    if (signal == "usage")
      connect(e, &AgentEngine::usageChanged, this,
              [&](const QJsonObject &) { observe(); });
    if (signal == "start")
      connect(e, &AgentEngine::compressionStarted, this, observe);
    if (signal == "finish")
      connect(e, &AgentEngine::compressionFinished, this,
              [&](bool, const QString &) { observe(); });
    e->submit("Continue");
    QTRY_VERIFY_WITH_TIMEOUT(fired > 0, 4000);
    QTRY_VERIFY_WITH_TIMEOUT(!e || !e->isBusy(), 4000);
    if (e) {
      if (action == "replace")
        QVERIFY(e->lastTurnSucceeded());
      else
        QVERIFY(!e->lastTurnSucceeded());
      delete e.data();
    }
  }
  void explicitAndDiscoveredLimitsAreModelScopedAndPersisted() {
    Server s;
    auto p = s.provider(8000);
    QTemporaryDir dir;
    const auto path = dir.filePath("providers.json");
    QString error;
    QVERIFY(ProviderStore::save(path, {p}, &error));
    QCOMPARE(ProviderStore::load(path, &error).first().contextWindowTokens,
             8000);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto before = file.readAll();
    file.close();
    p.contextWindowTokens = -1;
    QVERIFY(!ProviderStore::save(path, {p}, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    p = s.provider(8000);
    const QJsonArray entries{
        QJsonObject{{"providerInstanceId", p.id}, {"modelId", "other"}}};
    const auto catalog = cleanModelCatalog(
        p, {QJsonObject{{"id", "other"}, {"inputTokenLimit", 16000}}});
    QCOMPARE(
        resolveModelEntries({p}, entries).first().provider.contextWindowTokens,
        0);
    QCOMPARE(resolveModelEntries({p}, entries, {{p.id, catalog}})
                 .first()
                 .provider.contextWindowTokens,
             16000);
    auto stale = catalog;
    stale["baseUrl"] = "https://different.invalid";
    QCOMPARE(resolveModelEntries({p}, entries, {{p.id, stale}})
                 .first()
                 .provider.contextWindowTokens,
             0);
    stale = catalog;
    stale["protocol"] = "google";
    QCOMPARE(resolveModelEntries({p}, entries, {{p.id, stale}})
                 .first()
                 .provider.contextWindowTokens,
             0);
    const auto invalid = cleanModelCatalog(
        p, {QJsonObject{{"id", "other"}, {"inputTokenLimit", 1.5}}});
    QCOMPARE(resolveModelEntries({p}, entries, {{p.id, invalid}})
                 .first()
                 .provider.contextWindowTokens,
             0);
  }
  void utilityPairsRepeatedCallIdsWithinEachBatch() {
    Server s;
    s.respond = [&](bool, int) { return s.summary(); };
    QJsonArray rows;
    for (const auto &tag : QStringList{"FIRST", "SECOND"}) {
      rows.append(QJsonObject{
          {"role", "assistant"},
          {"content", tag + "_ASSISTANT"},
          {"tool_calls",
           QJsonArray{QJsonObject{
               {"id", "same-id"},
               {"type", "function"},
               {"function",
                QJsonObject{
                    {"name", "runCommand"},
                    {"arguments", "{\"command\":\"echo fixture\"}"}}}}}}});
      rows.append(QJsonObject{{"role", "tool"},
                              {"tool_call_id", "same-id"},
                              {"content", tag + "_RESULT"}});
    }
    UtilityService service;
    service.setMainModels({{s.provider(), false}});
    QSignalSpy ready(&service, &UtilityService::compressionReady);
    service.compressHistory(rows);
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 3000);
    const auto body = QJsonDocument(s.requests.first()).toJson();
    QVERIFY(body.contains("FIRST_RESULT"));
    QVERIFY(body.contains("SECOND_RESULT"));
    QVERIFY(body.indexOf("FIRST_RESULT") < body.indexOf("SECOND_ASSISTANT"));
    QVERIFY(!s.requests.first().contains("stream_options"));
  }
  void partialAndUnmeteredRoundsCannotUsePreviousCounts() {
    Server s;
    int turn = 0;
    s.respond = [&](bool, int) {
      ++turn;
      if (turn == 1)
        return Reply{streamed("openai", false, 6000)};
      auto full = response("openai", "PARTIAL", false, 9000);
      auto delta =
          full["choices"].toArray()[0].toObject()["message"].toObject();
      const auto text = sseEvent(
          {{"choices", QJsonArray{QJsonObject{
                           {"delta", delta},
                           {"finish_reason",
                            turn == 2 ? QJsonValue() : QJsonValue("stop")}}}}});
      return Reply{text + (turn == 2
                               ? sseEvent({{"choices", QJsonArray{}},
                                           {"usage", usage("openai", 9000)}})
                               : QByteArray("data: [DONE]\n\n"))};
    };
    AgentEngine e;
    e.setProvider(s.provider());
    e.setMessages(history());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("Metered");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QVERIFY(!e.latestUsage().isEmpty());
    e.submit("Interrupted");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 3000);
    QVERIFY(!e.lastTurnSucceeded());
    QVERIFY(e.latestUsage().isEmpty());
    QCOMPARE(e.messages()
                 .last()
                 .toObject()["metadata"]
                 .toObject()["usage"]
                 .toObject()["complete"],
             false);
    e.setProvider(s.provider(10000));
    e.submit("No usage reported");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 3, 3000);
    QVERIFY(e.lastTurnSucceeded());
    QVERIFY(e.latestUsage().isEmpty());
    QCOMPARE(s.requests.size(), 3);
    QCOMPARE(boundary(e.messages()), -1);
  }
  void overloadFallbackDoesNotAttributeOldProviderUsage() {
    Server first, second;
    first.respond = [](bool, int) {
      return Reply{
          QJsonDocument(
              QJsonObject{{"error", QJsonObject{{"message", "overloaded"}}},
                          {"usage", usage("openai", 199999)}})
              .toJson(),
          "application/json", 503};
    };
    second.respond = [](bool, int) {
      return Reply{streamed("openai", false, 100)};
    };
    auto a = first.provider(10000), b = second.provider(10000);
    b.id = "fallback";
    AgentEngine e;
    e.setModelChain({{a, false}, {b, false}}, "preset");
    e.setMessages(history());
    QSignalSpy done(&e, &AgentEngine::finished),
        used(&e, &AgentEngine::usageChanged);
    e.submit("Continue");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 4000);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(used.size(), 1);
    QCOMPARE(e.latestUsage()["providerId"], "fallback");
    QCOMPARE(e.latestUsage()["counters"].toObject()["totalTokens"], 100);
    QCOMPARE(second.requests.size(), 1);
    QCOMPARE(boundary(e.messages()), -1);
  }
  void largeContextUsesHardCapAndConfiguredUtilityFallback() {
    Server s;
    int utilities = 0;
    s.respond = [&](bool utility, int) {
      if (utility)
        return ++utilities == 1 ? s.summary(true) : s.summary();
      return Reply{streamed("openai", false, 100001)};
    };
    auto p = s.provider(1000000), utility = p;
    utility.model = "first-utility";
    AgentEngine e;
    e.setProvider(p);
    e.setCompressionModels({{utility, false}});
    e.setMessages(history());
    QSignalSpy done(&e, &AgentEngine::finished);
    e.submit("Continue");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(e.lastTurnSucceeded());
    QCOMPARE(s.requests.size(), 3);
    QCOMPARE(s.requests[1]["model"], "first-utility");
    QCOMPARE(s.requests[2]["model"], p.model);
    QVERIFY(boundary(e.messages()) > 0);
  }
  void utilityCompletionCanDestroyServiceOutsideNetworkFrame() {
    Server s;
    s.respond = [&](bool, int) { return s.summary(); };
    QPointer<UtilityService> utility = new UtilityService;
    utility->setMainModels({{s.provider(), false}});
    connect(utility, &UtilityService::busyChanged, this, [&](bool busy) {
      if (!busy)
        delete utility.data();
    });
    utility->compressHistory(history());
    QTRY_VERIFY_WITH_TIMEOUT(!utility, 3000);
    QTest::qWait(20);
  }
};
QTEST_GUILESS_MAIN(UsageCompressionTest)
#include "test_usage_compression.moc"
