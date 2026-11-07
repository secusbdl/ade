#include "core/CustomModelOptions.h"
#include "core/ProviderProtocol.h"
#include "core/UtilityService.h"
#include "ui/ModelSelection.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
#include <limits>
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

class ProviderHttpRulesTest : public QObject {
  Q_OBJECT
  QJsonArray captured;
  void capture(const QString &kind, const QJsonObject &body) {
    captured.append(QJsonObject{
        {"case", QString::fromLatin1(QTest::currentTestFunction()) + "/" +
                     QString::fromLatin1(QTest::currentDataTag())},
        {"transport", kind},
        {"body", body}});
  }
private slots:
  void anthropicBudget_data() {
    QTest::addColumn<QString>("model");
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("output");
    QTest::addColumn<int>("budget");
    QTest::addColumn<int>("expectedOutput");
    QTest::addColumn<int>("expectedBudget");
    QTest::newRow("enabled-provider-default-budget")
        << QString("claude-sonnet-4-5") << QString("enabled") << 200 << 0
        << 1224 << 1024;
    QTest::newRow("large-budget-unset-output")
        << QString("claude-sonnet-4-5") << QString("enabled") << 0 << 10000
        << 64000 << 10000;
    QTest::newRow("explicit-output-plus-budget")
        << QString("claude-sonnet-4-5") << QString("enabled") << 100 << 2048
        << 2148 << 2048;
    QTest::newRow("known-output-cap")
        << QString("claude-sonnet-4-5") << QString("enabled") << 63000 << 2048
        << 64000 << 2048;
    QTest::newRow("unknown-model-default")
        << QString("vendor-model") << QString("enabled") << 0 << 0 << 5120
        << 1024;
    QTest::newRow("legacy-haiku-limit")
        << QString("claude-3-haiku-20240307") << QString("disabled") << 32000
        << 0 << 4096 << 0;
    QTest::newRow("dot-model-normalization")
        << QString("claude-sonnet-4.5") << QString("enabled") << 100 << 2048
        << 2148 << 2048;
    QTest::newRow("unknown-explicit-output-not-capped")
        << QString("vendor-model") << QString("disabled") << 32000 << 0 << 32000
        << 0;
    QTest::newRow("adaptive-no-manual-budget")
        << QString("claude-opus-4-6") << QString("adaptive") << 100 << 2048
        << 100 << 0;
  }
  void anthropicBudget() {
    QFETCH(QString, model);
    QFETCH(QString, mode);
    QFETCH(int, output);
    QFETCH(int, budget);
    QFETCH(int, expectedOutput);
    QFETCH(int, expectedBudget);
    auto p = provider("anthropic");
    p.customRequest = {};
    p.model = model;
    p.thinkingMode = mode;
    p.thinkingBudget = budget;
    p.maxOutputTokens = output;
    const auto r = request(p);
    capture("prepare", r.body);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.body.value("max_tokens").toInt(), expectedOutput);
    QCOMPARE(r.body.value("model").toString(),
             QString(model).replace('.', '-'));
    const auto thinking = r.body.value("thinking").toObject();
    if (expectedBudget) {
      QCOMPARE(thinking.value("budget_tokens").toInt(), expectedBudget);
      QVERIFY(expectedOutput > expectedBudget);
    } else
      QVERIFY(!thinking.contains("budget_tokens"));
  }
  void utilityPayload_data() {
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QString>("model");
    QTest::addColumn<QString>("effort");
    QTest::addColumn<QString>("thinking");
    QTest::addColumn<bool>("title");
    QTest::addColumn<bool>("explicitThinking");
    QTest::addColumn<bool>("temperature");
    for (const auto protocol : {"openai", "responses"}) {
      QTest::newRow(qPrintable(QString(protocol) + "-gpt5-title"))
          << QString(protocol) << QString("gpt-5") << QString("high")
          << QString{} << true << false << false;
      QTest::newRow(qPrintable(QString(protocol) + "-o3-compression"))
          << QString(protocol) << QString("o3") << QString{} << QString{}
          << false << false << false;
      QTest::newRow(qPrintable(QString(protocol) + "-gpt51-none"))
          << QString(protocol) << QString("gpt-5.1") << QString("none")
          << QString{} << true << false << true;
      QTest::newRow(qPrintable(QString(protocol) + "-gpt51-high"))
          << QString(protocol) << QString("gpt-5.1") << QString("high")
          << QString{} << true << false << false;
      QTest::newRow(
          qPrintable(QString(protocol) + "-gpt5-none-still-unsupported"))
          << QString(protocol) << QString("gpt-5") << QString("none")
          << QString{} << true << false << false;
      QTest::newRow(qPrintable(QString(protocol) + "-o1"))
          << QString(protocol) << QString("o1") << QString{} << QString{}
          << true << false << false;
      QTest::newRow(qPrintable(QString(protocol) + "-gpt41"))
          << QString(protocol) << QString("gpt-4.1") << QString{} << QString{}
          << true << false << true;
      QTest::newRow(qPrintable(QString(protocol) + "-gpt5-chat"))
          << QString(protocol) << QString("gpt-5-chat-latest") << QString{}
          << QString{} << true << false << true;
    }
    QTest::newRow("anthropic-title-explicit-thinking")
        << QString("anthropic") << QString("claude-sonnet-4-5") << QString{}
        << QString("enabled") << true << true << false;
    QTest::newRow("anthropic-title-default-disables-thinking")
        << QString("anthropic") << QString("claude-sonnet-4-5") << QString{}
        << QString("enabled") << true << false << true;
    QTest::newRow("anthropic-compress-enabled")
        << QString("anthropic") << QString("claude-sonnet-4-5") << QString{}
        << QString("enabled") << false << true << false;
    QTest::newRow("anthropic-compress-adaptive")
        << QString("anthropic") << QString("claude-opus-4-6") << QString{}
        << QString("adaptive") << false << true << false;
    QTest::newRow("anthropic-opus47")
        << QString("anthropic") << QString("claude-opus-4-7") << QString{}
        << QString{} << true << false << false;
    QTest::newRow("openai-search-preview")
        << QString("openai") << QString("gpt-4o-search-preview") << QString{}
        << QString{} << true << false << false;
    QTest::newRow("google-unaffected")
        << QString("google") << QString("gemini-2.5-pro") << QString{}
        << QString{} << false << false << true;
    QTest::newRow("compatible-unknown-not-guessed")
        << QString("openai") << QString("vendor-reasoner") << QString{}
        << QString{} << true << false << true;
  }
  void utilityPayload() {
    QFETCH(QString, protocol);
    QFETCH(QString, model);
    QFETCH(QString, effort);
    QFETCH(QString, thinking);
    QFETCH(bool, title);
    QFETCH(bool, explicitThinking);
    QFETCH(bool, temperature);
    Endpoint endpoint;
    endpoint.replyFor(protocol, "A sufficiently long fixture response suitable "
                                "for compression and title output.");
    auto p = provider(protocol);
    p.customRequest = {};
    p.model = model;
    p.baseUrl = endpoint.base();
    p.reasoningEffort = effort;
    p.thinkingMode = thinking;
    p.thinkingBudget = 2048;
    UtilityService service;
    service.setMainModels({ModelConfig{p, explicitThinking}});
    QSignalSpy titleReady(&service, &UtilityService::titleReady),
        compressionReady(&service, &UtilityService::compressionReady),
        failed(&service, &UtilityService::failed);
    if (title)
      service.generateTitle(history());
    else
      service.compressHistory(history());
    QTRY_VERIFY_WITH_TIMEOUT(!titleReady.isEmpty() ||
                                 !compressionReady.isEmpty() ||
                                 !failed.isEmpty(),
                             3000);
    QVERIFY2(failed.isEmpty(),
             failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
    QCOMPARE(endpoint.bodies.size(), 1);
    const auto b = endpoint.bodies[0];
    capture("loopback", b);
    const auto sampling =
        protocol == "google" ? b.value("generationConfig").toObject() : b;
    QCOMPARE(sampling.contains("temperature"), temperature);
    if (temperature)
      QCOMPARE(sampling.value("temperature").toDouble(), title ? 0.15 : 0.1);
    QVERIFY(!b.value("stream").toBool());
    if (protocol == "anthropic") {
      const auto actual = b.value("thinking").toObject();
      if (thinking == "enabled" && (!title || explicitThinking)) {
        QCOMPARE(actual.value("budget_tokens").toInt(), 2048);
        QCOMPARE(b.value("max_tokens").toInt(), (title ? 100 : 20000) + 2048);
      } else if (title && !explicitThinking)
        QVERIFY(actual.isEmpty() || actual.value("type") == "disabled");
    }
  }
  void discoveredModelDoesNotInheritDefault_data() {
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QString>("original");
    QTest::addColumn<QString>("selected");
    QTest::newRow("openai")
        << QString("openai") << QString("gpt-5") << QString("gpt-4.1");
    QTest::newRow("responses")
        << QString("responses") << QString("gpt-5") << QString("gpt-4.1");
    QTest::newRow("anthropic")
        << QString("anthropic") << QString("claude-sonnet-4-5")
        << QString("claude-3-haiku-20240307");
    QTest::newRow("google") << QString("google") << QString("gemini-2.5-pro")
                            << QString("gemini-2.0-flash");
  }
  void discoveredModelDoesNotInheritDefault() {
    QFETCH(QString, protocol);
    QFETCH(QString, original);
    QFETCH(QString, selected);
    auto p = provider(protocol);
    p.customRequest = {};
    p.model = original;
    p.reasoningEffort = "high";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 8192;
    p.maxOutputTokens = 32000;
    p.contextWindowTokens = 90000;
    auto other = p;
    other.id = "other-instance";
    other.model = selected;
    ProviderConfig result;
    QVERIFY((ModelSelection{p.id, selected, {}, {}, {}}.resolve({other, p},
                                                                &result)));
    const auto r = request(result);
    capture("resolved-prepare", r.body);
    QCOMPARE(result.id, p.id);
    QVERIFY(result.reasoningEffort.isEmpty());
    QVERIFY(result.thinkingMode.isEmpty());
    QCOMPARE(result.thinkingBudget, 0);
    QCOMPARE(result.maxOutputTokens, 0);
    QCOMPARE(result.contextWindowTokens, 0);
    QVERIFY(!r.body.contains("reasoning_effort"));
    QVERIFY(!r.body.contains("reasoning"));
    QVERIFY(!r.body.contains("output_config"));
  }
  void defaultAndExplicitSelectionKeepTheirOwnSettings() {
    auto p = provider("anthropic");
    p.customRequest = {};
    p.model = "claude-sonnet-4-5";
    p.reasoningEffort = "high";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 8192;
    p.maxOutputTokens = 32000;
    p.contextWindowTokens = 90000;
    ProviderConfig result;
    QVERIFY((ModelSelection{p.id, p.model, {}, {}, {}}.resolve({p}, &result)));
    QCOMPARE(result.reasoningEffort, p.reasoningEffort);
    QCOMPARE(result.thinkingMode, p.thinkingMode);
    QCOMPARE(result.thinkingBudget, p.thinkingBudget);
    QCOMPARE(result.maxOutputTokens, p.maxOutputTokens);
    QCOMPARE(result.contextWindowTokens, p.contextWindowTokens);
    const QJsonObject overrides{{"reasoningEffort", "low"},
                                {"thinkingMode", "enabled"},
                                {"thinkingBudget", 2048},
                                {"maxOutputTokens", 500}};
    QVERIFY(
        (ModelSelection{p.id, "claude-haiku-4-5", {}, {}, overrides}.resolve(
            {p}, &result)));
    QCOMPARE(result.requestOverrides, overrides);
    QCOMPARE(result.reasoningEffort, QString("low"));
    QCOMPARE(result.thinkingBudget, 2048);
    QCOMPARE(result.maxOutputTokens, 500);
    QCOMPARE(result.contextWindowTokens, 0);
    const auto r = request(result);
    capture("explicit-resolved-prepare", r.body);
    QCOMPARE(r.body.value("max_tokens").toInt(), 2548);
  }

  void maximumIntegerOutputDoesNotOverflow() {
    auto p = provider("anthropic");
    p.customRequest = {};
    p.model = "unknown-model";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 1024;
    p.maxOutputTokens = std::numeric_limits<int>::max();
    const auto r = request(p);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.body.value("max_tokens").toInteger(),
             qint64(std::numeric_limits<int>::max()) + 1024);
  }
  void externalModelSelectionKeepsHarnessSettings() {
    for (const auto &kind : {"codex", "opencode", "claude"}) {
      ProviderConfig p;
      p.id = "same-instance";
      p.kind = kind;
      p.model = "default";
      p.reasoningEffort = "high";
      p.thinkingMode = "enabled";
      p.thinkingBudget = 2048;
      ProviderConfig selected;
      QVERIFY(
          (ModelSelection{p.id, "other", {}, {}, {}}.resolve({p}, &selected)));
      QCOMPARE(selected.reasoningEffort, p.reasoningEffort);
      QCOMPARE(selected.thinkingMode, p.thinkingMode);
      QCOMPARE(selected.thinkingBudget, p.thinkingBudget);
    }
  }
  void customSelectionRemainsIndependent() {
    auto p = provider("anthropic");
    p.customRequest = {};
    p.model = "default";
    p.reasoningEffort = "high";
    p.thinkingMode = "enabled";
    p.thinkingBudget = 8192;
    p.maxOutputTokens = 32000;
    CustomModelConfig custom;
    custom.providerInstanceId = p.id;
    custom.modelId = "custom-id";
    custom.displayName = "Independent";
    custom.providerOptions = {
        {"thinking", QJsonObject{{"type", "enabled"}, {"budgetTokens", 2048}}}};
    custom.capabilities = defaultCustomModelCapabilities();
    p.customModels = {custom};
    ProviderConfig selected;
    QVERIFY((ModelSelection{p.id, custom.modelId, {}, {}, {}}.resolve(
        {p}, &selected)));
    QVERIFY(selected.reasoningEffort.isEmpty());
    QVERIFY(selected.thinkingMode.isEmpty());
    QCOMPARE(selected.maxOutputTokens, 0);
    QCOMPARE(selected.customRequest.value("providerOptions").toObject(),
             custom.providerOptions);
    const auto r = request(selected);
    QVERIFY(r.error.isEmpty());
    QCOMPARE(r.body.value("thinking").toObject().value("budget_tokens").toInt(),
             2048);
    QCOMPARE(r.body.value("max_tokens").toInt(), 6144);
  }

  void invalidUtilityCandidateFallsThroughWithoutPosting() {
    Endpoint first, second;
    first.replyFor("anthropic");
    second.replyFor("anthropic", "A sufficiently detailed summary to satisfy "
                                 "the compression output requirement.");
    auto bad = provider("anthropic");
    bad.customRequest = {};
    bad.baseUrl = first.base();
    bad.model = "claude-sonnet-4-5";
    bad.thinkingMode = "enabled";
    bad.thinkingBudget = 64000;
    auto good = bad;
    good.id = "second-instance";
    good.baseUrl = second.base();
    good.model = "claude-sonnet-4-6";
    good.thinkingBudget = 2048;
    UtilityService service;
    service.setCompressionModels(
        {ModelConfig{bad, true}, ModelConfig{good, true}});
    QSignalSpy done(&service, &UtilityService::compressionReady),
        failed(&service, &UtilityService::failed),
        attempted(&service, &UtilityService::modelAttempted);
    service.compressHistory(history());
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 3000);
    QVERIFY(failed.isEmpty());
    QVERIFY(first.bodies.isEmpty());
    QCOMPARE(second.bodies.size(), 1);
    QCOMPARE(attempted.size(), 1);
    QCOMPARE(attempted[0][0].toString(), good.id);
    capture("loopback-fallback", second.bodies[0]);
    QCOMPARE(second.bodies[0].value("max_tokens").toInt(), 22048);
    QVERIFY(!second.bodies[0].contains("temperature"));
  }
  void cleanupTestCase() {
    const auto path = qEnvironmentVariable("ADE_PROVIDER_HTTP_CAPTURE");
    if (path.isEmpty())
      return;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(captured).toJson()) > 0);
  }
};
QTEST_GUILESS_MAIN(ProviderHttpRulesTest)
#include "test_provider_http_rules.moc"
