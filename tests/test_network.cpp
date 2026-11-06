#include "network/NetworkBroker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QtTest>
#include <QScopeGuard>
#include <new>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <functional>

using namespace ade;
namespace {
struct Request { QByteArray method, target, body; QMap<QByteArray, QByteArray> headers; };
struct Response {
    int status = 200;
    QByteArray body = "ok";
    QList<QPair<QByteArray, QByteArray>> headers;
    bool hold = false;
    int lengthOverride = -1;
};
class Server : public QObject {
public:
    QTcpServer listener;
    QList<Request> requests;
    std::function<Response(const Request &)> respond;
    Server() {
        listener.listen(QHostAddress::LocalHost);
        connect(&listener, &QTcpServer::newConnection, this, [this] {
            while (listener.hasPendingConnections()) {
                auto socket = listener.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("done").toBool()) { socket->readAll(); return; }
                    auto bytes = socket->property("bytes").toByteArray() + socket->readAll(); socket->setProperty("bytes", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n"); if (end < 0) return;
                    Request request; const auto lines = bytes.left(end).split('\n'); const auto first = lines.first().trimmed().split(' ');
                    request.method = first.value(0); request.target = first.value(1);
                    for (int i = 1; i < lines.size(); ++i) {
                        const auto line = lines[i].trimmed(); const auto colon = line.indexOf(':');
                        if (colon > 0) request.headers[line.left(colon).toLower()] = line.mid(colon + 1).trimmed();
                    }
                    const auto size = request.headers.value("content-length").toInt();
                    if (bytes.size() < end + 4 + size) return;
                    request.body = bytes.mid(end + 4, size); socket->setProperty("done", true); requests.append(request);
                    const auto response = respond ? respond(request) : Response{};
                    if (response.hold) return;
                    QByteArray header = "HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Length: "
                        + QByteArray::number(response.lengthOverride < 0 ? response.body.size() : response.lengthOverride) + "\r\nConnection: close\r\n";
                    for (const auto &field : response.headers) header += field.first + ": " + field.second + "\r\n";
                    socket->write(header + "\r\n" + response.body); socket->disconnectFromHost();
                });
            }
        });
    }
    QString url(const QString &path = {}) const { return QString("http://127.0.0.1:%1").arg(listener.serverPort()) + path; }
};
const QString Secret = "FAKE_BROKER_TOKEN_a7c98d+/=only";
QString configure(NetworkBroker &broker, const Server &server, const QString &agent = "agent-a") {
    if (!broker.registerCredentialType({"fixture-pat", {"token"}, {server.url()}, {"project"}})) return {};
    if (!broker.setCredential("fixture-pat", {{"token", Secret}, {"project", "public-project"}})) return {};
    broker.setAllowedCredentialTypes(agent, {"fixture-pat"});
    return broker.credential(agent, "fixture-pat").toObject()["token"].toString();
}
QJsonObject result(const QSignalSpy &spy, int index = 0) { return spy.at(index)[1].toJsonObject(); }
QString error(const QSignalSpy &spy, int index = 0) { return spy.at(index)[2].toString(); }
}

class NetworkTest : public QObject {
    Q_OBJECT
private slots:
    void registryAndOriginPolicy();
    void opaqueSubstitutionAndResponseMetadata();
    void handleIsolationRevocationAndDestination();
    void credentialRedirectIsManual();
    void publicRedirectPolicy();
    void binaryBodyResponseAndHttpErrors();
    void limitsTimeoutCancelAndReuse();
    void revocationIsImmediateAndAgentScoped();
    void responseHeaderAndEncodedUploadLimits();
    void secretReflectionAndTruncatedResponse();
    void malformedRequestsNeverReachNetwork();
    void cancellationObserverMayDeleteBroker_data();
    void cancellationObserverMayDeleteBroker();
};

