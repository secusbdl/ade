#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "core/ProviderProtocol.h"
#include "core/ProviderService.h"
#include "core/SseParser.h"
#include "core/ToolExecutor.h"
#include "core/UtilityService.h"
#include "core/ContextLoader.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ade;

namespace {
struct Response { QByteArray body; int status = 200; bool hold = false; QByteArray type = "text/event-stream"; };
class FixtureServer : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    QList<QByteArray> targets;
    QList<QByteArray> headers;
    std::function<Response(int, const QJsonObject &)> respond;
    FixtureServer() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    const auto headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto &line : buffer.left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer.size() < headerEnd + 4 + length) return;
                    socket->setProperty("handled", true);
                    headers.append(buffer.left(headerEnd));
                    targets.append(buffer.left(buffer.indexOf('\r')).split(' ').value(1));
                    const auto object = QJsonDocument::fromJson(buffer.mid(headerEnd + 4, length)).object();
                    requests.append(object);
                    const auto response = respond ? respond(requests.size(), object) : Response{};
                    if (response.hold) return;
                    const auto header = "HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: " + response.type
                        + "\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n";
                    socket->write(header);
                    socket->write(response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ProviderConfig provider() const {
        return {"fixture", "Local fixture", QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()),
                "fixture-secret", "fixture-model", "openai"};
    }
};

QByteArray sseEvent(const QJsonObject &object) { return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n"; }
QByteArray textResponse(const QString &text) {
    const QJsonObject delta{{"content", text}};
    const QJsonObject choice{{"delta", delta}, {"finish_reason", "stop"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
QByteArray toolResponse(const QString &name, const QJsonObject &arguments) {
    const QJsonObject function{{"name", name},
        {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}};
    const QJsonObject call{{"index", 0}, {"id", "call_test"}, {"type", "function"}, {"function", function}};
    const QJsonObject delta{{"tool_calls", QJsonArray{call}}};
    const QJsonObject choice{{"delta", delta}, {"finish_reason", "tool_calls"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
}

class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void toolObserverCancellation_data();
    void toolObserverCancellation();
    void toolObserverDeletion_data();
    void toolObserverDeletion();
    void cancellationFromResultSettlesWholeToolBlockBeforeReplacement();
    void sseArbitraryChunks();
    void sseLimits();
    void pathScope();
    void junctionEscape();
    void appsBinaryBoundsAndJunctionIsolation();
    void editsAreAtomic();
    void fileChangedReentrancy_data();
    void fileChangedReentrancy();
    void fileChangedMayDeleteExecutor();
    void finalProcessOutputMayReplaceOperation();
    void providerStorageAndImport();
    void streamsAndPersistsConversation();
    void approvedToolLoop();
    void rejectedToolDoesNotWrite();
    void cancellationSettlesToolCalls();
    void cancellationStopsNetwork();
    void incompleteStreamPreservesPartial();
    void retriesTransientBeforeOutput();
    void commandAndCancellation();
    void invalidSubmissionSettles();
    void nativeProtocolRequests();
    void anthropicSignedToolRoundTrip();
    void googleSignedToolRoundTrip();
    void modelDiscoveryPagination();
    void providerValidationIsBounded();
    void externalToolRoundTripAndCancellation();
    void malformedProviderRecordsRejected();
    void verifiedProviderRoutingMetadata();
    void modelDiscoveryRejectsInvalidResponses();
    void rawReadsPreserveBytesAndRejectTruncation();
    void responsesSignedToolRoundTrip();
    void responsesValidationAndFailure();
    void modelFallbackState();
    void modelFallbackRuntime();
    void utilityFallbackAndCancellation();
    void compressionPreservesHistoryAndTools();
    void partialCancellationDoesNotSucceed();
    void providerSaveRejectsUnreloadableRecords();
    void systemContextAndReadOnlySkillTool();
};

void CoreTest::sseArbitraryChunks() {
    const auto bytes = QByteArray(":comment\r\ndata: ") + QString::fromUtf8("héllo 🌍").toUtf8()
        + "\r\n\r\ndata: one\ndata: two\n\ndata: [DONE]\n\n";
    const QList<QByteArray> expected{QString::fromUtf8("héllo 🌍").toUtf8(), "one\ntwo", "[DONE]"};
    for (qsizetype i = 0; i <= bytes.size(); ++i) {
        SseParser parser;
        auto events = parser.feed(bytes.left(i));
        events += parser.feed(bytes.mid(i));
        events += parser.finish();
        QCOMPARE(events, expected);
    }
    SseParser single;
    QList<QByteArray> events;
    for (const auto c : bytes) events += single.feed(QByteArray(1, c));
    events += single.finish();
    QCOMPARE(events, expected);
}

void CoreTest::sseLimits() {
    SseParser parser;
    parser.feed("data: " + QByteArray(4 * 1024 * 1024 + 1, 'x'));
    QVERIFY(parser.exceededLimit());
    parser.reset();
    QCOMPARE(parser.feed("data: recovered\n\n"), QList<QByteArray>{"recovered"});
}

void CoreTest::pathScope() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    QVERIFY(!tools.resolvePath("ws1/src/main.cpp").isEmpty());
    QVERIFY(tools.resolvePath("../escape.txt").isEmpty());
    QVERIFY(tools.resolvePath(root.path() + "-sibling/file").isEmpty());
#ifdef Q_OS_WIN
    QVERIFY(tools.resolvePath("a.txt:secret").isEmpty());
    QVERIFY(tools.resolvePath("NUL.txt").isEmpty());
    QVERIFY(tools.resolvePath("folder./file").isEmpty());
    QVERIFY(tools.resolvePath("C:relative.txt").isEmpty());
#endif
}

void CoreTest::junctionEscape() {
#ifdef Q_OS_WIN
    QTemporaryDir root, outside;
    QVERIFY(root.isValid() && outside.isValid());
    const auto link = root.path() + "/junction";
    QProcess process;
    process.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(outside.path())});
    QVERIFY(process.waitForFinished(5000));
    if (process.exitCode() != 0) QSKIP("Creating a test junction is not available on this host.");
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    const bool rejected = tools.resolvePath("junction/new.txt").isEmpty();
    QVERIFY(QDir().rmdir(link));
    QVERIFY(rejected);
#else
    QSKIP("Windows junction regression.");
#endif
}

void CoreTest::editsAreAtomic() {
    QTemporaryDir root;
    QVERIFY(writeFile(root.filePath("a.txt"), "alpha\nbeta\n"));
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    QSignalSpy done(&tools, &ToolExecutor::completed);
    tools.execute("edit", "multiEdit", {{"path", "a.txt"}, {"edits", QJsonArray{
        QJsonObject{{"old_string", "alpha"}, {"new_string", "changed"}},
        QJsonObject{{"old_string", "missing"}, {"new_string", "never"}}}}});
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done.first()[2].toBool());
    QCOMPARE(readFile(root.filePath("a.txt")), QByteArray("alpha\nbeta\n"));
    done.clear();
    tools.execute("write", "write", {{"path", "nested/new.txt"}, {"content", "hello"}});
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!done.first()[2].toBool());
    QCOMPARE(readFile(root.filePath("nested/new.txt")), QByteArray("hello"));
}

