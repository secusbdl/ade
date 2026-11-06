#include "QuestionMcpBridge.h"
#include "QuestionMcpProtocol.h"
#include "core/QuestionForm.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject schema() {
  return QuestionForm::toolDefinition()["function"]
      .toObject()["parameters"]
      .toObject();
}
QJsonObject form() {
  return {
      {"title", "Fixture"},
      {"steps", QJsonArray{QJsonObject{
                    {"fields", QJsonArray{QJsonObject{{"questionId", "answer"},
                                                      {"type", "input"},
                                                      {"label", "Reply"},
                                                      {"required", true}}}}}}}};
}
QJsonObject answer() {
  return {{"completed", true},
          {"cancelled", false},
          {"answers", QJsonObject{{"answer", "synthetic"}}},
          {"completedSteps", 1}};
}
QJsonObject request(QJsonValue id, QString method, QJsonObject params = {}) {
  return {
      {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}
QJsonObject call(QJsonValue id) {
  return request(
      id, "tools/call",
      {{"name", "stagewise_request_user_input"}, {"arguments", form()}});
}
void initialize(QuestionMcpProtocol &protocol) {
  protocol.receive(
      request(1, "initialize", {{"protocolVersion", "2025-11-25"}}));
  protocol.receive(
      {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
}
QHash<QString, QString> environment(const QJsonObject &config) {
  QHash<QString, QString> result;
  for (const auto &value : config["env"].toArray()) {
    const auto v = value.toObject();
    result.insert(v["name"].toString(), v["value"].toString());
  }
  return result;
}
QNetworkReply *
post(QNetworkAccessManager &network, const QJsonObject &config,
     QByteArray body = QJsonDocument(form()).toJson(QJsonDocument::Compact),
     bool token = true) {
  const auto values = environment(config);
  QNetworkRequest r{QUrl(values["STAGEWISE_MCP_CALLBACK_URL"])};
  r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  if (token)
    r.setRawHeader("Authorization",
                   "Bearer " +
                       values["STAGEWISE_MCP_CALLBACK_TOKEN"].toLatin1());
  return network.post(r, body);
}
void launch(QProcess &process, const QJsonObject &config,
            const QString &executable = QString::fromUtf8(MCP_HELPER)) {
  auto env = QProcessEnvironment::systemEnvironment();
  const auto values = environment(config);
  for (auto it = values.begin(); it != values.end(); ++it)
    env.insert(it.key(), it.value());
  process.setProcessEnvironment(env);
  process.setProgram(executable);
  process.setArguments({"--question-mcp-stdio"});
  process.start();
}
void send(QProcess &process, const QJsonObject &value) {
  process.write(QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n');
}
QList<QJsonObject> receive(QProcess &process, QByteArray &buffer) {
  buffer += process.readAllStandardOutput();
  QList<QJsonObject> result;
  while (buffer.contains('\n')) {
    const auto index = buffer.indexOf('\n');
    result.append(QJsonDocument::fromJson(buffer.left(index)).object());
    buffer.remove(0, index + 1);
  }
  return result;
}
} // namespace
class McpTests : public QObject {
  Q_OBJECT
private slots:
  void protocolCatalogAndBoundaries() {
    QuestionMcpProtocol protocol(schema());
    QSignalSpy output(&protocol, &QuestionMcpProtocol::output);
    protocol.receive(request(0, "tools/list"));
    QVERIFY(output.takeLast()[0].toJsonObject().contains("error"));
    initialize(protocol);
    QVERIFY(output.last()[0]
                .toJsonObject()["result"]
                .toObject()["capabilities"]
                .toObject()
                .contains("tools"));
    protocol.receive(request(2, "tools/list"));
    const auto tools =
        output.last()[0].toJsonObject()["result"].toObject()["tools"].toArray();
    QCOMPARE(tools.size(), 1);
    QCOMPARE(tools[0].toObject()["name"].toString(),
             "stagewise_request_user_input");
    QCOMPARE(tools[0].toObject()["inputSchema"].toObject(), schema());
    protocol.receive(request(
        3, "tools/call", {{"name", "execute"}, {"arguments", QJsonObject{}}}));
    QVERIFY(output.last()[0].toJsonObject().contains("error"));
  }
  void protocolOnlyOneQuestionAndExactCancellation() {
    QuestionMcpProtocol protocol(schema());
    initialize(protocol);
    QSignalSpy questions(&protocol, &QuestionMcpProtocol::questionRequested),
        output(&protocol, &QuestionMcpProtocol::output),
        cancelled(&protocol, &QuestionMcpProtocol::questionCancelled);
    protocol.receive(call("one"));
    protocol.receive(call("two"));
    QCOMPARE(questions.size(), 1);
    QVERIFY(output.last()[0]
                .toJsonObject()["result"]
                .toObject()["structuredContent"]
                .toObject()["cancelled"]
                .toBool());
    protocol.receive({{"jsonrpc", "2.0"},
                      {"method", "notifications/cancelled"},
                      {"params", QJsonObject{{"requestId", "foreign"}}}});
    QVERIFY(protocol.hasPending());
    protocol.receive({{"jsonrpc", "2.0"},
                      {"method", "notifications/cancelled"},
                      {"params", QJsonObject{{"requestId", "one"}}}});
    QCOMPARE(cancelled.size(), 1);
    QVERIFY(!protocol.complete("one", answer()));
  }
  void duplicateCannotReplaceOriginal() {
    QuestionMcpProtocol protocol(schema());
    initialize(protocol);
    QSignalSpy questions(&protocol, &QuestionMcpProtocol::questionRequested),
        output(&protocol, &QuestionMcpProtocol::output);
    protocol.receive(call(1));
    protocol.receive(call(1));
    QCOMPARE(questions.size(), 1);
    QVERIFY(output.last()[0].toJsonObject().contains("error"));
    QVERIFY(protocol.complete(1, answer()));
    QCOMPARE(output.last()[0]
                 .toJsonObject()["result"]
                 .toObject()["structuredContent"]
                 .toObject(),
             answer());
    protocol.receive(call(1));
    QCOMPARE(questions.size(), 2);
  }
  void protocolRequestObserverCanDeleteOwner() {
    auto *protocol = new QuestionMcpProtocol(schema());
    initialize(*protocol);
    QPointer<QuestionMcpProtocol> guarded = protocol;
    connect(protocol, &QuestionMcpProtocol::questionRequested, this,
            [protocol] { delete protocol; });
    protocol->receive(call(1));
    QVERIFY(!guarded);
  }
  void bridgeCompletesOnlyOwningTurn() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    QVERIFY(!config.isEmpty());
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    bridge.beginTurn("turn-a");
    auto *reply = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    QCOMPARE(questions[0][1].toString(), "turn-a");
    const auto token = questions[0][0].toString();
    QVERIFY(!bridge.complete("foreign", answer()));
    QVERIFY(bridge.complete(token, answer()));
    QTRY_VERIFY(reply->isFinished());
    QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object(), answer());
    QVERIFY(!bridge.complete(token, answer()));
  }
  void bridgeRejectsUnauthenticatedAndIdle() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *unauth = post(network, config, {}, false);
    QTRY_VERIFY(unauth->isFinished());
    QCOMPARE(
        unauth->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        401);
    auto *idle = post(network, config);
    QTRY_VERIFY(idle->isFinished());
    QCOMPARE(idle->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
             400);
    QCOMPARE(questions.size(), 0);
  }
  void bridgeConcurrentAndDisconnect() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("turn");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested),
        cancelled(&bridge, &QuestionMcpBridge::questionCancelled);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *first = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    auto *second = post(network, config);
    QTRY_VERIFY(second->isFinished());
    QVERIFY(QJsonDocument::fromJson(second->readAll())
                .object()["cancelled"]
                .toBool());
    QCOMPARE(questions.size(), 1);
    first->abort();
    QTRY_COMPARE(cancelled.size(), 1);
    QVERIFY(!bridge.complete(questions[0][0].toString(), answer()));
  }
  void bridgeTurnReplacementRejectsOldResult() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("a");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *old = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    const auto oldToken = questions[0][0].toString();
    bridge.beginTurn("b");
    QTRY_VERIFY(old->isFinished());
    QVERIFY(
        QJsonDocument::fromJson(old->readAll()).object()["cancelled"].toBool());
    auto *current = post(network, config);
    QTRY_COMPARE(questions.size(), 2);
    QVERIFY(!bridge.complete(oldToken, answer()));
    QVERIFY(bridge.complete(questions[1][0].toString(), answer()));
    QTRY_VERIFY(current->isFinished());
  }
  void bridgeObserverMayDeleteOwner() {
    auto *bridge = new QuestionMcpBridge;
    QPointer<QuestionMcpBridge> guard = bridge;
    const auto config = bridge->start(MCP_HELPER);
    bridge->beginTurn("turn");
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    connect(bridge, &QuestionMcpBridge::questionRequested, this,
            [bridge] { delete bridge; });
    auto *reply = post(network, config);
    QTRY_VERIFY(!guard);
    QTRY_VERIFY(reply->isFinished());
    QVERIFY(reply->error() != QNetworkReply::NoError);
  }
  void helperRealStdioAndNativeForm_data() {
    QTest::addColumn<QString>("executable");
    QTest::newRow("console-helper") << QString::fromUtf8(MCP_HELPER);
    QTest::newRow("gui-subsystem-helper") << QString::fromUtf8(MCP_GUI_HELPER);
    QTest::newRow("actual-ADE-executable") << QString::fromUtf8(MCP_ADE_HELPER);
  }
  void helperRealStdioAndNativeForm() {
    QFETCH(QString, executable);
    QuestionMcpBridge bridge;
    const auto config = bridge.start(executable);
    bridge.beginTurn("turn");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QProcess process;
    launch(process, config, executable);
    QVERIFY(process.waitForStarted());
    QByteArray bytes;
    auto init = QJsonDocument(request(1, "initialize",
                                      {{"protocolVersion", "2025-11-25"}}))
                    .toJson(QJsonDocument::Compact) +
                '\n';
    process.write(init.left(9));
    QTest::qWait(20);
    QVERIFY(process.readAllStandardOutput().isEmpty());
    process.write(init.mid(9));
    QList<QJsonObject> frames;
    QTRY_VERIFY(!(frames += receive(process, bytes)).isEmpty());
    QCOMPARE(frames[0]["id"].toInt(), 1);
    send(process,
         {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    send(process, call("native-question"));
    QTRY_COMPARE(questions.size(), 1);
    QVERIFY(bridge.complete(questions[0][0].toString(), answer()));
    frames.clear();
    QTRY_VERIFY(!(frames += receive(process, bytes)).isEmpty());
    QCOMPARE(frames[0]["id"].toString(), "native-question");
    QCOMPARE(frames[0]["result"].toObject()["structuredContent"].toObject(),
             answer());
    QVERIFY(process.readAllStandardError().isEmpty());
    process.closeWriteChannel();
    QTRY_COMPARE(process.state(), QProcess::NotRunning);
    QCOMPARE(process.exitCode(), 0);
  }
  void helperCancellationDisconnectsExactForm() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("turn");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested),
        cancelled(&bridge, &QuestionMcpBridge::questionCancelled);
    QProcess process;
    launch(process, config);
    QVERIFY(process.waitForStarted());
    send(process,
         request(1, "initialize", {{"protocolVersion", "2025-11-25"}}));
    send(process,
         {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    send(process, call(2));
    QTRY_COMPARE(questions.size(), 1);
    send(process, {{"jsonrpc", "2.0"},
                   {"method", "notifications/cancelled"},
                   {"params", QJsonObject{{"requestId", 2}}}});
    QTRY_COMPARE(cancelled.size(), 1);
    QVERIFY(!bridge.complete(questions[0][0].toString(), answer()));
    process.closeWriteChannel();
    QTRY_COMPARE(process.state(), QProcess::NotRunning);
  }
  void windowsGuiSubsystemPreservesStdio() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    QProcess process;
    launch(process, config, MCP_GUI_HELPER);
    QVERIFY(process.waitForStarted());
    send(process,
         request(1, "initialize", {{"protocolVersion", "2025-11-25"}}));
    QByteArray bytes;
    QList<QJsonObject> frames;
    QTRY_VERIFY(!(frames += receive(process, bytes)).isEmpty());
    QCOMPARE(frames[0]["id"].toInt(), 1);
    QVERIFY(process.readAllStandardError().isEmpty());
    process.closeWriteChannel();
    QTRY_COMPARE(process.state(), QProcess::NotRunning);
    QCOMPARE(process.exitCode(), 0);
  }
  void invalidHelperCallbackCannotContactExternalHost() {
    QuestionMcpBridge bridge;
    auto config = bridge.start(MCP_HELPER);
    config["env"] = QJsonArray{
        QJsonObject{
            {"name", "STAGEWISE_MCP_CALLBACK_URL"},
            {"value", "https://example.invalid/tools/request-user-input"}},
        QJsonObject{{"name", "STAGEWISE_MCP_CALLBACK_TOKEN"},
                    {"value", QString(64, 'a')}}};
    QProcess process;
    launch(process, config);
    QVERIFY(process.waitForStarted());
    QTRY_COMPARE(process.state(), QProcess::NotRunning);
    QCOMPARE(process.exitCode(), 3);
    QVERIFY(process.readAllStandardOutput().isEmpty());
    QVERIFY(process.readAllStandardError().isEmpty());
  }
  void revocationRotatesCapabilityAndRejectsOldOwner() {
    QuestionMcpBridge bridge;
    const auto old = bridge.start(MCP_HELPER);
    bridge.revoke();
    const auto current = bridge.start(MCP_HELPER);
    QVERIFY(environment(old)["STAGEWISE_MCP_CALLBACK_TOKEN"] !=
            environment(current)["STAGEWISE_MCP_CALLBACK_TOKEN"]);
    auto foreign = current;
    auto env = current["env"].toArray();
    auto token = env[1].toObject();
    token["value"] = environment(old)["STAGEWISE_MCP_CALLBACK_TOKEN"];
    env[1] = token;
    foreign["env"] = env;
    bridge.beginTurn("current");
    QSignalSpy asked(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *response = post(network, foreign);
    QTRY_VERIFY(response->isFinished());
    QCOMPARE(
        response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        401);
    QCOMPARE(asked.size(), 0);
  }
  void rawHttpRejectsAmbiguousAndOversizedRequests_data() {
    QTest::addColumn<QByteArray>("extra");
    QTest::addColumn<QByteArray>("length");
    QTest::addColumn<int>("status");
    QTest::newRow("origin") << QByteArray("Origin: https://example.invalid\r\n")
                            << QByteArray("2") << 401;
    QTest::newRow("duplicate-length")
        << QByteArray("Content-Length: 2\r\n") << QByteArray("2") << 400;
    QTest::newRow("chunked") << QByteArray("Transfer-Encoding: chunked\r\n")
                             << QByteArray("2") << 400;
    QTest::newRow("oversized") << QByteArray{} << QByteArray("262145") << 413;
    QTest::newRow("invalid-length") << QByteArray{} << QByteArray("+2") << 413;
  }
  void rawHttpRejectsAmbiguousAndOversizedRequests() {
    QFETCH(QByteArray, extra);
    QFETCH(QByteArray, length);
    QFETCH(int, status);
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("turn");
    const auto values = environment(config);
    const QUrl url(values["STAGEWISE_MCP_CALLBACK_URL"]);
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, url.port());
    QTRY_COMPARE(socket.state(), QTcpSocket::ConnectedState);
    socket.write("POST /tools/request-user-input HTTP/1.1\r\nHost: 127.0.0.1:" +
                 QByteArray::number(url.port()) + "\r\nAuthorization: Bearer " +
                 values["STAGEWISE_MCP_CALLBACK_TOKEN"].toLatin1() + "\r\n" +
                 extra + "Content-Length: " + length + "\r\n\r\n{}");
    QByteArray response;
    QTRY_VERIFY((response += socket.readAll()).contains("\r\n\r\n"));
    QVERIFY(response.startsWith("HTTP/1.1 " + QByteArray::number(status)));
    QCOMPARE(questions.size(), 0);
  }
  void replacementDuringCancellationWinsOverObsoleteBeginTurn() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("old");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *old = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    connect(&bridge, &QuestionMcpBridge::questionCancelled, this,
            [&] { bridge.beginTurn("newest"); });
    bridge.beginTurn("obsolete");
    QTRY_VERIFY(old->isFinished());
    auto *current = post(network, config);
    QTRY_COMPARE(questions.size(), 2);
    QCOMPARE(questions[1][1].toString(), "newest");
    QVERIFY(bridge.complete(questions[1][0].toString(), answer()));
    QTRY_VERIFY(current->isFinished());
  }
  void revokedOwnerCannotBeReactivatedByObsoleteBeginTurn() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("old");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *old = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    connect(&bridge, &QuestionMcpBridge::questionCancelled, this,
            [&] { bridge.revoke(); });
    bridge.beginTurn("obsolete");
    QTRY_VERIFY(old->isFinished());
    const auto next = bridge.start(MCP_HELPER);
    auto *current = post(network, next);
    QTRY_VERIFY(current->isFinished());
    QCOMPARE(
        current->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        400);
    QCOMPARE(questions.size(), 1);
  }
  void invalidUtf8CannotReachQuestionPresenter() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    bridge.beginTurn("turn");
    QSignalSpy questions(&bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QByteArray bytes = QByteArray::fromHex("7b227469746c65223a22");
    bytes.append(char(0xff));
    bytes += QByteArray::fromHex("227d");
    auto *response = post(network, config, bytes);
    QTRY_VERIFY(response->isFinished());
    QCOMPARE(
        response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        400);
    QCOMPARE(questions.size(), 0);
  }
  void helperRejectsInvalidUtf8AndRecoversNextFrame() {
    QuestionMcpBridge bridge;
    const auto config = bridge.start(MCP_HELPER);
    QProcess process;
    launch(process, config);
    QVERIFY(process.waitForStarted());
    QByteArray bad = QByteArray::fromHex(
        "7b226a736f6e727063223a22322e30222c226964223a372c226d6574686f64223a22");
    bad.append(char(0xff));
    bad += QByteArray::fromHex("227d");
    bad += '\n';
    process.write(bad);
    send(process, request(8, "ping"));
    QByteArray bytes;
    QList<QJsonObject> frames;
    QTRY_VERIFY((frames += receive(process, bytes)).size() >= 2);
    QCOMPARE(frames[0]["error"].toObject()["code"].toInt(), -32700);
    QCOMPARE(frames[1]["id"].toInt(), 8);
    process.closeWriteChannel();
    QTRY_COMPARE(process.state(), QProcess::NotRunning);
  }
  void ownerDeletionFromCancellationIsSafe() {
    auto *bridge = new QuestionMcpBridge;
    QPointer<QuestionMcpBridge> guard = bridge;
    const auto config = bridge->start(MCP_HELPER);
    bridge->beginTurn("old");
    QSignalSpy questions(bridge, &QuestionMcpBridge::questionRequested);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    auto *response = post(network, config);
    QTRY_COMPARE(questions.size(), 1);
    connect(bridge, &QuestionMcpBridge::questionCancelled, this,
            [bridge] { delete bridge; });
    bridge->beginTurn("replacement");
    QVERIFY(!guard);
    QTRY_VERIFY(response->isFinished());
  }
};
QTEST_GUILESS_MAIN(McpTests)
#include "test_question_mcp.moc"
