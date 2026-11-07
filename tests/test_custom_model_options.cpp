#include "core/CustomModelOptions.h"
#include "core/ProviderProtocol.h"
#include "core/UtilityService.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(const QString &protocol, QJsonObject options = {},
                        QJsonObject headers = {}) {
  ProviderConfig p{"owner",       "Name",           "http://127.0.0.1:1234/v1",
                   "fixture-key", "private-custom", protocol};
  p.customRequest = {{"providerInstanceId", p.id},
                     {"modelId", p.model},
                     {"providerOptions", options},
                     {"headers", headers},
                     {"capabilities", defaultCustomModelCapabilities()}};
  return p;
}
QJsonArray history() {
  return {
      QJsonObject{{"role", "user"},
                  {"content", "Keep exact content"},
                  {"metadata", QJsonObject{{"display", "UI_ONLY_SENTINEL"},
                                           {"attachments", QJsonArray{}}}}}};
}
QJsonArray tools() {
  return {QJsonObject{
      {"type", "function"},
      {"function",
       QJsonObject{{"name", "fixtureTool"},
                   {"description", "Fixture tool"},
                   {"parameters", QJsonObject{{"type", "object"}}}}}}};
}
ProviderRequest request(const ProviderConfig &p) {
  return ProviderProtocol::prepare(p, history(), "HOST_INSTRUCTIONS", tools());
}
void rows() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p : {"openai", "responses", "anthropic", "google"})
    QTest::newRow(p) << QString(p);
}
class Endpoint : public QTcpServer {
public:
  QList<QJsonObject> bodies;
  QList<QMap<QByteArray, QByteArray>> headers;
  QJsonObject response;
  int status = 200;
  Endpoint() {
    listen(QHostAddress::LocalHost, 0);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (hasPendingConnections()) {
        auto *s = nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        connect(s, &QTcpSocket::readyRead, this, [this, s, bytes] {
          *bytes += s->readAll();
          const auto at = bytes->indexOf("\r\n\r\n");
          if (at < 0)
            return;
          QMap<QByteArray, QByteArray> h;
          for (const auto &line : bytes->left(at).split('\n')) {
            const auto colon = line.indexOf(':');
            if (colon > 0)
              h.insert(line.left(colon).trimmed().toLower(),
                       line.mid(colon + 1).trimmed());
          }
          const auto length = h.value("content-length").toInt();
          if (bytes->size() < at + 4 + length)
            return;
          bodies.append(
              QJsonDocument::fromJson(bytes->mid(at + 4, length)).object());
          headers.append(h);
          s->disconnect(this);
          const auto body =
              QJsonDocument(response).toJson(QJsonDocument::Compact);
          s->write(
              "HTTP/1.1 " + QByteArray::number(status) +
              " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
              QByteArray::number(body.size()) +
              "\r\nConnection: close\r\n\r\n" + body);
          s->disconnectFromHost();
        });
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
      }
    });
  }
  QString base() const {
    return "http://127.0.0.1:" + QString::number(serverPort()) + "/v1";
  }
  void replyFor(const QString &p,
                const QString &text = "A useful fixture title") {
    if (p == "responses")
      response = {
          {"status", "completed"},
          {"output",
           QJsonArray{QJsonObject{
               {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                  {"text", text}}}}}}}};
    else if (p == "anthropic")
      response = {{"stop_reason", "end_turn"},
                  {"content",
                   QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    else if (p == "google")
      response = {
          {"candidates",
           QJsonArray{QJsonObject{
               {"finishReason", "STOP"},
               {"content", QJsonObject{{"parts", QJsonArray{QJsonObject{
                                                     {"text", text}}}}}}}}}};
    else
      response = {
          {"choices", QJsonArray{QJsonObject{
                          {"finish_reason", "stop"},
                          {"message", QJsonObject{{"content", text}}}}}}};
  }
};
} // namespace
class CustomOptionsTest : public QObject {
  Q_OBJECT
private slots:
  void privateMetadataKeysAreRedacted() {
    auto p = provider(
        "openai",
        {{"metadata", QJsonObject{{"PRIVATE_METADATA_KEY_123", "value"}}}});
    QCOMPARE(CustomModelOptions::redactDiagnostic(
                 p, "Echo PRIVATE_METADATA_KEY_123"),
             QString("Echo [REDACTED]"));
  }