void CoreTest::appsBinaryBoundsAndJunctionIsolation() {
    QTemporaryDir apps,outside;QVERIFY(apps.isValid()&&outside.isValid());ToolExecutor tools;
    QVERIFY(tools.setAppsDirectory(apps.path()));QVERIFY(tools.workspace().isEmpty());
    QSignalSpy completed(&tools,&ToolExecutor::completed);
    const auto perform=[&](const QString& name,const QJsonObject& arguments){
        completed.clear();tools.execute("binary",name,arguments);if(completed.isEmpty())completed.wait(4000);
        return completed.isEmpty()?QList<QVariant>{}:completed.last();
    };
    const QByteArray bytes=QByteArray::fromHex("00ff80efbbbf410d0a");
    auto result=perform("writeBytes",{{"path","apps/asset.bin"},{"base64",QString::fromLatin1(bytes.toBase64())}});
    QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY2(!result[2].toBool(),qPrintable(result[1].toString()));QCOMPARE(readFile(apps.filePath("asset.bin")),bytes);
    for(const auto& invalid:QStringList{"%%%","/w=",QString(4*((4*1024*1024+2)/3)+1,'A')}){
        result=perform("writeBytes",{{"path","apps/asset.bin"},{"base64",invalid}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(result[2].toBool());QCOMPARE(readFile(apps.filePath("asset.bin")),bytes);
    }
    const QByteArray maximum(4*1024*1024,static_cast<char>(0xff));QVERIFY(writeFile(apps.filePath("maximum.bin"),maximum));
    result=perform("readBytes",{{"path","apps/maximum.bin"}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY2(!result[2].toBool(),qPrintable(result[1].toString().left(100)));QCOMPARE(QByteArray::fromBase64(result[1].toByteArray()),maximum);
    QFile excessive(apps.filePath("maximum.bin"));QVERIFY(excessive.open(QIODevice::WriteOnly));QVERIFY(excessive.resize(maximum.size()+1));excessive.close();
    result=perform("readBytes",{{"path","apps/maximum.bin"}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(result[2].toBool());
    for(const auto& path:QStringList{"apps/../outside.bin","apps/asset.bin:secret","apps/NUL.txt","apps/folder./file",outside.filePath("secret.txt")})QVERIFY2(tools.resolvePath(path).isEmpty(),qPrintable(path));
#ifdef Q_OS_WIN
    QVERIFY(writeFile(outside.filePath("secret.txt"),"outside-secret"));const auto junction=apps.filePath("escape");
    QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(junction),QDir::toNativeSeparators(outside.path())});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
    const auto cleanup=qScopeGuard([&]{QDir().rmdir(junction);});
    result=perform("readBytes",{{"path","apps/escape/secret.txt"}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(result[2].toBool());QVERIFY(!result[1].toString().contains("outside-secret"));
    result=perform("writeBytes",{{"path","apps/escape/secret.txt"},{"base64","eA=="}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(result[2].toBool());QCOMPARE(readFile(outside.filePath("secret.txt")),QByteArray("outside-secret"));
    result=perform("readDirectory",{{"path","apps"},{"recursive",true}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(!result[2].toBool());QVERIFY(!result[1].toString().contains("secret.txt"));
    result=perform("grepSearch",{{"mount_prefix","apps"},{"query","outside-secret"}});QCOMPARE(result.size(),4);QVERIFY(result[3].toJsonObject().isEmpty());QVERIFY(!result[2].toBool());QVERIFY(!result[1].toString().contains("outside-secret"));
#endif
}

void CoreTest::fileChangedReentrancy_data() {
    QTest::addColumn<QString>("operation");
    QTest::newRow("write") << QString("write");
    QTest::newRow("multiEdit") << QString("multiEdit");
}

void CoreTest::fileChangedReentrancy() {
    QFETCH(QString, operation);
    QTemporaryDir first, second; QVERIFY(first.isValid()); QVERIFY(second.isValid());
    QVERIFY(writeFile(first.filePath("first.txt"), "before"));
    ToolExecutor executor; QVERIFY(executor.setWorkspace(first.path()));
    QSignalSpy completed(&executor, &ToolExecutor::completed); bool switched = false;
    connect(&executor, &ToolExecutor::fileChanged, this, [&](const QString &, const QByteArray &, const QByteArray &) {
        if (switched) return;
        switched = true; executor.cancel();
        QVERIFY(executor.setWorkspace(second.path()));
        // Reusing the same ID ensures the guard must check generation, not only ID.
        executor.execute("same-id", "write", {{"path", "second.txt"}, {"content", "replacement"}});
    });
    QJsonObject arguments{{"path", "first.txt"}, {"content", "after"}};
    if (operation == "multiEdit") arguments = {{"path", "first.txt"}, {"edits", QJsonArray{QJsonObject{{"old_string", "before"}, {"new_string", "after"}}}}};
    executor.execute("same-id", operation, arguments);
    QTRY_COMPARE(completed.size(), 2);
    QVERIFY(completed[0][2].toBool()); QVERIFY(completed[0][1].toString().contains("cancelled"));
    QVERIFY(!completed[1][2].toBool()); QVERIFY(completed[1][1].toString().contains("second.txt"));
    QCOMPARE(readFile(first.filePath("first.txt")), QByteArray("after"));
    QCOMPARE(readFile(second.filePath("second.txt")), QByteArray("replacement"));
    QVERIFY(!QFileInfo::exists(first.filePath("second.txt")));
}

void CoreTest::fileChangedMayDeleteExecutor() {
    QTemporaryDir root; QPointer<ToolExecutor> executor = new ToolExecutor;
    QVERIFY(executor->setWorkspace(root.path())); int changes = 0, completions = 0;
    connect(executor, &ToolExecutor::completed, this, [&](const QString &, const QString &, bool) { ++completions; });
    connect(executor, &ToolExecutor::fileChanged, this, [&](const QString &, const QByteArray &, const QByteArray &) {
        ++changes; delete executor.data();
    });
    executor->execute("delete-owner", "write", {{"path", "written.txt"}, {"content", "committed"}});
    QTRY_VERIFY(executor.isNull()); QCOMPARE(changes, 1); QCOMPARE(completions, 0);
    QCOMPARE(readFile(root.filePath("written.txt")), QByteArray("committed"));
}

void CoreTest::finalProcessOutputMayReplaceOperation() {
    QTemporaryDir root; ToolExecutor executor; QVERIFY(executor.setWorkspace(root.path()));
    QSignalSpy completed(&executor, &ToolExecutor::completed); bool replaced = false;
    connect(&executor, &ToolExecutor::processOutput, this, [&](const QString &, const QString &) {
        if (replaced) return;
        replaced = true; executor.cancel();
        executor.execute("same-id", "write", {{"path", "replacement.txt"}, {"content", "new operation"}});
    });
#ifdef Q_OS_WIN
    const QString command = "Start-Sleep -Milliseconds 500; [Console]::Out.Write('final bytes')";
#else
    const QString command = "sleep 1; printf 'final bytes'";
#endif
    executor.execute("same-id", "runCommand", {{"command", command}});
    QProcess *process = nullptr;
    QTRY_VERIFY((process = executor.findChild<QProcess *>()) != nullptr);
    // Force the only output callback to occur from the finished handler's drain.
    QVERIFY(QObject::disconnect(process, SIGNAL(readyReadStandardOutput()), &executor, nullptr));
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);
    QVERIFY(replaced); QVERIFY(completed[0][2].toBool()); QVERIFY(!completed[1][2].toBool());
    QCOMPARE(readFile(root.filePath("replacement.txt")), QByteArray("new operation"));
    QVERIFY(completed[1][1].toString().contains("replacement.txt"));
}

void CoreTest::providerStorageAndImport() {
    QTemporaryDir root;
    const QString secret = "a-test-secret-that-must-never-be-plaintext";
    const ProviderConfig provider{"test", "Test", "https://example.invalid/v1", secret, "model", "openai"};
    QString error;
    QVERIFY2(ProviderStore::save(root.filePath("providers.json"), {provider}, &error), qPrintable(error));
    QVERIFY(!readFile(root.filePath("providers.json")).contains(secret.toUtf8()));
    const auto loaded = ProviderStore::load(root.filePath("providers.json"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().apiKey, secret);
    QVERIFY(writeFile(root.filePath("keys.env"), "OPENAI_API_KEY=\"test-openai\"\nUNKNOWN_API_KEY=test-unknown\n"));
    const auto imported = ProviderStore::importKeyFile(root.filePath("keys.env"), &error);
    QCOMPARE(imported.size(), 2);
    QCOMPARE(imported.first().apiKey, QString("test-openai"));
    QVERIFY(imported.last().baseUrl.isEmpty());
    QVERIFY(error.contains("UNKNOWN"));
    QVERIFY(!error.contains("test-unknown"));
}

void CoreTest::streamsAndPersistsConversation() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &) { return Response{textResponse("Hello native Qt")}; };
    AgentEngine engine;
    engine.setProvider(server.provider());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed), text(&engine, &AgentEngine::textDelta);
    engine.submit("Hello");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(text.first().first().toString(), QString("Hello native Qt"));
    QCOMPARE(engine.messages().size(), 2);
    QCOMPARE(engine.messages().last().toObject()["content"].toString(), QString("Hello native Qt"));
    QVERIFY(!engine.isBusy());
    QVERIFY(!server.requests.first().contains("tools"));
    QVERIFY(engine.lastTurnSucceeded());
}

void CoreTest::approvedToolLoop() {
    QTemporaryDir root;
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        return Response{count == 1 ? toolResponse("write", {{"path", "result.txt"}, {"content", "native tool result"}})
                                   : textResponse("File created.")};
    };
    AgentEngine engine;
    engine.setProvider(server.provider()); engine.setWorkspace(root.path());
    QSignalSpy approvals(&engine, &AgentEngine::approvalRequired), done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submit("Create result.txt");
    QTRY_COMPARE(approvals.size(), 1);
    QVERIFY(!QFile::exists(root.filePath("result.txt")));
    engine.approveTool(approvals.first()[0].toString(), true);
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(readFile(root.filePath("result.txt")), QByteArray("native tool result"));
    QCOMPARE(server.requests.size(), 2);
    const auto messages = server.requests.last()["messages"].toArray();
    QCOMPARE(messages.last().toObject()["role"].toString(), QString("tool"));
    QCOMPARE(messages.last().toObject()["tool_call_id"].toString(), QString("call_test"));
    QCOMPARE(engine.messages().size(), 4);
}

void CoreTest::rejectedToolDoesNotWrite() {
    QTemporaryDir root;
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        return Response{count == 1 ? toolResponse("write", {{"path", "denied.txt"}, {"content", "do not write"}})
                                   : textResponse("Action denied.")};
    };
    AgentEngine engine;
    engine.setProvider(server.provider()); engine.setWorkspace(root.path());
    connect(&engine, &AgentEngine::approvalRequired, &engine, [&engine](QString id, QString, QJsonObject) { engine.approveTool(id, false); });
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("Create denied.txt");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QVERIFY(!QFile::exists(root.filePath("denied.txt")));
    QVERIFY(server.requests.last()["messages"].toArray().last().toObject()["content"].toString().contains("denied"));
}

void CoreTest::cancellationSettlesToolCalls() {
    QTemporaryDir root;
    FixtureServer server;
    server.respond = [](int, const QJsonObject &) { return Response{toolResponse("write", {{"path", "never.txt"}, {"content", "never"}})}; };
    AgentEngine engine;
    engine.setProvider(server.provider()); engine.setWorkspace(root.path());
    QSignalSpy approvals(&engine, &AgentEngine::approvalRequired), done(&engine, &AgentEngine::finished);
    engine.submit("Propose file");
    QTRY_COMPARE(approvals.size(), 1);
    engine.cancel();
    QCOMPARE(done.size(), 1);
    QCOMPARE(engine.messages().last().toObject()["role"].toString(), QString("tool"));
    engine.approveTool("call_test", true);
    QTest::qWait(20);
    QVERIFY(!QFile::exists(root.filePath("never.txt")));
    QVERIFY(!engine.isBusy());
}

void CoreTest::cancellationStopsNetwork() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &) { return Response{{}, 200, true}; };
    AgentEngine engine;
    engine.setProvider(server.provider());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submit("Wait");
    QTRY_COMPARE(server.requests.size(), 1);
    engine.cancel();
    QCOMPARE(done.size(), 1);
    QVERIFY(!engine.isBusy());
    QTest::qWait(20);
    QCOMPARE(failed.size(), 0);
}

void CoreTest::incompleteStreamPreservesPartial() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &) {
        const QJsonObject choice{{"delta", QJsonObject{{"content", "Partial"}}}};
        return Response{sseEvent(QJsonObject{{"choices", QJsonArray{choice}}})};
    };
    AgentEngine engine;
    engine.setProvider(server.provider());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submit("Hello");
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(failed.size(), 1);
    QCOMPARE(engine.messages().last().toObject()["content"].toString(), QString("Partial"));
    QVERIFY(!engine.lastTurnSucceeded());
}

void CoreTest::retriesTransientBeforeOutput() {
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        return count == 1 ? Response{"{\"error\":{\"message\":\"busy\"}}", 503, false, "application/json"}
                          : Response{textResponse("Recovered")};
    };
    AgentEngine engine;
    engine.setProvider(server.provider());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submit("Hello");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(server.requests.size(), 2);
    QCOMPARE(failed.size(), 0);
    QCOMPARE(engine.messages().size(), 2);
}