void NetworkTest::registryAndOriginPolicy() {
    QCOMPARE(NetworkBroker::builtinCredentialTypes().size(), 5);
    QVERIFY(NetworkBroker::originMatches("https://API.GITHUB.COM:443/repos", "https://api.github.com"));
    QVERIFY(!NetworkBroker::originMatches("https://api.github.com.evil.test", "https://api.github.com"));
    QVERIFY(!NetworkBroker::originMatches("https://api.github.com@evil.test", "https://api.github.com"));
    QVERIFY(!NetworkBroker::originMatches("http://api.github.com", "https://api.github.com"));
    QVERIFY(!NetworkBroker::originMatches("https://api.github.com:444", "https://api.github.com"));
    QVERIFY(NetworkBroker::originMatches("https://one.api.example.test:443/path", "https://*.api.example.test"));
    QVERIFY(!NetworkBroker::originMatches("https://two.one.api.example.test", "https://*.api.example.test"));
    QVERIFY(!NetworkBroker::originMatches("https://api.example.test", "https://*.api.example.test"));
    QVERIFY(!NetworkBroker::originMatches("https://example.test", "https://*"));
    QVERIFY(!NetworkBroker::originMatches("https://evil.test", "https://api.example.test/path"));
    NetworkBroker broker; QString failure;
    QVERIFY(!broker.registerCredentialType({"invalid", {"token"}, {"https://*"}, {}}, &failure));
    QVERIFY(!failure.isEmpty()); QVERIFY(!broker.setCredential("openai", {{"apiKey", "fake"}}, &failure));
    QVERIFY(broker.credential("agent-a", "github-pat").isNull());
    QVERIFY(broker.setCredential("github-pat", {{"token", Secret}}));
    QVERIFY(broker.credential("agent-a", "github-pat").isNull());
    broker.setAllowedCredentialTypes("agent-a", {"github-pat"});
    const auto opaque = broker.credential("agent-a", "github-pat").toObject()["token"].toString();
    QVERIFY(opaque.startsWith("{{CRED:github-pat:token:")); QVERIFY(!opaque.contains(Secret));
}