  void unknownPrivatePropertyNameIsNotDiagnostic() {
    QString error;
    QVERIFY(!CustomModelOptions::validate(
        "openai", {{"SYNTHETIC_PRIVATE_HEADER_123456", true}}, {}, &error));
    QVERIFY(!error.contains("SYNTHETIC_PRIVATE_HEADER_123456"));
    QVERIFY(error.contains("providerOptions"));
  }

  void appliesCustomFields_data() { rows(); }
  void appliesCustomFields() {
    QFETCH(QString, protocol);
    QJsonObject o;
    QString field;
    if (protocol == "openai" || protocol == "responses") {
      o = {{"user", "fixture-user"}};
      field = "user";
    } else if (protocol == "anthropic") {
      o = {{"metadata", QJsonObject{{"userId", "fixture-user"}}}};
      field = "metadata";
    } else {
      o = {{"serviceTier", "standard"}};
      field = "serviceTier";
    }
    auto p = provider(protocol, o, {{"X-Fixture", "header-value"}});
    const auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QVERIFY2(r.body.contains(field),
             qPrintable(protocol + " custom option missing"));
    QCOMPARE(r.request.rawHeader("X-Fixture"), QByteArray("header-value"));
  }
  void unchangedOrdinaryRequests_data() { rows(); }
  void unchangedOrdinaryRequests() {
    QFETCH(QString, protocol);
    auto p = provider(protocol);
    p.customRequest = {};
    p.reasoningEffort = "high";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 2048;
    p.maxOutputTokens = 1000;
    const auto before = request(p);
    QJsonObject body = before.body;
    auto q = before.request;
    QString error;
    QVERIFY(CustomModelOptions::apply(p, &body, &q, &error));
    QCOMPARE(body, before.body);
    QCOMPARE(q, before.request);
  }
  void historyAndPrivateConfigurationNeverMix_data() { rows(); }
  void historyAndPrivateConfigurationNeverMix() {
    QFETCH(QString, protocol);
    auto p = provider(protocol, {}, {{"X-Private-Token", "SECRET_HEADER"}});
    const auto custom = request(p);
    p.customRequest = {};
    const auto ordinary = request(p);
    QVERIFY2(custom.error.isEmpty(), qPrintable(custom.error));
    for (const auto &key : {"messages", "input", "contents", "instructions",
                            "systemInstruction", "system"})
      QCOMPARE(custom.body.value(key), ordinary.body.value(key));
    const auto bytes = QJsonDocument(custom.body).toJson();
    QVERIFY(!bytes.contains("SECRET_HEADER"));
    QVERIFY(!bytes.contains("UI_ONLY_SENTINEL"));
    QVERIFY(!bytes.contains("customRequest"));
    QVERIFY(!bytes.contains("requestOverrides"));
    QVERIFY(!bytes.contains("contextWindow"));
  }
  void unsupportedFields_data() {
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QString>("field");
    for (const auto &p : {"openai", "responses", "anthropic", "google"})
      for (const auto &f :
           {"model", "messages", "input", "tools", "tool_choice", "stream",
            "temperature", "maxOutputTokens", "baseURL", "openai"})
        QTest::newRow(qPrintable(QString(p) + "-" + f))
            << QString(p) << QString(f);
    for (const auto &f :
         {"instructions", "previousResponseId", "conversation", "allowedTools",
          "passThroughUnsupportedFiles", "systemMessageMode"})
      QTest::newRow(qPrintable("responses-" + QString(f)))
          << QString("responses") << QString(f);
    for (const auto &f : {"mcpServers", "container", "contextManagement",
                          "sendReasoning", "structuredOutputMode"})
      QTest::newRow(qPrintable("anthropic-" + QString(f)))
          << QString("anthropic") << QString(f);
    for (const auto &f : {"cachedContent", "imageConfig", "audioTimestamp",
                          "retrievalConfig", "labels", "threshold",
                          "streamFunctionCallArguments", "structuredOutputs"})
      QTest::newRow(qPrintable("google-" + QString(f)))
          << QString("google") << QString(f);
  }
  void unsupportedFields() {
    QFETCH(QString, protocol);
    QFETCH(QString, field);
    QString error;
    QVERIFY(!CustomModelOptions::validate(protocol, {{field, "SECRET_OPTION"}},
                                          {}, &error));
    QVERIFY(error.contains("unsupported"));
    QVERIFY(!error.contains("SECRET_OPTION"));
  }
  void headersAreValidated_data() {
    QTest::addColumn<QJsonObject>("headers");
    QTest::newRow("case-alias")
        << QJsonObject{{"X-Key", "one"}, {"x-key", "two"}};
    QTest::newRow("newline") << QJsonObject{{"X-Key", "one\r\nHost:evil"}};
    QTest::newRow("nul") << QJsonObject{{"X-Key", QString("a") + QChar(0)}};
    QTest::newRow("nonlatin") << QJsonObject{{"X-Key", QString::fromUtf8("⚠")}};
    for (const auto &h : {"Host", "Content-Length", "Transfer-Encoding",
                          "Connection", "Proxy-Authorization", "TE", "Upgrade"})
      QTest::newRow(h) << QJsonObject{{h, "bad"}};
    QTest::newRow("wrong-json") << QJsonObject{{"content-type", "text/plain"}};
    QTest::newRow("nonstring") << QJsonObject{{"x-key", 17}};
  }
  void headersAreValidated() {
    QFETCH(QJsonObject, headers);
    auto p = provider("openai", {}, headers);
    QJsonObject body{{"sentinel", true}};
    QNetworkRequest q{QUrl("http://127.0.0.1/original")};
    q.setRawHeader("Authorization", "old");
    const auto before = q;
    QString error;
    QVERIFY(!CustomModelOptions::apply(p, &body, &q, &error));
    QCOMPARE(body, QJsonObject({{"sentinel", true}}));
    QCOMPARE(q, before);
    QVERIFY(!error.isEmpty());
  }
  void customAuthenticationOverridesDefault_data() { rows(); }
  void customAuthenticationOverridesDefault() {
    QFETCH(QString, protocol);
    const auto name = protocol == "anthropic" ? "X-Api-Key"
                      : protocol == "google"  ? "X-Goog-Api-Key"
                                              : "aUtHoRiZaTiOn";
    const auto p = provider(protocol, {}, {{name, "custom-secret"}});
    const auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.request.rawHeader(name), QByteArray("custom-secret"));
    QCOMPARE(
        r.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
        int(QNetworkRequest::SameOriginRedirectPolicy));
  }
  void exactBindingAndCapabilities_data() {
    QTest::addColumn<QString>("field");
    for (const auto &f : {"owner", "model", "audio-input", "image-output",
                          "no-text", "external"})
      QTest::newRow(f) << QString(f);
  }
  void exactBindingAndCapabilities() {
    QFETCH(QString, field);
    auto p = provider("openai");
    if (field == "owner")
      p.customRequest["providerInstanceId"] = "other";
    else if (field == "model")
      p.customRequest["modelId"] = "other";
    else if (field == "external")
      p.kind = "codex";
    else {
      auto c = p.customRequest.value("capabilities").toObject();
      const auto section =
          field == "image-output" ? "outputModalities" : "inputModalities";
      auto m = c.value(section).toObject();
      m[field == "audio-input"    ? "audio"
        : field == "image-output" ? "image"
                                  : "text"] = field != "no-text";
      c[section] = m;
      p.customRequest["capabilities"] = c;
    }
    const auto r = request(p);
    QVERIFY(!r.error.isEmpty());
    QVERIFY(r.body.isEmpty());
    QVERIFY(!CustomModelOptions::allowsTools(p));
  }
  void declaredNoTools_data() { rows(); }
  void declaredNoTools() {
    QFETCH(QString, protocol);
    auto p = provider(protocol);
    auto c = p.customRequest.value("capabilities").toObject();
    c["toolCalling"] = false;
    p.customRequest["capabilities"] = c;
    const auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QVERIFY(!r.body.contains("tools"));
    QVERIFY(!r.body.contains("tool_choice"));
    QVERIFY(!CustomModelOptions::allowsTools(p));
  }
  void openAiNormalization() {
    auto p = provider("openai",
                      {{"forceReasoning", true},
                       {"reasoningEffort", "high"},
                       {"textVerbosity", "low"},
                       {"maxCompletionTokens", 900},
                       {"promptCacheOptions",
                        QJsonObject{{"mode", "explicit"}, {"ttl", "30m"}}}});
    p.maxOutputTokens = 100;
    p.requestOverrides = {{"temperature", 0.15}};
    auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.body.value("max_completion_tokens").toInt(), 900);
    QVERIFY(!r.body.contains("max_tokens"));
    QVERIFY(!r.body.contains("temperature"));
    QCOMPARE(r.body.value("verbosity").toString(), QString("low"));
    QCOMPARE(r.body.value("messages")
                 .toArray()[0]
                 .toObject()
                 .value("role")
                 .toString(),
             QString("developer"));
    QCOMPARE(
        r.body.value("prompt_cache_options").toObject().value("ttl").toString(),
        QString("30m"));
    p.requestOverrides["maxOutputTokens"] = 60;
    r = request(p);
    QCOMPARE(r.body.value("max_completion_tokens").toInt(), 60);
  }
  void sourceClassifierDoesNotGuessNewModels() {
    auto p = provider("responses", {{"reasoningEffort", "high"}});
    p.model = "gpt-6-fixture";
    p.customRequest["modelId"] = p.model;
    QVERIFY(!request(p).error.isEmpty());
    auto o = p.customRequest.value("providerOptions").toObject();
    o["forceReasoning"] = true;
    p.customRequest["providerOptions"] = o;
    QVERIFY(request(p).error.isEmpty());
  }
  void responsesNestedReasoningAndEncryptedReplay() {
    auto p = provider("responses", {{"forceReasoning", true},
                                    {"reasoningEffort", "high"},
                                    {"reasoningSummary", "auto"},
                                    {"reasoningMode", "standard"},
                                    {"reasoningContext", "all_turns"},
                                    {"textVerbosity", "low"},
                                    {"store", false}});
    const auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.body.value("reasoning").toObject().value("context").toString(),
             QString("all_turns"));
    QCOMPARE(r.body.value("text").toObject().value("verbosity").toString(),
             QString("low"));
    QVERIFY(r.body.value("include").toArray().contains(
        "reasoning.encrypted_content"));
    QVERIFY(!r.body.contains("reasoningEffort"));
  }
  void anthropicBudgetAndBetas() {
    auto p = provider("anthropic",
                      {{"thinking", QJsonObject{{"type", "enabled"}}},
                       {"anthropicBeta", QJsonArray{"Beta-One", "beta-two"}},
                       {"speed", "fast"},
                       {"disableParallelToolUse", true}},
                      {{"Anthropic-Beta", "beta-one, BETA-THREE"}});
    p.maxOutputTokens = 100;
    p.requestOverrides = {{"temperature", 0.15}};
    auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.body.value("max_tokens").toInt(), 1124);
    QCOMPARE(r.body.value("thinking").toObject().value("budget_tokens").toInt(),
             1024);
    QVERIFY(!r.body.contains("temperature"));
    QCOMPARE(r.body.value("tool_choice")
                 .toObject()
                 .value("disable_parallel_tool_use"),
             QJsonValue(true));
    QVERIFY(r.body.value("tools")
                .toArray()[0]
                .toObject()
                .value("eager_input_streaming")
                .toBool());
    QCOMPARE(r.request.rawHeader("anthropic-beta"),
             QByteArray(
                 "beta-one,beta-three,Beta-One,beta-two,fast-mode-2026-02-01"));
    p.requestOverrides["thinkingMode"] = "disabled";
    r = request(p);
    QVERIFY(!r.body.contains("thinking"));
    QCOMPARE(r.body.value("max_tokens").toInt(), 100);
    QCOMPARE(r.body.value("temperature").toDouble(), 0.15);
  }
  void anthropicWireModelAndPinnedCap() {
    auto p = provider("anthropic",
                      {{"thinking", QJsonObject{{"type", "enabled"},
                                                {"budgetTokens", 2048}}}});
    p.model = "custom-claude-opus-4.7";
    p.customRequest["modelId"] = p.model;
    p.maxOutputTokens = 128000;
    const auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.body.value("model").toString(),
             QString("custom-claude-opus-4-7"));
    QCOMPARE(r.body.value("max_tokens").toInt(), 128000);
  }
  void googleNestedFields() {
    auto p = provider(
        "google",
        {{"thinkingConfig",
          QJsonObject{{"thinkingBudget", 1024}, {"includeThoughts", true}}},
         {"mediaResolution", "MEDIA_RESOLUTION_HIGH"},
         {"responseModalities", QJsonArray{"TEXT"}},
         {"safetySettings",
          QJsonArray{QJsonObject{{"category", "HARM_CATEGORY_HATE_SPEECH"},
                                 {"threshold", "BLOCK_ONLY_HIGH"}}}}});
    p.requestOverrides = {{"thinkingMode", "disabled"},
                          {"maxOutputTokens", 100}};
    const auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    const auto g = r.body.value("generationConfig").toObject();
    QCOMPARE(
        g.value("thinkingConfig").toObject().value("thinkingBudget").toInt(),
        0);
    QCOMPARE(g.value("maxOutputTokens").toInt(), 100);
    QVERIFY(r.body.value("safetySettings").isArray());
  }
  void diagnosticRedactionDoesNotChangeSuccessfulPayload() {
    auto p =
        provider("openai",
                 {{"user", "private-user"},
                  {"metadata", QJsonObject{{"private", "secret-metadata"}}}},
                 {{"X-Key", "secret-header"}});
    QCOMPARE(CustomModelOptions::redactDiagnostic(
                 p, "private-user secret-metadata secret-header fixture-key"),
             QString("[REDACTED] [REDACTED] [REDACTED] [REDACTED]"));
    const auto r = ProviderProtocol::prepare(
        p,
        {QJsonObject{{"role", "tool"},
                     {"tool_call_id", "id"},
                     {"content", "secret-header private-user"}}},
        "Host", {});
    QVERIFY(
        QJsonDocument(r.body).toJson().contains("secret-header private-user"));
  }
  void actualUtilityWire_data() { rows(); }
  void actualUtilityWire() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.replyFor(protocol);
    auto p = provider(protocol, {}, {{"X-Fixture", "secret-header"}});
    p.baseUrl = endpoint.base();
    if (protocol == "anthropic")
      p.customRequest["providerOptions"] =
          QJsonObject{{"thinking", QJsonObject{{"type", "enabled"},
                                               {"budgetTokens", 5000}}}};
    if (protocol == "openai")
      p.customRequest["providerOptions"] =
          QJsonObject{{"maxCompletionTokens", 19000}};
    UtilityService service;
    service.setMainModels({ModelConfig{p}});
    QSignalSpy done(&service, &UtilityService::titleReady);
    QSignalSpy failed(&service, &UtilityService::failed);
    service.generateTitle(history());
    QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 3000);
    QVERIFY2(failed.isEmpty(),
             failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
    QCOMPARE(endpoint.bodies.size(), 1);
    QCOMPARE(endpoint.headers[0].value("x-fixture"),
             QByteArray("secret-header"));
    const auto b = endpoint.bodies[0];
    if (protocol == "anthropic") {
      QVERIFY(!b.contains("thinking"));
      QCOMPARE(b.value("max_tokens").toInt(), 100);
    }
    if (protocol == "openai")
      QCOMPARE(b.value("max_completion_tokens").toInt(), 100);
    if (protocol == "responses")
      QCOMPARE(b.value("max_output_tokens").toInt(), 100);
    if (protocol == "google")
      QCOMPARE(b.value("generationConfig")
                   .toObject()
                   .value("maxOutputTokens")
                   .toInt(),
               100);
    QVERIFY(!QJsonDocument(b).toJson().contains("secret-header"));
    QVERIFY(!b.value("stream").toBool());
  }
  void explicitUtilityThinkingOverrideIsPreserved() {
    Endpoint endpoint;
    endpoint.replyFor("anthropic");
    auto p = provider("anthropic",
                      {{"thinking", QJsonObject{{"type", "enabled"},
                                                {"budgetTokens", 2048}}}});
    p.baseUrl = endpoint.base();
    p.requestOverrides = {{"thinkingMode", "enabled"},
                          {"thinkingBudget", 3072}};
    UtilityService service;
    service.setTitleModels({ModelConfig{p, true}});
    QSignalSpy done(&service, &UtilityService::titleReady);
    service.generateTitle(history());
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QCOMPARE(endpoint.bodies[0]
                 .value("thinking")
                 .toObject()
                 .value("budget_tokens")
                 .toInt(),
             3072);
    QCOMPARE(endpoint.bodies[0].value("max_tokens").toInt(), 3172);
    QVERIFY(!endpoint.bodies[0].contains("temperature"));
  }

  void legacyZeroOutputIsUnset_data() { rows(); }
  void legacyZeroOutputIsUnset() {
    QFETCH(QString, protocol);
    auto p = provider(protocol);
    p.maxOutputTokens = 900;
    p.requestOverrides = {{"maxOutputTokens", 0}};
    if (protocol == "openai")
      p.customRequest["providerOptions"] =
          QJsonObject{{"maxCompletionTokens", 800}};
    auto r = request(p);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    if (protocol == "openai") {
      QVERIFY(!r.body.contains("max_tokens"));
      QVERIFY(!r.body.contains("max_completion_tokens"));
    } else if (protocol == "responses")
      QVERIFY(!r.body.contains("max_output_tokens"));
    else if (protocol == "google")
      QVERIFY(!r.body.value("generationConfig")
                   .toObject()
                   .contains("maxOutputTokens"));
    else
      QCOMPARE(r.body.value("max_tokens").toInt(), 4096);
    p.requestOverrides["maxOutputTokens"] = 100;
    r = request(p);
    QVERIFY(r.error.isEmpty());
    if (protocol == "google")
      QCOMPARE(r.body.value("generationConfig")
                   .toObject()
                   .value("maxOutputTokens")
                   .toInt(),
               100);
    else
      QCOMPARE(r.body
                   .value(protocol == "responses" ? "max_output_tokens"
                          : protocol == "openai"  ? "max_completion_tokens"
                                                  : "max_tokens")
                   .toInt(),
               100);
  }
  void explicitImageCapability_data() { rows(); }
  void explicitImageCapability() {
    QFETCH(QString, protocol);
    auto p = provider(protocol);
    auto caps = p.customRequest.value("capabilities").toObject();
    auto input = caps.value("inputModalities").toObject();
    input["image"] = false;
    caps["inputModalities"] = input;
    p.customRequest["capabilities"] = caps;
    const QJsonArray h{QJsonObject{
        {"role", "user"},
        {"content", QJsonArray{QJsonObject{{"type", "image"},
                                           {"mediaType", "image/png"},
                                           {"dataBase64", "YQ=="}}}}}};
    const auto denied = ProviderProtocol::prepare(p, h, "Host", {});
    QVERIFY(!denied.error.isEmpty());
    QVERIFY(denied.body.isEmpty());
    input["image"] = true;
    caps["inputModalities"] = input;
    p.customRequest["capabilities"] = caps;
    const auto allowed = ProviderProtocol::prepare(p, h, "Host", {});
    QVERIFY2(allowed.error.isEmpty(), qPrintable(allowed.error));
    QVERIFY(QJsonDocument(allowed.body).toJson().contains("YQ=="));
    p.customRequest = {};
    QVERIFY(ProviderProtocol::prepare(p, h, "Host", {}).error.isEmpty());
  }
  void explicitThinkingOverridesAreTyped() {
    for (const auto &protocol : {"openai", "responses"})
      for (const auto &mode : {"enabled", "adaptive"}) {
        auto p = provider(protocol);
        p.requestOverrides = {{"thinkingMode", mode}};
        QVERIFY(!request(p).error.isEmpty());
      }
    auto p = provider(
        "google", {{"thinkingConfig", QJsonObject{{"thinkingBudget", 2048},
                                                  {"includeThoughts", true}}}});
    p.requestOverrides = {{"thinkingMode", "enabled"}};
    auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.body.value("generationConfig")
                 .toObject()
                 .value("thinkingConfig")
                 .toObject()
                 .value("thinkingBudget")
                 .toInt(),
             2048);
    p.requestOverrides["thinkingMode"] = "adaptive";
    QVERIFY(!request(p).error.isEmpty());
  }
  void sourceForceReasoningDoesNotResetSystemRole() {
    auto p = provider("openai", {{"forceReasoning", false}});
    p.model = "gpt-5.4-fixture";
    p.customRequest["modelId"] = p.model;
    p.requestOverrides = {{"temperature", 0.5}};
    const auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.body.value("messages")
                 .toArray()[0]
                 .toObject()
                 .value("role")
                 .toString(),
             QString("developer"));
    QCOMPARE(r.body.value("temperature").toDouble(), 0.5);
  }
  void actualCompressionLimit_data() { rows(); }
  void actualCompressionLimit() {
    QFETCH(QString, protocol);
    Endpoint endpoint;
    endpoint.replyFor(
        protocol,
        "A detailed fixture briefing preserving the current task and context.");
    auto p = provider(protocol, {}, {{"X-Fixture", "compression-secret"}});
    p.baseUrl = endpoint.base();
    p.requestOverrides = {{"maxOutputTokens", 0}};
    if (protocol == "openai")
      p.customRequest["providerOptions"] =
          QJsonObject{{"maxCompletionTokens", 45000}};
    UtilityService service;
    service.setCompressionModels({ModelConfig{p}});
    QSignalSpy done(&service, &UtilityService::compressionReady),
        failed(&service, &UtilityService::failed);
    service.compressHistory(history());
    QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty() || !failed.isEmpty(), 3000);
    QVERIFY2(failed.isEmpty(),
             failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
    QCOMPARE(endpoint.bodies.size(), 1);
    const auto b = endpoint.bodies[0];
    if (protocol == "google")
      QCOMPARE(b.value("generationConfig")
                   .toObject()
                   .value("maxOutputTokens")
                   .toInt(),
               20000);
    else
      QCOMPARE(b.value(protocol == "responses" ? "max_output_tokens"
                       : protocol == "openai"  ? "max_completion_tokens"
                                               : "max_tokens")
                   .toInt(),
               20000);
    QCOMPARE(endpoint.headers[0].value("x-fixture"),
             QByteArray("compression-secret"));
  }
  void supportedWireFields_data() {
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QJsonObject>("options");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QJsonValue>("expected");
    auto add = [](const char *p, const char *option, const QJsonValue &value,
                  const char *destination) {
      QTest::newRow(qPrintable(QString(p) + "-" + option))
          << QString(p) << QJsonObject{{option, value}} << QString(destination)
          << value;
    };
    for (const auto &p : {"openai", "responses"}) {
      add(p, "parallelToolCalls", false, "parallel_tool_calls");
      add(p, "user", "caller", "user");
      add(p, "store", true, "store");
      add(p, "metadata", QJsonObject{{"label", "value"}}, "metadata");
      add(p, "promptCacheKey", "cache", "prompt_cache_key");
      add(p, "promptCacheRetention", "24h", "prompt_cache_retention");
      add(p, "promptCacheOptions",
          QJsonObject{{"mode", "explicit"}, {"ttl", "30m"}},
          "prompt_cache_options");
      add(p, "safetyIdentifier", "fixture", "safety_identifier");
      add(p, "serviceTier", "default", "service_tier");
    }
    add("openai", "prediction",
        QJsonObject{{"type", "content"}, {"content", "predicted text"}},
        "prediction");
    add("responses", "truncation", "disabled", "truncation");
    add("responses", "maxToolCalls", 3, "max_tool_calls");
    add("anthropic", "effort", "high", "output_config.effort");
    add("anthropic", "cacheControl",
        QJsonObject{{"type", "ephemeral"}, {"ttl", "1h"}}, "cache_control");
    add("anthropic", "inferenceGeo", "us", "inference_geo");
    add("anthropic", "speed", "standard", "speed");
    add("google", "serviceTier", "flex", "serviceTier");
    add("google", "mediaResolution", "MEDIA_RESOLUTION_LOW",
        "generationConfig.mediaResolution");
    add("google", "responseModalities", QJsonArray{"TEXT"},
        "generationConfig.responseModalities");
  }
  void supportedWireFields() {
    QFETCH(QString, protocol);
    QFETCH(QJsonObject, options);
    QFETCH(QString, path);
    QFETCH(QJsonValue, expected);
    const auto r = request(provider(protocol, options));
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QJsonValue value = r.body;
    for (const auto &key : path.split('.'))
      value = value.toObject().value(key);
    QCOMPARE(value, expected);
  }
  void malformedNestedOptionsAreRejected() {
    for (const auto &pair : QList<QPair<QString, QJsonObject>>{
             {"openai", {{"metadata", QJsonObject{{"label", 7}}}}},
             {"responses",
              {{"include", QJsonArray{"web_search_call.action.sources"}}}},
             {"anthropic",
              {{"thinking",
                QJsonObject{{"type", "enabled"}, {"budgetTokens", 512}}}}},
             {"google",
              {{"thinkingConfig", QJsonObject{{"thinkingLevel", "invented"}}}}},
             {"google", {{"responseModalities", QJsonArray{"IMAGE"}}}}}) {
      QString error;
      QVERIFY(
          !CustomModelOptions::validate(pair.first, pair.second, {}, &error));
      QVERIFY(!error.isEmpty());
    }
  }
  void invalidUtilityNeverDispatches() {
    Endpoint endpoint;
    auto p =
        provider("responses", {{"previousResponseId", "private-remote-id"}});
    p.baseUrl = endpoint.base();
    UtilityService service;
    service.setTitleModels({ModelConfig{p}});
    QSignalSpy failed(&service, &UtilityService::failed);
    service.generateTitle(history());
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 3000);
    QVERIFY(endpoint.bodies.isEmpty());
    QVERIFY(!failed[0][0].toString().contains("private-remote-id"));
  }
};
QTEST_GUILESS_MAIN(CustomOptionsTest)
#include "test_custom_model_options.moc"