void CoreTest::commandAndCancellation() {
    QTemporaryDir root;
    ToolExecutor tools;
    QVERIFY(tools.setWorkspace(root.path()));
    QSignalSpy done(&tools, &ToolExecutor::completed);
#ifdef Q_OS_WIN
    tools.execute("command", "runCommand", {{"command", "Write-Output 'native-command-ok'"}});
#else
    tools.execute("command", "runCommand", {{"command", "printf native-command-ok"}});
#endif
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
    QVERIFY2(!done.first()[2].toBool(), qPrintable(done.first()[1].toString()));
    QVERIFY(done.first()[1].toString().contains("native-command-ok"));
    done.clear();
#ifdef Q_OS_WIN
    tools.execute("cancel", "runCommand", {{"command", "Start-Sleep -Seconds 30"}});
#else
    tools.execute("cancel", "runCommand", {{"command", "sleep 30"}});
#endif
    QTest::qWait(100);
    tools.cancel();
    QCOMPARE(done.size(), 1);
    QVERIFY(done.first()[2].toBool());
}

void CoreTest::invalidSubmissionSettles() {
    AgentEngine engine;
    const QJsonArray history{QJsonObject{{"role", "user"}, {"content", "Previous turn"}},
                             QJsonObject{{"role", "assistant"}, {"content", "Previous answer"}}};
    engine.setMessages(history);
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed), busy(&engine, &AgentEngine::busyChanged);
    engine.submit("Invalid endpoint");
    QCOMPARE(done.size(), 1);
    QCOMPARE(failed.size(), 1);
    ProviderConfig provider{"test", "Test", "https://example.invalid/v1", "", "", "openai"};
    engine.setProvider(provider);
    engine.submit("Missing model");
    QCOMPARE(done.size(), 2);
    provider.model = "test";
    provider.protocol = "unimplemented";
    engine.setProvider(provider);
    engine.submit("Unsupported protocol");
    QCOMPARE(done.size(), 3);
    QCOMPARE(failed.size(), 3);
    QCOMPARE(busy.size(), 0);
    QCOMPARE(engine.messages(), history);
    QVERIFY(!engine.isBusy());
}