void NetworkTest::opaqueSubstitutionAndResponseMetadata() {
    Server server; NetworkBroker broker; const auto handle = configure(broker, server); QVERIFY(!handle.isEmpty());
    const auto safe = broker.credential("agent-a", "fixture-pat").toObject();
    QCOMPARE(safe["project"].toString(), QString("public-project")); QCOMPARE(safe["token"].toString(), handle);
    QSignalSpy done(&broker, &NetworkBroker::completed);
    const auto url = server.url("/submit?token=") + handle;
    broker.fetch("substitute", "agent-a", {{"url", url}, {"method", "POST"},
        {"headers", QJsonArray{QJsonArray{"Authorization", "Bearer " + handle}}}, {"body", "token=" + handle}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(error(done).isEmpty()); QCOMPARE(server.requests.size(), 1);
    QCOMPARE(server.requests.first().headers.value("authorization"), "Bearer " + Secret.toUtf8());
    QCOMPARE(server.requests.first().body, "token=" + Secret.toUtf8());
    QVERIFY(server.requests.first().target.contains(QUrl::toPercentEncoding(Secret)));
    QVERIFY(result(done)["ok"].toBool()); QCOMPARE(result(done)["url"].toString(), url);
    QVERIFY(!QJsonDocument(result(done)).toJson().contains(Secret.toUtf8()));
    QCOMPARE(QByteArray::fromBase64(result(done)["bodyBase64"].toString().toLatin1()), QByteArray("ok"));
}

void NetworkTest::handleIsolationRevocationAndDestination() {
    Server allowed, other; NetworkBroker broker; const auto handle = configure(broker, allowed); QSignalSpy done(&broker, &NetworkBroker::completed);
    const QJsonObject headers{{"Authorization", "Bearer " + handle}};
    broker.fetch("destination", "agent-a", {{"url", other.url("/steal")}, {"headers", headers}});
    QCOMPARE(done.size(), 1); QVERIFY(!error(done).isEmpty()); QCOMPARE(other.requests.size(), 0);
    broker.setAllowedCredentialTypes("agent-b", {"fixture-pat"});
    broker.fetch("foreign", "agent-b", {{"url", allowed.url()}, {"headers", headers}});
    QCOMPARE(done.size(), 2); QVERIFY(!error(done, 1).isEmpty()); QCOMPARE(allowed.requests.size(), 0);
    broker.resetAgent("agent-a"); const auto refreshed = broker.credential("agent-a", "fixture-pat").toObject()["token"].toString();
    QVERIFY(refreshed != handle);
    broker.fetch("stale", "agent-a", {{"url", allowed.url()}, {"headers", headers}});
    QCOMPARE(done.size(), 3); QVERIFY(!error(done, 2).isEmpty());
    broker.removeCredential("fixture-pat"); QVERIFY(broker.credential("agent-a", "fixture-pat").isNull());
    broker.fetch("removed", "agent-a", {{"url", allowed.url()}, {"headers", QJsonObject{{"Authorization", refreshed}}}});
    QCOMPARE(done.size(), 4); QVERIFY(!error(done, 3).isEmpty()); QCOMPARE(allowed.requests.size(), 0);
    for (const auto &entry : done) QVERIFY(!entry[2].toString().contains(Secret));
}

void NetworkTest::credentialRedirectIsManual() {
    Server server, other;
    server.respond = [&](const Request &) { return Response{302, "moved", {{"Location", other.url("/steal").toUtf8()}}}; };
    NetworkBroker broker; const auto handle = configure(broker, server); QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("redirect", "agent-a", {{"url", server.url("/start")}, {"redirect", "follow"}, {"headers", QJsonObject{{"Authorization", handle}}}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(error(done).isEmpty()); QCOMPARE(result(done)["status"].toInt(), 302);
    QVERIFY(!result(done)["redirected"].toBool()); QCOMPARE(server.requests.size(), 1); QCOMPARE(other.requests.size(), 0);
}

void NetworkTest::publicRedirectPolicy() {
    Server source, destination;
    source.respond = [&](const Request &) { return Response{302, {}, {{"Location", destination.url("/final").toUtf8()}}}; };
    NetworkBroker broker; QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("redirect", "agent-a", {{"url", source.url("/start")}, {"method", "POST"}, {"body", "client-body"},
        {"headers", QJsonObject{{"Authorization", "client-owned-token"}, {"X-Private-Header", "do-not-forward"}}}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(error(done).isEmpty()); QVERIFY(result(done)["redirected"].toBool());
    QCOMPARE(destination.requests.size(), 1); QCOMPARE(destination.requests.first().method, QByteArray("GET"));
    QVERIFY(destination.requests.first().body.isEmpty()); QVERIFY(!destination.requests.first().headers.contains("authorization"));
    QVERIFY(!destination.requests.first().headers.contains("x-private-header"));
    Server loop; loop.respond = [](const Request &) { return Response{302, {}, {{"Location", "/loop"}}}; };
    NetworkLimits limits; limits.maxRedirects = 2; QVERIFY(broker.setLimits(limits));
    broker.fetch("loop", "agent-a", {{"url", loop.url("/loop")}}); QTRY_COMPARE(done.size(), 2);
    QVERIFY(error(done, 1).contains("redirect limit")); QCOMPARE(loop.requests.size(), 3);
}

void NetworkTest::binaryBodyResponseAndHttpErrors() {
    const auto binary = QByteArray::fromHex("00ff010203c3b7fe00");
    Server server; server.respond = [&](const Request &request) {
        return Response{request.target == "/missing" ? 404 : 200, request.target == "/missing" ? QByteArray("missing") : binary,
            {{"Content-Type", "application/octet-stream"}, {"Set-Cookie", "ambient=forbidden"}}};
    };
    NetworkBroker broker; QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("binary", "agent-a", {{"url", server.url("/binary")}, {"method", "POST"}, {"bodyBase64", QString::fromLatin1(binary.toBase64())}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(error(done).isEmpty()); QCOMPARE(server.requests.first().body, binary);
    QCOMPARE(QByteArray::fromBase64(result(done)["bodyBase64"].toString().toLatin1()), binary);
    QVERIFY(!QJsonDocument(result(done)).toJson().contains("ambient=forbidden"));
    broker.fetch("missing", "agent-b", {{"url", server.url("/missing")}}); QTRY_COMPARE(done.size(), 2);
    QVERIFY(error(done, 1).isEmpty()); QCOMPARE(result(done, 1)["status"].toInt(), 404); QVERIFY(!result(done, 1)["ok"].toBool());
    QVERIFY(!server.requests.last().headers.contains("cookie"));
}

void NetworkTest::limitsTimeoutCancelAndReuse() {
    Server server; server.respond = [](const Request &request) {
        return request.target == "/hold" ? Response{200, {}, {}, true} : Response{200, QByteArray(100, 'x'), {}};
    };
    NetworkBroker broker; NetworkLimits limits; limits.timeoutMs = 100; limits.maxResponseBytes = 20; limits.maxRequestBytes = 20;
    QVERIFY(broker.setLimits(limits)); QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("large-response", "agent-a", {{"url", server.url("/large")}}); QTRY_COMPARE(done.size(), 1); QVERIFY(!error(done).isEmpty());
    broker.fetch("large-upload", "agent-a", {{"url", server.url("/large")}, {"method", "POST"}, {"body", QString(21, 'x')}});
    QCOMPARE(done.size(), 2); QCOMPARE(server.requests.size(), 1);
    broker.fetch("timeout", "agent-a", {{"url", server.url("/hold")}}); QTRY_COMPARE(done.size(), 3); QVERIFY(error(done, 2).contains("deadline"));
    limits.timeoutMs = 5000; limits.maxResponseBytes = 200; QVERIFY(broker.setLimits(limits));
    broker.fetch("reuse", "agent-a", {{"url", server.url("/hold")}}); QTRY_COMPARE(server.requests.size(), 3);
    broker.fetch("reuse", "agent-a", {{"url", server.url("/large")}}); QCOMPARE(server.requests.size(), 3);
    broker.cancel("reuse"); QCOMPARE(done.size(), 4); QVERIFY(error(done, 3).contains("cancelled"));
    broker.fetch("reuse", "agent-a", {{"url", server.url("/large")}}); QTRY_COMPARE(done.size(), 5); QVERIFY(error(done, 4).isEmpty());
    QTest::qWait(30); QCOMPARE(done.size(), 5);
}

void NetworkTest::secretReflectionAndTruncatedResponse() {
    for (int mode = 0; mode < 4; ++mode) {
        Server server; server.respond = [mode](const Request &) {
            if (mode == 0) return Response{200, Secret.toUtf8(), {}};
            if (mode == 1) return Response{200, Secret.toUtf8().toBase64(), {}};
            if (mode == 2) return Response{200, "safe", {{"X-Echo", QUrl::toPercentEncoding(Secret)}}};
            return Response{200, "partial", {}, false, 100};
        };
        NetworkBroker broker; const auto handle = configure(broker, server); QSignalSpy done(&broker, &NetworkBroker::completed);
        broker.fetch("echo", "agent-a", {{"url", server.url()}, {"headers", QJsonObject{{"Authorization", handle}}}});
        QTRY_COMPARE(done.size(), 1); QVERIFY(!error(done).isEmpty()); QVERIFY(result(done).isEmpty());
        QVERIFY(!error(done).contains(Secret));
    }
}

void NetworkTest::revocationIsImmediateAndAgentScoped() {
    Server server; server.respond = [](const Request &) { return Response{200, {}, {}, true}; };
    NetworkBroker broker; const auto oldHandle = configure(broker, server);
    QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("revoked", "agent-a", {{"url", server.url()}, {"headers", QJsonObject{{"Authorization", oldHandle}}}});
    broker.fetch("other-agent", "agent-b", {{"url", server.url()}});
    QTRY_COMPARE(server.requests.size(), 2);
    connect(&broker, &NetworkBroker::completed, &broker, [&](const QString &id, const QJsonObject &, const QString &) {
        if (id == "revoked") broker.fetch("reentrant", "agent-a", {{"url", server.url()}, {"headers", QJsonObject{{"Authorization", oldHandle}}}});
    });
    broker.resetAgent("agent-a");
    QCOMPARE(done.size(), 2); QVERIFY(error(done, 0).contains("cancelled")); QVERIFY(error(done, 1).contains("revoked"));
    QCOMPARE(server.requests.size(), 2);
    broker.cancelAgent("agent-b"); QCOMPARE(done.size(), 3); QCOMPARE(done[2][0].toString(), QString("other-agent"));
    const auto newHandle = broker.credential("agent-a", "fixture-pat").toObject()["token"].toString();
    QVERIFY(!newHandle.isEmpty()); QVERIFY(newHandle != oldHandle);
    broker.fetch("changed", "agent-a", {{"url", server.url()}, {"headers", QJsonObject{{"Authorization", newHandle}}}});
    QTRY_COMPARE(server.requests.size(), 3);
    QVERIFY(broker.setCredential("fixture-pat", {{"token", Secret + "-rotated"}, {"project", "new-project"}}));
    QCOMPARE(done.size(), 4); QVERIFY(error(done, 3).contains("changed"));
}

void NetworkTest::responseHeaderAndEncodedUploadLimits() {
    Server server; server.respond = [](const Request &request) {
        if (request.target == "/headers") return Response{200, "ok", {{"X-Large", QByteArray(65536, 'h')}}};
        if (request.target == "/not-modified") return Response{304, {}, {{"Location", "/must-not-follow"}}};
        return Response{};
    };
    NetworkBroker broker; NetworkLimits limits; limits.maxRequestBytes = 10; QVERIFY(broker.setLimits(limits));
    QSignalSpy done(&broker, &NetworkBroker::completed);
    broker.fetch("encoded", "agent-a", {{"url", server.url()}, {"method", "POST"}, {"bodyBase64", QString::fromLatin1(QByteArray(11, 'x').toBase64())}});
    QCOMPARE(done.size(), 1); QVERIFY(!error(done).isEmpty()); QCOMPARE(server.requests.size(), 0);
    broker.fetch("headers", "agent-a", {{"url", server.url("/headers")}}); QTRY_COMPARE(done.size(), 2);
    QVERIFY(!error(done, 1).isEmpty()); QVERIFY(result(done, 1).isEmpty());
    broker.fetch("not-modified", "agent-a", {{"url", server.url("/not-modified")}}); QTRY_COMPARE(done.size(), 3);
    QVERIFY(error(done, 2).isEmpty()); QCOMPARE(result(done, 2)["status"].toInt(), 304);
    QCOMPARE(server.requests.size(), 2);
    limits.maxRequestBytes = 8192; QVERIFY(broker.setLimits(limits));
    configure(broker, server);
    QVERIFY(broker.setCredential("fixture-pat", {{"token", QString(40000, 's')}, {"project", "public"}}));
    const auto handle = broker.credential("agent-a", "fixture-pat").toObject()["token"].toString();
    broker.fetch("expanded-body", "agent-a", {{"url", server.url()}, {"method", "POST"}, {"body", handle + handle}});
    QCOMPARE(done.size(), 4); QVERIFY(error(done, 3).contains("exceeds"));
    broker.fetch("expanded-url", "agent-a", {{"url", server.url("/?key=") + handle + handle}});
    QCOMPARE(done.size(), 5); QVERIFY(error(done, 4).contains("exceeds"));
    broker.fetch("expanded-headers", "agent-a", {{"url", server.url()}, {"headers", QJsonObject{{"Authorization", handle + handle}}}});
    QCOMPARE(done.size(), 6); QVERIFY(!error(done, 5).isEmpty()); QCOMPARE(server.requests.size(), 2);
}

void NetworkTest::malformedRequestsNeverReachNetwork() {
    Server server; NetworkBroker broker; const auto handle = configure(broker, server); QSignalSpy done(&broker, &NetworkBroker::completed);
    const QList<QJsonObject> requests{
        {{"url", "file:///C:/private"}},
        {{"url", server.url()}, {"method", "CONNECT"}},
        {{"url", server.url()}, {"method", 10}},
        {{"url", server.url()}, {"redirect", false}},
        {{"url", server.url()}, {"headers", QJsonObject{{"Host", "elsewhere"}}}},
        {{"url", server.url()}, {"headers", QJsonObject{{"X-Test", "line\r\nInjected: value"}}}},
        {{"url", server.url()}, {"method", "POST"}, {"bodyBase64", "%%%"}},
        {{"url", server.url()}, {"body", "GET body"}},
        {{"url", server.url()}, {"method", "POST"}, {"bodyBase64", QString::fromLatin1(handle.toUtf8().toBase64())}},
        {{"url", "http://" + handle + "@127.0.0.1"}},
        {{"url", server.url()}, {"headers", QJsonObject{{"X-Test", "{{CRED:fixture-pat:token:unknown}}"}}}}
    };
    for (int i = 0; i < requests.size(); ++i) { broker.fetch(QString::number(i), "agent-a", requests[i]); QCOMPARE(done.size(), i + 1); QVERIFY(!error(done, i).isEmpty()); }
    QCOMPARE(server.requests.size(), 0);
}

void NetworkTest::cancellationObserverMayDeleteBroker_data() {
    QTest::addColumn<QString>("operation");
    for (const auto *name : {"cancel", "reset", "remove", "rotate", "register", "allowlist"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void NetworkTest::cancellationObserverMayDeleteBroker() {
    QFETCH(QString, operation);
    Server server; server.respond = [](const Request &) { return Response{200, {}, {}, true}; };
#ifdef Q_OS_WIN
    // Protect destroyed storage so a stale loop access fails deterministically,
    // rather than depending on how long the heap retains a freed QHash value.
    void *storage = VirtualAlloc(nullptr, sizeof(NetworkBroker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    QVERIFY(storage);
    QPointer<NetworkBroker> broker = new (storage) NetworkBroker;
    const auto cleanup = qScopeGuard([&] { if (broker) broker.data()->~NetworkBroker(); VirtualFree(storage, 0, MEM_RELEASE); });
#else
    QPointer<NetworkBroker> broker = new NetworkBroker;
    const auto cleanup = qScopeGuard([&] { delete broker.data(); });
#endif
    const auto handle = configure(*broker, server); QVERIFY(!handle.isEmpty());
    const QJsonObject request{{"url", server.url()}, {"headers", QJsonObject{{"Authorization", handle}}}};
    broker->fetch("first", "agent-a", request); broker->fetch("second", "agent-a", request);
    QTRY_COMPARE(server.requests.size(), 2);
    int completions = 0;
    connect(broker, &NetworkBroker::completed, this, [&](const QString &, const QJsonObject &, const QString &failure) {
        ++completions; QVERIFY(failure.contains("cancelled"));
#ifdef Q_OS_WIN
        broker.data()->~NetworkBroker();
        DWORD oldProtection = 0; QVERIFY(VirtualProtect(storage, sizeof(NetworkBroker), PAGE_NOACCESS, &oldProtection));
#else
        delete broker.data();
#endif
    });
    if (operation == "cancel") broker->cancelAgent("agent-a");
    else if (operation == "reset") broker->resetAgent("agent-a");
    else if (operation == "remove") broker->removeCredential("fixture-pat");
    else if (operation == "rotate") QVERIFY(!broker->setCredential("fixture-pat", {{"token", Secret + "-new"}, {"project", "public"}}));
    else if (operation == "register") QVERIFY(!broker->registerCredentialType({"fixture-pat", {"token"}, {server.url()}, {"project"}}));
    else broker->setAllowedCredentialTypes("agent-a", {});
    QVERIFY(broker.isNull()); QCOMPARE(completions, 1);
    QCoreApplication::processEvents(); QCOMPARE(completions, 1);
}

QTEST_GUILESS_MAIN(NetworkTest)
#include "test_network.moc"
