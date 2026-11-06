#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
class Server : public QObject {
public:
  QTcpServer server;
  QList<QJsonObject> requests;
  QList<QPointer<QTcpSocket>> sockets;
  Server() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          if (socket->property("received").toBool())
            return;
          const auto end = bytes.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(end).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < end + 4 + length)
            return;
          socket->setProperty("received", true);
          requests.append(
              QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
          sockets.append(socket);
        });
      }
    });
  }
  void respond(int index, const QByteArray &body) {
    auto socket = sockets.value(index);
    if (!socket)
      return;
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                  "text/event-stream\r\nContent-Length: " +
                  QByteArray::number(body.size()) +
                  "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
  }
  ProviderConfig provider(const QString &protocol) const {
    return {"fixture",
            "Browser context fixture",
            QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
            "synthetic-local-only",
            "fixture-model",
            protocol};
  }
};
QString instructions(const QJsonObject &body, const QString &protocol) {
  if (protocol == "responses")
    return body["instructions"].toString();
  if (protocol == "anthropic")
    return body["system"].toString();
  if (protocol == "google")
    return body["systemInstruction"]
        .toObject()["parts"]
        .toArray()
        .first()
        .toObject()["text"]
        .toString();
  return body["messages"].toArray().first().toObject()["content"].toString();
}
QByteArray event(const QJsonObject &object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QByteArray reply(const QString &protocol, bool tool) {
  const QString name = "getBrowserTabs", id = "browser-discovery";
  if (protocol == "openai") {
    QJsonObject delta{{"content", "Done"}};
    if (tool)
      delta = {{"tool_calls",
                QJsonArray{QJsonObject{
                    {"index", 0},
                    {"id", id},
                    {"type", "function"},
                    {"function",
                     QJsonObject{{"name", name}, {"arguments", "{}"}}}}}}};
    return event({{"choices",
                   QJsonArray{QJsonObject{
                       {"delta", delta},
                       {"finish_reason", tool ? "tool_calls" : "stop"}}}}}) +
           "data: [DONE]\n\n";
  }
  if (protocol == "anthropic") {
    const QJsonObject block =
        tool ? QJsonObject{{"type", "tool_use"},
                           {"id", id},
                           {"name", name},
                           {"input", QJsonObject{}}}
             : QJsonObject{{"type", "text"}, {"text", "Done"}};
    return event({{"type", "content_block_start"},
                  {"index", 0},
                  {"content_block", block}}) +
           event({{"type", "content_block_stop"}, {"index", 0}}) +
           event({{"type", "message_delta"},
                  {"delta", QJsonObject{{"stop_reason",
                                         tool ? "tool_use" : "end_turn"}}}}) +
           event({{"type", "message_stop"}});
  }
  if (protocol == "google") {
    const QJsonObject part =
        tool ? QJsonObject{{"functionCall",
                            QJsonObject{{"id", id},
                                        {"name", name},
                                        {"args", QJsonObject{}}}}}
             : QJsonObject{{"text", "Done"}};
    return event({{"candidates",
                   QJsonArray{QJsonObject{
                       {"content", QJsonObject{{"role", "model"},
                                               {"parts", QJsonArray{part}}}},
                       {"finishReason", "STOP"}}}}});
  }
  const QJsonObject item =
      tool ? QJsonObject{{"id", "item1"},
                         {"type", "function_call"},
                         {"call_id", id},
                         {"name", name},
                         {"arguments", "{}"}}
           : QJsonObject{
                 {"id", "item1"},
                 {"type", "message"},
                 {"role", "assistant"},
                 {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                    {"text", "Done"}}}}};
  return event({{"type", "response.output_item.done"},
                {"output_index", 0},
                {"item", item}}) +
         event({{"type", "response.completed"},
                {"response", QJsonObject{{"status", "completed"},
                                         {"output", QJsonArray{item}}}}});
}
void protocols() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p : {"openai", "responses", "anthropic", "google"})
    QTest::newRow(p) << QString(p);
}
} // namespace
class BrowserContextRuntimeTest : public QObject {
  Q_OBJECT
private slots:
  void snapshotReachesActualHostWire_data() { protocols(); }
  void snapshotReachesActualHostWire() {
    QFETCH(QString, protocol);
    QTemporaryDir data;
    Server server;
    BrowserToolBridge bridge;
    BrowserWidget shared, own, other;
    own.setProperty("adeOwnerAgentId", "owner");
    other.setProperty("adeOwnerAgentId", "foreign");
    QVERIFY(!bridge.registerTab(&shared, "public-tab").isEmpty());
    QVERIFY(!bridge.registerTab(&own, "owned-tab").isEmpty());
    QVERIFY(!bridge.registerTab(&other, "foreign-tab").isEmpty());
    emit shared.titleChanged("Public fixture");
    emit own.titleChanged("Owned fixture");
    emit other.titleChanged("Foreign private title");
    QVERIFY(bridge.setActiveTab("foreign-tab"));
    AgentEngine engine;
    engine.setProvider(server.provider(protocol));
    AgentToolHost host(&engine, &bridge, "owner", data.path());
    engine.submit("Describe the current browser context.");
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 5000);
    const auto prompt = instructions(server.requests.first(), protocol);
    engine.cancel();
    QVERIFY2(prompt.contains("<open-tabs>"), qPrintable(prompt));
    QVERIFY(prompt.contains("id=\"public-tab\""));
    QVERIFY(prompt.contains("id=\"owned-tab\""));
    QVERIFY(prompt.contains("Owned fixture"));
    QVERIFY(!prompt.contains("foreign-tab"));
    QVERIFY(!prompt.contains("Foreign private title"));
    QVERIFY(!prompt.contains("active=\"true\""));
    QVERIFY(prompt.contains("<shell-sessions>"));
    QVERIFY(prompt.contains("[Tab title](tab:ID)"));
    QVERIFY(!prompt.contains("[](tab:"));
  }
  void nextModelStepSeesFreshState_data() { protocols(); }
  void nextModelStepSeesFreshState() {
    QFETCH(QString, protocol);
    QTemporaryDir data;
    Server server;
    BrowserToolBridge bridge;
    BrowserWidget first, next;
    bridge.registerTab(&first, "first");
    bridge.registerTab(&next, "next");
    emit first.titleChanged("Old title");
    emit next.titleChanged("Before navigation");
    emit next.urlChanged(QUrl("https://example.test/before"));
    AgentEngine engine;
    engine.setProvider(server.provider(protocol));
    AgentToolHost host(&engine, &bridge, "owner", data.path());
    engine.submit("Inspect current tabs.");
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 1, 5000);
    QVERIFY(instructions(server.requests[0], protocol).contains("Old title"));
    bridge.unregisterTab("first");
    emit next.titleChanged("After navigation");
    emit next.urlChanged(QUrl("https://example.test/after"));
    bridge.setActiveTab("next");
    server.respond(0, reply(protocol, true));
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 2, 5000);
    const auto prompt = instructions(server.requests[1], protocol);
    QVERIFY(prompt.contains("After navigation"));
    QVERIFY(prompt.contains("https://example.test/after"));
    QVERIFY(prompt.contains("active=\"true\""));
    QVERIFY(!prompt.contains("Old title"));
    QVERIFY(!prompt.contains("id=\"first\""));
    int users = 0, tools = 0;
    for (const auto &value : engine.messages()) {
      const auto row = value.toObject();
      users += row["role"] == "user";
      tools += row["role"] == "tool";
      QVERIFY(!row["content"].toString().contains("<open-tabs>"));
    }
    QCOMPARE(users, 1);
    QCOMPARE(tools, 1);
    server.respond(1, reply(protocol, false));
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
  }
  void sharedBridgeDoesNotCrossAgentPayloads() {
    QTemporaryDir data;
    BrowserToolBridge bridge;
    BrowserWidget a, b, global;
    a.setProperty("adeOwnerAgentId", "a");
    b.setProperty("adeOwnerAgentId", "b");
    bridge.registerTab(&a, "a-only");
    bridge.registerTab(&b, "b-only");
    bridge.registerTab(&global, "shared");
    emit a.titleChanged("SECRET_FIXTURE_A");
    emit b.titleChanged("SECRET_FIXTURE_B");
    Server sa, sb;
    AgentEngine ea, eb;
    ea.setProvider(sa.provider("openai"));
    eb.setProvider(sb.provider("openai"));
    AgentToolHost ha(&ea, &bridge, "a", data.path()),
        hb(&eb, &bridge, "b", data.path());
    ea.submit("A");
    eb.submit("B");
    QTRY_COMPARE(sa.requests.size(), 1);
    QTRY_COMPARE(sb.requests.size(), 1);
    ea.cancel();
    eb.cancel();
    const auto pa = instructions(sa.requests[0], "openai"),
               pb = instructions(sb.requests[0], "openai");
    QVERIFY(pa.contains("a-only") && !pa.contains("b-only"));
    QVERIFY(pb.contains("b-only") && !pb.contains("a-only"));
    QVERIFY(!pa.contains("SECRET_FIXTURE_B"));
    QVERIFY(!pb.contains("SECRET_FIXTURE_A"));
    QVERIFY(pa.contains("shared") && pb.contains("shared"));
  }
  void deadBridgePreservesShellAndDeadHostIsInert() {
    QTemporaryDir data;
    Server server;
    auto bridge = std::make_unique<BrowserToolBridge>();
    BrowserWidget browser;
    bridge->registerTab(&browser, "old");
    AgentEngine engine;
    engine.setProvider(server.provider("openai"));
    auto host = std::make_unique<AgentToolHost>(&engine, bridge.get(), "owner",
                                                data.path());
    bridge.reset();
    engine.submit("No browser");
    QTRY_COMPARE(server.requests.size(), 1);
    engine.cancel();
    const auto prompt = instructions(server.requests[0], "openai");
    QVERIFY(prompt.contains("<shell-sessions>"));
    QVERIFY(!prompt.contains("## Current browser tabs"));
    host.reset();
    QSignalSpy failed(&engine, &AgentEngine::failed);
    engine.submit("No host");
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 3000);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!failed.isEmpty());
    QVERIFY(failed.last()[0].toString().contains("unavailable"));
  }
  void invalidHostIdentityDoesNotListGlobalTabs() {
    QTemporaryDir data;
    Server server;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    bridge.registerTab(&browser, "global");
    AgentEngine engine;
    engine.setProvider(server.provider("openai"));
    AgentToolHost host(&engine, &bridge, QString(), data.path());
    engine.submit("No implicit global scope");
    QTRY_COMPARE(server.requests.size(), 1);
    engine.cancel();
    QVERIFY(
        !instructions(server.requests[0], "openai").contains("<open-tabs>"));
  }

  void harnessDoesNotReceiveNativeBrowserContext_data() {
    QTest::addColumn<bool>("codex");
    QTest::newRow("codex") << true;
    QTest::newRow("opencode") << false;
  }
  void harnessDoesNotReceiveNativeBrowserContext() {
    QFETCH(bool, codex);
    QTemporaryDir data;
    const auto log = data.filePath("fixture-calls.jsonl");
    const char *env = codex ? "ADE_HARNESS_FIXTURE_REQUESTS" : "ADE_ACP_LOG";
    qputenv(env, log.toUtf8());
    BrowserToolBridge bridge;
    BrowserWidget browser;
    bridge.registerTab(&browser, "HTTP_ONLY_TAB_SENTINEL");
    emit browser.titleChanged("HTTP_ONLY_TITLE_SENTINEL");
    bridge.setActiveTab("HTTP_ONLY_TAB_SENTINEL");
    ProviderConfig provider;
    provider.id = "managed-fixture";
    provider.name = "Managed fixture";
    provider.kind = codex ? "codex" : "opencode";
    provider.protocol = codex ? "codex-app-server" : "opencode-acp";
    provider.model = codex ? "model-a" : "model-b";
    provider.harnessExecutable =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(codex ? "harness_fixture.exe" : "acp_fixture.exe");
    AgentEngine engine;
    engine.setProvider(provider);
    AgentToolHost host(&engine, &bridge, "owner", data.path());
    engine.setExternalHarnessContext("EXTERNAL_CONTEXT_PRESERVED");
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Describe actual runtime only", {}));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 8000);
    QVERIFY(engine.lastTurnSucceeded());
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto wire = file.readAll();
    QVERIFY(wire.contains("EXTERNAL_CONTEXT_PRESERVED"));
    QVERIFY(!wire.contains("HTTP_ONLY_TAB_SENTINEL"));
    QVERIFY(!wire.contains("HTTP_ONLY_TITLE_SENTINEL"));
    QVERIFY(!wire.contains("<open-tabs>"));
    QVERIFY(!wire.contains("getBrowserTabs"));
    qunsetenv(env);
  }
  void cleanup() {
    qunsetenv("ADE_HARNESS_FIXTURE_REQUESTS");
    qunsetenv("ADE_ACP_LOG");
  }
};
QTEST_MAIN(BrowserContextRuntimeTest)
#include "test_browser_context_runtime.moc"