void CoreTest::nativeProtocolRequests() {
    ProviderConfig provider{"vendor", "Vendor", "https://example.invalid/v1", "fixture-key", "model", "anthropic"};
    provider.reasoningEffort = "high"; provider.thinkingMode = "adaptive";
    const QJsonArray history{QJsonObject{{"role", "user"}, {"content", "Hello"}}};
    auto request = ProviderProtocol::prepare(provider, history, "Instructions", ToolExecutor::definitions());
    QCOMPARE(request.request.url().path(), QString("/v1/messages"));
    QCOMPARE(request.request.rawHeader("x-api-key"), QByteArray("fixture-key"));
    QVERIFY(!request.request.hasRawHeader("Authorization"));
    QCOMPARE(request.body["output_config"].toObject()["effort"].toString(), QString("high"));
    QCOMPARE(request.body["thinking"].toObject()["type"].toString(), QString("adaptive"));
    provider.protocol = "google"; provider.baseUrl = "https://example.invalid/v1beta";
    request = ProviderProtocol::prepare(provider, history, "Instructions", ToolExecutor::definitions());
    QCOMPARE(request.request.url().path(), QString("/v1beta/models/model:streamGenerateContent"));
    QVERIFY(request.request.url().query().contains("alt=sse"));
    QCOMPARE(request.request.rawHeader("x-goog-api-key"), QByteArray("fixture-key"));
    QVERIFY(!request.request.url().toString().contains("fixture-key"));
    QVERIFY(request.body["tools"].toArray().first().toObject()["functionDeclarations"].isArray());
    provider.protocol = "openai"; provider.thinkingMode.clear();
    request = ProviderProtocol::prepare(provider, history, "Instructions", {});
    QCOMPARE(request.body["reasoning_effort"].toString(), QString("high"));
    QVERIFY(!request.body.contains("thinking"));
}

void CoreTest::anthropicSignedToolRoundTrip() {
    QTemporaryDir root; QVERIFY(writeFile(root.filePath("fixture.txt"), "native-fixture"));
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        QByteArray body = sseEvent({{"type", "message_start"}, {"message", QJsonObject{{"model", "claude-fixture-actual"}}}});
        if (count == 1) {
            body += sseEvent({{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "thinking"}, {"thinking", ""}}}});
            body += sseEvent({{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "thinking_delta"}, {"thinking", "Inspect fixture"}}}});
            body += sseEvent({{"type", "content_block_delta"}, {"index", 0}, {"delta", QJsonObject{{"type", "signature_delta"}, {"signature", "fixture-signature"}}}});
            body += sseEvent({{"type", "content_block_stop"}, {"index", 0}});
            body += sseEvent({{"type", "content_block_start"}, {"index", 1}, {"content_block", QJsonObject{{"type", "tool_use"}, {"id", "anthropic-call"}, {"name", "read"}, {"input", QJsonObject{}}}}});
            body += sseEvent({{"type", "content_block_delta"}, {"index", 1}, {"delta", QJsonObject{{"type", "input_json_delta"}, {"partial_json", "{\"path\":\"fixture.txt\"}"}}}});
            body += sseEvent({{"type", "content_block_stop"}, {"index", 1}});
        } else body += sseEvent({{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "text"}, {"text", "Verified"}}}});
        body += sseEvent({{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", count == 1 ? "tool_use" : "end_turn"}}}});
        body += sseEvent({{"type", "message_stop"}});
        return Response{body};
    };
    AgentEngine engine; auto provider = server.provider(); provider.protocol = "anthropic";
    engine.setProvider(provider); engine.setWorkspace(root.path());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed), thought(&engine, &AgentEngine::reasoningDelta);
    engine.submit("Read fixture"); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0); QCOMPARE(server.requests.size(), 2); QCOMPARE(thought.size(), 1);
    QCOMPARE(engine.actualModel(), QString("claude-fixture-actual"));
    const auto outgoing = server.requests.last()["messages"].toArray();
    QCOMPARE(outgoing[1].toObject()["content"].toArray()[0].toObject()["signature"].toString(), QString("fixture-signature"));
    QCOMPARE(outgoing.last().toObject()["content"].toArray()[0].toObject()["tool_use_id"].toString(), QString("anthropic-call"));
    QVERIFY(server.headers.first().toLower().contains("x-api-key: fixture-secret"));
}

void CoreTest::googleSignedToolRoundTrip() {
    QTemporaryDir root; QVERIFY(writeFile(root.filePath("fixture.txt"), "native-fixture"));
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        QJsonObject part;
        if (count == 1) part = {{"functionCall", QJsonObject{{"id", "google-call"}, {"name", "read"}, {"args", QJsonObject{{"path", "fixture.txt"}}}}}, {"thoughtSignature", "opaque-fixture-signature"}};
        else part = {{"text", "Verified"}};
        const QJsonObject candidate{{"content", QJsonObject{{"role", "model"}, {"parts", QJsonArray{part}}}}, {"finishReason", "STOP"}};
        return Response{sseEvent({{"candidates", QJsonArray{candidate}}, {"modelVersion", "gemini-fixture-actual"}})};
    };
    AgentEngine engine; auto provider = server.provider(); provider.protocol = "google";
    engine.setProvider(provider); engine.setWorkspace(root.path());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submit("Read fixture"); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0); QCOMPARE(server.requests.size(), 2);
    QCOMPARE(engine.actualModel(), QString("gemini-fixture-actual"));
    const auto contents = server.requests.last()["contents"].toArray();
    QCOMPARE(contents[1].toObject()["parts"].toArray()[0].toObject()["thoughtSignature"].toString(), QString("opaque-fixture-signature"));
    const auto result = contents.last().toObject()["parts"].toArray()[0].toObject()["functionResponse"].toObject();
    QCOMPARE(result["name"].toString(), QString("read")); QCOMPARE(result["id"].toString(), QString("google-call"));
    QVERIFY(server.headers.first().toLower().contains("x-goog-api-key: fixture-secret"));
    QVERIFY(!server.targets.first().contains("fixture-secret"));
}

void CoreTest::modelDiscoveryPagination() {
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        QJsonObject body{{"data", QJsonArray{QJsonObject{{"id", count == 1 ? "model-a" : "model-b"}, {"display_name", "Fixture model"}}}},
            {"has_more", count == 1}, {"last_id", count == 1 ? "model-a" : "model-b"}};
        return Response{QJsonDocument(body).toJson(), 200, false, "application/json"};
    };
    ProviderService service; auto provider = server.provider(); provider.protocol = "anthropic";
    QSignalSpy ready(&service, &ProviderService::modelsReady), failed(&service, &ProviderService::discoveryFailed);
    service.discover(provider); QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    QCOMPARE(failed.size(), 0); QCOMPARE(ready.first()[0].toJsonArray().size(), 2);
    QVERIFY(server.targets.last().contains("after_id=model-a"));
    QVERIFY(!service.isBusy());
}

void CoreTest::providerValidationIsBounded() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &) { return Response{textResponse("OK")}; };
    ProviderService service; QSignalSpy validated(&service, &ProviderService::validationFinished);
    service.test(server.provider()); QTRY_COMPARE_WITH_TIMEOUT(validated.size(), 1, 5000);
    QVERIFY(validated.first()[0].toBool());
    QCOMPARE(server.requests.first()["max_tokens"].toInt(), 64);
    QVERIFY(!server.requests.first().contains("tools"));
}

void CoreTest::externalToolRoundTripAndCancellation() {
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        return Response{count == 1 ? toolResponse("getBrowserTabs", {}) : textResponse("Browser inspected")};
    };
    AgentEngine engine; engine.setProvider(server.provider());
    const QJsonArray definitions{QJsonObject{{"type", "function"}, {"function", QJsonObject{
        {"name", "getBrowserTabs"}, {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}}}}};
    engine.setExternalTools(definitions);
    QSignalSpy requested(&engine, &AgentEngine::externalToolRequested), done(&engine, &AgentEngine::finished);
    engine.submit("Inspect browser"); QTRY_COMPARE(requested.size(), 1);
    const auto token = requested.first()[0].toString(); QVERIFY(token != "call_test");
    engine.completeExternalTool("stale-request", "ignored", false); QVERIFY(engine.isBusy());
    engine.completeExternalTool(token, "[]", false); QTRY_COMPARE(done.size(), 1);
    QCOMPARE(server.requests.last()["messages"].toArray().last().toObject()["content"].toString(), QString("[]"));
    FixtureServer second; second.respond = [](int, const QJsonObject &) { return Response{toolResponse("getBrowserTabs", {})}; };
    engine.setProvider(second.provider()); engine.setMessages({}); requested.clear();
    QSignalSpy cancellation(&engine, &AgentEngine::externalToolCancellationRequested);
    engine.submit("Inspect again"); QTRY_COMPARE(requested.size(), 1); engine.cancel();
    QCOMPARE(cancellation.size(), 1);
    const auto before = engine.messages(); engine.completeExternalTool(requested.first()[0].toString(), "late", false);
    QCOMPARE(engine.messages(), before);
}

void CoreTest::malformedProviderRecordsRejected() {
    QTemporaryDir directory; QString error;
    const auto path = directory.filePath("providers.json");
    const QList<QJsonValue> invalid{QJsonValue("not-an-object"), QJsonObject{},
        QJsonObject{{"id", "x"}, {"name", "X"}, {"baseUrl", ""}, {"model", ""}, {"protocol", "openai"}, {"encryptedApiKey", "%%%"}}};
    for (const auto &record : invalid) {
        QVERIFY(writeFile(path, QJsonDocument(QJsonObject{{"version", 1}, {"providers", QJsonArray{record}}}).toJson()));
        QVERIFY(ProviderStore::load(path, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
}

void CoreTest::verifiedProviderRoutingMetadata() {
    QTemporaryDir directory;
    const auto path = directory.filePath("keys.env");
    QVERIFY(writeFile(path, "NVIDIA_API_KEY=test-nvidia\nYOU_API_KEY=test-you\nSAKANA_API_KEY=test-sakana\nTHEROUTER_API_KEY=test-router\nPORTKEY_API_KEY=test-portkey\nPERPLEXITY_API_KEY=test-perplexity\n"));
    QString error; const auto imported = ProviderStore::importKeyFile(path, &error);
    QCOMPARE(imported.size(), 6);
    const QMap<QString, QString> expected{{"nvidia", "https://integrate.api.nvidia.com/v1"},
        {"you", "https://api.you.com/v1"}, {"sakana", "https://api.sakana.ai/v1"},
        {"therouter", "https://api.therouter.ai/v1"}, {"portkey", ""}, {"perplexity", "https://api.perplexity.ai/v1"}};
    for (const auto &provider : imported) {
        QCOMPARE(provider.baseUrl, expected.value(provider.id));
        QVERIFY(provider.model.isEmpty());
        if (provider.id == "perplexity") QCOMPARE(provider.protocol, QString("openai-responses"));
        QVERIFY(!error.contains(provider.apiKey));
    }
    QVERIFY(error.contains("Discover"));
}

void CoreTest::modelDiscoveryRejectsInvalidResponses() {
    for (int mode = 0; mode < 3; ++mode) {
        FixtureServer server;
        server.respond = [mode](int, const QJsonObject &) {
            if (mode == 0) return Response{"{\"error\":\"fixture-secret\"}", 401, false, "application/json"};
            if (mode == 1) return Response{"{\"unexpected\":[]}", 200, false, "application/json"};
            return Response{"{\"data\":[{\"id\":\"model-a\"}],\"has_more\":true,\"last_id\":\"model-a\"}", 200, false, "application/json"};
        };
        ProviderService service; auto provider = server.provider(); provider.protocol = "anthropic";
        QSignalSpy failed(&service, &ProviderService::discoveryFailed), ready(&service, &ProviderService::modelsReady);
        service.discover(provider); QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
        QCOMPARE(ready.size(), 0); QVERIFY(!service.isBusy());
        QVERIFY(!failed.first()[0].toString().contains(provider.apiKey));
        QCOMPARE(server.requests.size(), mode == 2 ? 2 : 1);
    }
}

void CoreTest::rawReadsPreserveBytesAndRejectTruncation() {
    QTemporaryDir directory;
    const QByteArray content = QByteArray::fromHex("efbbbf") + "first\r\nsecond\n";
    QVERIFY(writeFile(directory.filePath("exact.txt"), content));
    ToolExecutor tools; QVERIFY(tools.setWorkspace(directory.path()));
    QSignalSpy done(&tools, &ToolExecutor::completed);
    tools.execute("raw", "read", {{"path", "exact.txt"}, {"raw", true}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(!done.first()[2].toBool());
    QCOMPARE(done.first()[1].toString().toUtf8(), content);
    done.clear(); QVERIFY(writeFile(directory.filePath("large.txt"), QByteArray(65537, 'x')));
    tools.execute("large", "read", {{"path", "large.txt"}, {"raw", true}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done.first()[2].toBool());
    done.clear(); const QByteArray malformed = QByteArray("alpha") + char(0xff) + "beta";
    QVERIFY(writeFile(directory.filePath("invalid.txt"), malformed));
    tools.execute("edit", "multiEdit", {{"path", "invalid.txt"}, {"edits", QJsonArray{QJsonObject{{"old_string", "alpha"}, {"new_string", "changed"}}}}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done.first()[2].toBool());
    QCOMPARE(readFile(directory.filePath("invalid.txt")), malformed);
    const auto unfinished = QByteArray("alpha") + QByteArray::fromHex("e282");
    QVERIFY(writeFile(directory.filePath("unfinished.txt"), unfinished)); done.clear();
    tools.execute("unfinished-read", "read", {{"path", "unfinished.txt"}, {"raw", true}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done.first()[2].toBool());
    done.clear();
    tools.execute("unfinished-edit", "multiEdit", {{"path", "unfinished.txt"}, {"edits", QJsonArray{QJsonObject{{"old_string", "alpha"}, {"new_string", "changed"}}}}});
    QTRY_COMPARE(done.size(), 1); QVERIFY(done.first()[2].toBool());
    QCOMPARE(readFile(directory.filePath("unfinished.txt")), unfinished);
}

void CoreTest::responsesSignedToolRoundTrip() {
    QTemporaryDir root; QVERIFY(writeFile(root.filePath("fixture.txt"), "native-fixture"));
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        const QJsonObject reasoning{{"type", "reasoning"}, {"id", "reasoning-item"}, {"encrypted_content", "opaque-fixture"},
            {"summary", QJsonArray{QJsonObject{{"type", "summary_text"}, {"text", "Inspect fixture"}}}}};
        QJsonObject call{{"type", "function_call"}, {"id", "function-item"}, {"call_id", "responses-call"}, {"name", "read"}, {"arguments", ""}};
        QJsonArray output;
        QByteArray body = sseEvent({{"type", "response.created"}, {"response", QJsonObject{{"model", "responses-fixture-actual"}}}});
        if (count == 1) {
            body += sseEvent({{"type", "response.reasoning_summary_text.delta"}, {"delta", "Inspect fixture"}});
            body += sseEvent({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", reasoning}});
            body += sseEvent({{"type", "response.output_item.added"}, {"output_index", 1}, {"item", call}});
            body += sseEvent({{"type", "response.function_call_arguments.delta"}, {"output_index", 1}, {"delta", "{\"path\":"}});
            body += sseEvent({{"type", "response.function_call_arguments.delta"}, {"output_index", 1}, {"delta", "\"fixture.txt\"}"}});
            call["arguments"] = "{\"path\":\"fixture.txt\"}";
            body += sseEvent({{"type", "response.function_call_arguments.done"}, {"output_index", 1}, {"arguments", call["arguments"]}});
            body += sseEvent({{"type", "response.output_item.done"}, {"output_index", 1}, {"item", call}});
            output = {reasoning, call};
        } else {
            body += sseEvent({{"type", "response.output_text.delta"}, {"delta", "Veri"}});
            body += sseEvent({{"type", "response.output_text.delta"}, {"delta", "fied"}});
            output = {QJsonObject{{"type", "message"}, {"id", "message-item"}, {"role", "assistant"}, {"status", "completed"},
                {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", "Verified"}, {"annotations", QJsonArray{}}}}}}};
        }
        body += sseEvent({{"type", "response.completed"}, {"response", QJsonObject{{"status", "completed"},
            {"model", "responses-fixture-actual"}, {"output", output}, {"error", QJsonValue::Null}}}});
        return Response{body};
    };
    AgentEngine engine; auto provider = server.provider(); provider.protocol = "openai-responses";
    engine.setProvider(provider); engine.setWorkspace(root.path());
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed), text(&engine, &AgentEngine::textDelta);
    engine.submit("Read fixture"); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
    QCOMPARE(failed.size(), 0); QCOMPARE(server.requests.size(), 2); QCOMPARE(text.size(), 2);
    QCOMPARE(engine.actualModel(), QString("responses-fixture-actual"));
    QCOMPARE(engine.messages().last().toObject()["content"].toString(), QString("Verified"));
    const auto input = server.requests.last()["input"].toArray();
    QCOMPARE(input[1].toObject()["encrypted_content"].toString(), QString("opaque-fixture"));
    QCOMPARE(input[2].toObject()["call_id"].toString(), QString("responses-call"));
    QCOMPARE(input.last().toObject()["type"].toString(), QString("function_call_output"));
    QCOMPARE(input.last().toObject()["call_id"].toString(), QString("responses-call"));
    QVERIFY(input.last().toObject()["output"].toString().contains("native-fixture"));
    QCOMPARE(server.targets.first(), QByteArray("/v1/responses"));
    QVERIFY(!server.requests.first()["store"].toBool(true));
    QVERIFY(server.requests.first()["tools"].toArray().first().toObject()["parameters"].isObject());
    provider.id = "different-provider";
    auto request = ProviderProtocol::prepare(provider, engine.messages(), "Instructions", {});
    QVERIFY(!QJsonDocument(request.body).toJson().contains("opaque-fixture"));
    QVERIFY(QJsonDocument(request.body).toJson().contains("function_call_output"));
    provider.baseUrl = "https://api.openai.com/v1";
    request = ProviderProtocol::prepare(provider, {}, "Instructions", {});
    QVERIFY(request.body["include"].toArray().contains("reasoning.encrypted_content"));
}

void CoreTest::responsesValidationAndFailure() {
    for (int mode = 0; mode < 5; ++mode) {
        FixtureServer server;
        server.respond = [mode](int, const QJsonObject &) {
            const QJsonObject message{{"type", "message"}, {"role", "assistant"},
                {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", "OK"}}}}};
            QJsonObject response{{"status", "completed"}, {"output", QJsonArray{message}}, {"error", QJsonValue::Null}};
            if (mode == 1) {
                response["status"] = "incomplete";
                response["incomplete_details"] = QJsonObject{{"reason", "max_output_tokens"}};
            }
            if (mode == 2) { response["status"] = "failed"; response["error"] = QJsonObject{{"message", "fixture-secret denied"}}; }
            if (mode == 3) return Response{sseEvent({{"type", "response.output_text.delta"}, {"delta", "partial"}})};
            if (mode == 4) return Response{QJsonDocument(response).toJson(), 200, false, "application/json"};
            return Response{sseEvent({{"type", mode == 1 ? "response.incomplete" : mode == 2 ? "response.failed" : "response.completed"}, {"response", response}})};
        };
        ProviderService service; auto provider = server.provider(); provider.protocol = "responses";
        QSignalSpy validated(&service, &ProviderService::validationFinished);
        service.test(provider); QTRY_COMPARE_WITH_TIMEOUT(validated.size(), 1, 5000);
        QCOMPARE(validated.first()[0].toBool(), mode == 0 || mode == 4);
        QVERIFY(!validated.first()[1].toString().contains(provider.apiKey));
        QCOMPARE(server.requests.size(), 1);
        const auto request = server.requests.first();
        QCOMPARE(request["max_output_tokens"].toInt(), 64);
        QVERIFY(request["tools"].isArray()); QVERIFY(request["tools"].toArray().isEmpty());
        QCOMPARE(request["tool_choice"].toString(), QString("none"));
    }
}

void CoreTest::modelFallbackState() {
    ProviderConfig a{"a", "A", "https://example.invalid/v1", "fixture-secret", "model-a", "openai"};
    auto b = a; b.id = "b"; b.model = "model-b";
    QList<ModelConfig> models{{a}, {b}};
    ModelFallback state;
    QCOMPARE(state.resolve("preset", models, 1000), 0); QVERIFY(state.advance(2, 1000));
    QCOMPARE(state.resolve("preset", models, 2000), 1);
    QCOMPARE(state.resolve("preset", models, 302001), 0);
    QVERIFY(state.advance(2, 303000));
    std::reverse(models.begin(), models.end());
    QCOMPARE(state.resolve("preset", models, 304000), 0);
    QVERIFY(state.advance(2, 304001)); models[0].provider.reasoningEffort = "high";
    QCOMPARE(state.resolve("preset", models, 304002), 0);
    QVERIFY(state.advance(2, 304003)); QCOMPARE(state.resolve("other-preset", models, 304004), 0);
    QVERIFY(state.advance(2)); QVERIFY(!state.advance(2)); QCOMPARE(state.index(), 0);
    QVERIFY(state.advance(2)); QCOMPARE(state.resolve({}, models), 0);
}

void CoreTest::modelFallbackRuntime() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &request) {
        if (request["model"] == "primary") return Response{"{\"error\":{\"message\":\"overloaded\"}}", 503, false, "application/json"};
        return Response{textResponse("Fallback response")};
    };
    auto primary = server.provider(); primary.model = "primary";
    auto secondary = primary; secondary.id = "second-instance"; secondary.model = "secondary";
    AgentEngine engine; engine.setModelChain({{primary}, {secondary}}, "preset");
    QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed), selected(&engine, &AgentEngine::effectiveProviderChanged);
    engine.submit("First"); QTRY_COMPARE(done.size(), 1);
    QCOMPARE(failed.size(), 0); QCOMPARE(server.requests.size(), 2); QCOMPARE(engine.provider().id, secondary.id);
    QCOMPARE(selected.last()[2].toInt(), 1);
    engine.setModelChain({{primary}, {secondary}}, "preset");
    engine.submit("Second"); QTRY_COMPARE(done.size(), 2); QCOMPARE(server.requests.size(), 3);
    QCOMPARE(server.requests.last()["model"].toString(), QString("secondary"));
    engine.cancel(); engine.submit("After stop"); QTRY_COMPARE(done.size(), 3); QCOMPARE(server.requests.size(), 5);
    for (int status : {401, 503}) {
        FixtureServer errorServer;
        errorServer.respond = [status](int, const QJsonObject &) { return Response{"{\"error\":{\"message\":\"fixture-secret denied\"}}", status, false, "application/json"}; };
        auto a = errorServer.provider(), b = a; b.model = "fallback";
        engine.setMessages({}); engine.setModelChain({{a}, {b}}, "failure-preset"); done.clear(); failed.clear();
        engine.submit("Fail boundedly"); QTRY_COMPARE(done.size(), 1); QCOMPARE(failed.size(), 1);
        QCOMPARE(errorServer.requests.size(), status == 503 ? 2 : 1);
        QVERIFY(!failed.first()[0].toString().contains("fixture-secret"));
    }
    FixtureServer invalidServer;
    invalidServer.respond = [](int, const QJsonObject &) { return Response{"{}", 503, false, "application/json"}; };
    auto a = invalidServer.provider(), invalid = a; invalid.protocol = "unimplemented";
    engine.setMessages({}); engine.setModelChain({{a}, {invalid}}, "invalid-preset"); done.clear(); failed.clear();
    engine.submit("Reject invalid fallback"); QTRY_COMPARE(done.size(), 1);
    QCOMPARE(failed.size(), 1); QCOMPARE(invalidServer.requests.size(), 1);
}

void CoreTest::utilityFallbackAndCancellation() {
    FixtureServer server;
    server.respond = [](int, const QJsonObject &request) {
        const auto text = request["model"] == "bad-title" ? "No" : "\"**Fix C# parser**.\"";
        return Response{QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"content", text}}}, {"finish_reason", "stop"}}}}}).toJson(), 200, false, "application/json"};
    };
    auto bad = server.provider(); bad.model = "bad-title"; auto good = bad; good.model = "good-title";
    UtilityService service; service.setTitleModels({{bad}, {bad}}); service.setMainModels({{bad}, {good}});
    QSignalSpy title(&service, &UtilityService::titleReady), failed(&service, &UtilityService::failed);
    const QJsonArray history{QJsonObject{{"role", "user"}, {"content", "Fix the C# parser"}}};
    service.generateTitle(history); QTRY_COMPARE(title.size(), 1);
    QCOMPARE(title.first()[0].toString(), QString("Fix C# parser")); QCOMPARE(server.requests.size(), 2); QCOMPARE(failed.size(), 0);
    QVERIFY(!server.requests.first()["stream"].toBool(true)); QCOMPARE(server.requests.first()["max_tokens"].toInt(), 100);
    QVERIFY(!server.requests.first().contains("tools"));
    FixtureServer hanging; hanging.respond = [](int, const QJsonObject &) { return Response{{}, 200, true}; };
    service.setTitleModels({{hanging.provider()}}); service.setMainModels({}); title.clear();
    service.generateTitle(history); QTRY_COMPARE(hanging.requests.size(), 1); service.cancel();
    QVERIFY(!service.isBusy()); QTest::qWait(30); QCOMPARE(title.size(), 0);
    service.setTitleModels({{good}}); service.generateTitle(history); QTRY_COMPARE(title.size(), 1);
    QCOMPARE(failed.size(), 0);
    // An empty successful HTTP object must fail safely, never assert or fabricate a title.
    FixtureServer empty; empty.respond = [](int, const QJsonObject &) { return Response{"{}", 200, false, "application/json"}; };
    service.setTitleModels({{empty.provider()}}); service.generateTitle(history); QTRY_COMPARE(failed.size(), 1);
}

void CoreTest::compressionPreservesHistoryAndTools() {
    const QString briefing = "## Earlier work\nYou fixed the parser and verified the regression. The user requested a follow-up.";
    FixtureServer utility;
    utility.respond = [briefing](int, const QJsonObject &) {
        const QJsonObject message{{"type", "message"}, {"content", QJsonArray{QJsonObject{{"type", "output_text"}, {"text", briefing}}}}};
        return Response{QJsonDocument(QJsonObject{{"status", "completed"}, {"output", QJsonArray{message}}}).toJson(), 200, false, "application/json"};
    };
    QJsonArray history;
    for (int i = 0; i < 8; ++i) {
        history.append(QJsonObject{{"role", "user"}, {"content", QString("Request %1 <untrusted>").arg(i)}});
        history.append(QJsonObject{{"role", "assistant"}, {"content", QString("Answer %1").arg(i)}});
    }
    const int boundary = UtilityService::compressionBoundary(history);
    QVERIFY(boundary > 0); QCOMPARE(history[boundary].toObject()["role"].toString(), QString("user"));
    QJsonArray prefix; for (int i = 0; i < boundary; ++i) prefix.append(history[i]);
    UtilityService service; auto p = utility.provider(); p.protocol = "responses"; service.setMainModels({{p}});
    QSignalSpy compacted(&service, &UtilityService::compressionReady), failed(&service, &UtilityService::failed);
    service.compressHistory(prefix); QTRY_COMPARE(compacted.size(), 1); QCOMPARE(failed.size(), 0);
    QCOMPARE(compacted.first()[0].toString(), briefing);
    QCOMPARE(utility.requests.first()["max_output_tokens"].toInt(), 20000);
    QVERIFY(utility.requests.first()["tools"].toArray().isEmpty());
    QVERIFY(QJsonDocument(utility.requests.first()).toJson().contains("&lt;untrusted&gt;"));
    FixtureServer chat; chat.respond = [](int, const QJsonObject &) { return Response{textResponse("Continued")}; };
    AgentEngine engine; engine.setProvider(chat.provider()); engine.setMessages(history);
    QVERIFY(!engine.applyCompression(boundary + 1, briefing)); QVERIFY(engine.applyCompression(boundary, briefing));
    QCOMPARE(engine.messages().size(), history.size()); QCOMPARE(engine.messages().first(), history.first());
    QSignalSpy done(&engine, &AgentEngine::finished); engine.submit("Continue"); QTRY_COMPARE(done.size(), 1);
    const auto request = chat.requests.first()["messages"].toArray();
    QVERIFY(request[1].toObject()["content"].toString().contains("<memory><![CDATA[" + briefing));
    QVERIFY(!QJsonDocument(chat.requests.first()).toJson().contains("Request 0"));
    QVERIFY(!QJsonDocument(chat.requests.first()).toJson().contains("ade_compressedHistory"));
    QCOMPARE(engine.messages().first(), history.first());
    QVERIFY(UtilityService::compressionBoundary(QJsonArray{history[0], history[1]}) < 0);
}

void CoreTest::partialCancellationDoesNotSucceed() {
    FixtureServer server; server.respond = [](int, const QJsonObject &) { return Response{textResponse("Partial response")}; };
    AgentEngine engine; engine.setProvider(server.provider());
    QSignalSpy done(&engine, &AgentEngine::finished);
    connect(&engine, &AgentEngine::textDelta, &engine, [&engine] { engine.cancel(); });
    engine.submit("Cancel after first text"); QTRY_COMPARE(done.size(), 1);
    QVERIFY(!engine.lastTurnSucceeded()); QVERIFY(!engine.isBusy());
    QCOMPARE(engine.messages().last().toObject()["content"].toString(), QString("Partial response"));
}

void CoreTest::providerSaveRejectsUnreloadableRecords() {
    QTemporaryDir directory; const auto path = directory.filePath("providers.json");
    ProviderConfig provider{"valid", "Valid", "https://example.invalid/v1", "fixture-secret", "model", "openai"};
    QString error; QVERIFY(ProviderStore::save(path, {provider}, &error)); const auto original = readFile(path);
    QList<QList<ProviderConfig>> invalid{{provider, provider}};
    auto empty = provider; empty.id.clear(); invalid.append({empty});
    auto negative = provider; negative.thinkingBudget = -1; invalid.append({negative});
    auto oversized = provider; oversized.name = QString(4 * 1024 * 1024, 'x'); invalid.append({oversized});
    for (const auto &records : invalid) {
        QVERIFY(!ProviderStore::save(path, records, &error)); QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(provider.apiKey)); QCOMPARE(readFile(path), original);
        const auto loaded = ProviderStore::load(path, &error); QCOMPARE(loaded.size(), 1); QVERIFY(error.isEmpty());
        QCOMPARE(loaded.first().apiKey, provider.apiKey);
    }
}

void CoreTest::systemContextAndReadOnlySkillTool() {
    FixtureServer server;
    server.respond = [](int count, const QJsonObject &) {
        return Response{count == 1 ? toolResponse("readContext", {{"path", "plugins/fixture/SKILL.md"}}) : textResponse("Applied scoped context")};
    };
    AgentEngine engine; engine.setProvider(server.provider()); engine.setExternalTools(ContextLoader::toolDefinitions());
    const QString context = "<file path=\"ws1/AGENTS.md\" scope=\"ws1\" respected=\"true\"><![CDATA[Use scoped conventions.]]></file>";
    engine.setSystemContext(context);
    QSignalSpy approval(&engine, &AgentEngine::approvalRequired), requested(&engine, &AgentEngine::externalToolRequested), done(&engine, &AgentEngine::finished);
    connect(&engine, &AgentEngine::externalToolRequested, &engine, [&engine](const QString &id, const QString &, const QJsonObject &) {
        engine.completeExternalTool(id, "Fixture skill text.", false);
    });
    engine.submit("Apply the available skill");
    engine.setSystemContext("Must not replace context during a turn.");
    QTRY_COMPARE(done.size(), 1); QCOMPARE(approval.size(), 0); QCOMPARE(requested.size(), 1);
    for (const auto &request : server.requests) {
        const auto system = request["messages"].toArray().first().toObject()["content"].toString();
        QVERIFY(system.contains("<local_workspace_context>\n" + context));
        QVERIFY(!system.contains("Must not replace"));
        QVERIFY(system.contains("readContext reads enabled local skills"));
        // This host exposes readContext only. Sandbox guidance belongs to
        // requests that actually provide executeSandboxJs.
        QVERIFY(!system.contains("executeSandboxJs"));
        QVERIFY(!system.contains("API.openApp"));
    }
    QVERIFY(engine.lastTurnSucceeded());
}

void CoreTest::toolObserverCancellation_data(){
    QTest::addColumn<bool>("afterResult");QTest::addColumn<bool>("replacement");
    QTest::newRow("started-cancel")<<false<<false;
    QTest::newRow("started-replace")<<false<<true;
    QTest::newRow("finished-cancel")<<true<<false;
    QTest::newRow("finished-replace")<<true<<true;
}
void CoreTest::toolObserverCancellation(){
    QFETCH(bool,afterResult);QFETCH(bool,replacement);QTemporaryDir root;FixtureServer server;
    server.respond=[](int count,const QJsonObject&){return Response{count==1?toolResponse("write",{{"path","controlled.txt"},{"content","CONTROLLED_WRITE"}}):textResponse("Replacement turn finished")};};
    AgentEngine engine;engine.setProvider(server.provider());engine.setWorkspace(root.path());engine.setAlwaysAllow(true);
    QSignalSpy finished(&engine,&AgentEngine::finished);bool acted=false;
    auto cancel=[&]{if(acted)return;acted=true;engine.cancel();if(replacement)engine.submit("A replacement turn");};
    if(afterResult)connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString&,const QString&,bool error){if(!error)cancel();});
    else connect(&engine,&AgentEngine::toolStarted,&engine,[&](const QString&,const QString&,const QJsonObject&){cancel();});
    engine.submit("Request the controlled write");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),replacement?2:1,5000);QTest::qWait(60);
    QVERIFY(acted);QCOMPARE(QFile::exists(root.filePath("controlled.txt")),afterResult);QCOMPARE(server.requests.size(),replacement?2:1);
    int results=0;for(const auto& message:engine.messages())if(message.toObject()["role"]=="tool")++results;
    QCOMPARE(results,1);QVERIFY(!engine.isBusy());if(replacement)QVERIFY(engine.lastTurnSucceeded());
}
void CoreTest::toolObserverDeletion_data(){
    QTest::addColumn<bool>("afterResult");QTest::newRow("started")<<false;QTest::newRow("finished")<<true;
}
void CoreTest::toolObserverDeletion(){
#ifdef Q_OS_WIN
    QFETCH(bool,afterResult);QTemporaryDir root;FixtureServer server;
    server.respond=[](int,const QJsonObject&){return Response{toolResponse("write",{{"path","controlled.txt"},{"content","CONTROLLED_WRITE"}})};};
    SYSTEM_INFO system{};GetSystemInfo(&system);const SIZE_T size=((sizeof(AgentEngine)+system.dwPageSize-1)/system.dwPageSize)*system.dwPageSize;
    void* storage=VirtualAlloc(nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);QVERIFY(storage);
    auto* engine=new(storage)AgentEngine;QPointer<AgentEngine> alive(engine);bool protectedStorage=false;
    const auto cleanup=qScopeGuard([&]{if(alive)engine->~AgentEngine();VirtualFree(storage,0,MEM_RELEASE);});
    engine->setProvider(server.provider());engine->setWorkspace(root.path());engine->setAlwaysAllow(true);
    auto destroy=[&]{if(!alive)return;engine->~AgentEngine();DWORD previous=0;protectedStorage=VirtualProtect(storage,size,PAGE_NOACCESS,&previous);};
    if(afterResult)connect(engine,&AgentEngine::toolFinished,this,[&](const QString&,const QString&,bool error){if(!error)destroy();});
    else connect(engine,&AgentEngine::toolStarted,this,[&](const QString&,const QString&,const QJsonObject&){destroy();});
    engine->submit("Request the controlled write");QTRY_VERIFY_WITH_TIMEOUT(!alive,5000);QVERIFY(protectedStorage);QTest::qWait(60);
    QCOMPARE(QFile::exists(root.filePath("controlled.txt")),afterResult);QCOMPARE(server.requests.size(),1);
#else
    QSKIP("Windows protected-storage observer fixture.");
#endif
}
void CoreTest::cancellationFromResultSettlesWholeToolBlockBeforeReplacement(){
    QTemporaryDir root;FixtureServer server;
    server.respond=[](int count,const QJsonObject&){
        if(count>1)return Response{textResponse("Replacement completed")};
        QJsonArray calls;for(int i=0;i<2;++i){
            const QJsonObject arguments{{"path",QString("controlled-%1.txt").arg(i)},{"content","fixture"}};
            calls.append(QJsonObject{{"index",i},{"id",QString("call-%1").arg(i)},{"type","function"},{"function",QJsonObject{{"name","write"},{"arguments",QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}});
        }
        return Response{sseEvent({{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"tool_calls",calls}}},{"finish_reason","tool_calls"}}}}})+"data: [DONE]\n\n"};
    };
    AgentEngine engine;engine.setProvider(server.provider());engine.setWorkspace(root.path());engine.setAlwaysAllow(true);bool acted=false;
    QSignalSpy done(&engine,&AgentEngine::finished);
    connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString& id,const QString&,bool error){
        if(id!="call-0"||error||acted)return;
        acted=true;engine.cancel();engine.submit("Continue safely in the replacement");
    });
    engine.submit("Request two controlled writes");QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,5000);
    QVERIFY(QFile::exists(root.filePath("controlled-0.txt")));QVERIFY(!QFile::exists(root.filePath("controlled-1.txt")));QCOMPARE(server.requests.size(),2);
    QMap<QString,int> counts;for(const auto& item:server.requests.last()["messages"].toArray()){
        const auto message=item.toObject();if(message["role"]=="tool")++counts[message["tool_call_id"].toString()];
    }
    QCOMPARE(counts.value("call-0"),1);QCOMPARE(counts.value("call-1"),1);QVERIFY(engine.lastTurnSucceeded());
}
QTEST_GUILESS_MAIN(CoreTest)
#include "test_core.moc"
