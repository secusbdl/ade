#include "core/ProviderStore.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace ade;
namespace {
const QString PluginToken = "SYNTHETIC_SMOKE_GITHUB_TOKEN_38fc2a";
const QString ModelToken = "SYNTHETIC_SMOKE_MODEL_TOKEN_6ba81e";
bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QByteArray sse(const QJsonObject& choice) {
    return "data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}}).toJson(QJsonDocument::Compact)
        + "\n\ndata: [DONE]\n\n";
}
QJsonObject call(int index, const QString& id, const QString& name, const QJsonObject& arguments) {
    const QJsonObject function{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}};
    return {{"index", index}, {"id", id}, {"type", "function"}, {"function", function}};
}
class Model final : public QTcpServer {
public:
    QList<QJsonObject> requests;
    Model() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto& line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append(QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
                    QByteArray response;
                    if (requests.size() == 1) {
                        const QJsonArray calls{
                            call(0, "smoke-context", "readContext", {{"path", "plugins/github/SKILL.md"}}),
                            call(1, "smoke-credential", "executeSandboxJs", {
                                {"explanation", "Inspect only the synthetic credential configured state; no service request"},
                                {"script", "const cred=await API.getCredential('github-pat');return {configured:!!(cred&&typeof cred.token==='string'&&cred.token.length>0)};"}})};
                        response = sse({{"delta", QJsonObject{{"tool_calls", calls}}}, {"finish_reason", "tool_calls"}});
                    } else {
                        response = sse({{"delta", QJsonObject{{"content", "SMOKE_PLUGIN_FIXTURE_DONE"}}}, {"finish_reason", "stop"}});
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                                  + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ProviderConfig provider() const {
        return {"fixture", "Loopback fixture", QString("http://127.0.0.1:%1/v1").arg(serverPort()),
                ModelToken, "synthetic-model", "openai"};
    }
};
class SmokeProcess final : public QProcess {
public:
    ~SmokeProcess() override { if (state() != NotRunning) { kill(); waitForFinished(3000); } }
    void launch(const QString& directory, const QString& prompt = "Inspect the GitHub plugin fixture only.") {
        auto environment = QProcessEnvironment::systemEnvironment();
        // ADE imports the native Windows platform. The smoke path creates no
        // MainWindow, so a test-only platform override is neither needed nor supported.
        environment.remove("QT_QPA_PLATFORM");
        setProcessEnvironment(environment);
#ifdef Q_OS_WIN
        setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
            arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            arguments->startupInfo->wShowWindow = SW_HIDE;
        });
#endif
        const auto executable = qEnvironmentVariable("ADE_TEST_EXECUTABLE",
            QCoreApplication::applicationDirPath() + "/ADE.exe");
        start(executable, {"--smoke", "--data-dir", directory, "--provider", "fixture", "--always-allow",
                           "--timeout", "15", "--prompt", prompt, "--report", directory + "/report.json"});
    }
};
bool profile(const QString& directory, const Model& model, bool credential, QString* error) {
    if (!ProviderStore::save(directory + "/providers.json", {model.provider()}, error)) return false;
    if (!credential) return true;
    ProviderConfig plugin;
    plugin.id = "github"; plugin.name = "Synthetic GitHub"; plugin.protocol = "credential"; plugin.apiKey = PluginToken;
    return ProviderStore::save(directory + "/plugin-credentials.json", {plugin}, error);
}
}

class SmokePluginsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY(QFileInfo::exists(qEnvironmentVariable("ADE_TEST_EXECUTABLE", QCoreApplication::applicationDirPath() + "/ADE.exe")));
    }
    void actualProcessLoadsPluginContextAndCredential_data() {
        QTest::addColumn<int>("enabledFlag");
        QTest::addColumn<bool>("credential");
        QTest::newRow("default-enabled") << -1 << true;
        QTest::newRow("saved-enabled") << 1 << true;
        QTest::newRow("saved-disabled") << 0 << true;
        QTest::newRow("missing-credential") << -1 << false;
    }
    void actualProcessLoadsPluginContextAndCredential() {
        QFETCH(int, enabledFlag); QFETCH(bool, credential);
        Model model; QTemporaryDir data; QString error;
        QVERIFY(model.isListening()); QVERIFY(data.isValid()); QVERIFY2(profile(data.path(), model, credential, &error), qPrintable(error));
        if (enabledFlag >= 0) {
            const QJsonObject preferences{{"plugins", QJsonObject{{"github", QJsonObject{{"enabled", enabledFlag != 0}}}}}};
            QVERIFY(writeFile(data.filePath("preferences.json"), QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"preferences", preferences}}).toJson()));
        }
        const auto encrypted = readFile(data.filePath("plugin-credentials.json"));
        QVERIFY(!encrypted.contains(PluginToken.toUtf8()));
        SmokeProcess process; process.launch(data.path()); QVERIFY(process.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), QProcess::NotRunning, 20000);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 0);
        QCOMPARE(model.requests.size(), 2);
        const auto firstWire = QJsonDocument(model.requests.first()).toJson();
        QCOMPARE(firstWire.contains("plugins/github/SKILL.md"), enabledFlag != 0);
        QVERIFY(firstWire.contains("readContext"));
        const auto reportBytes = readFile(data.filePath("report.json"));
        const auto report = QJsonDocument::fromJson(reportBytes).object();
        QVERIFY(report["success"].toBool()); QCOMPARE(report["response"].toString(), QString("SMOKE_PLUGIN_FIXTURE_DONE"));
        int toolResults = 0; QString contextResult, credentialResult;
        for (const auto& value : report["messages"].toArray()) {
            const auto message = value.toObject();
            if (message["role"] != "tool") continue;
            ++toolResults;
            if (message["tool_call_id"] == "smoke-context") contextResult = message["content"].toString();
            if (message["tool_call_id"] == "smoke-credential") credentialResult = message["content"].toString();
        }
        QCOMPARE(toolResults, 2);
        QCOMPARE(contextResult.contains("# GitHub Plugin"), enabledFlag != 0);
        QJsonParseError envelopeError;
        const auto envelope = QJsonDocument::fromJson(credentialResult.toUtf8(), &envelopeError);
        QCOMPARE(envelopeError.error, QJsonParseError::NoError);
        QVERIFY(envelope.isObject());
        QVERIFY(envelope.object()["result"].isString());
        QJsonParseError resultError;
        const auto result = QJsonDocument::fromJson(envelope.object()["result"].toString().toUtf8(), &resultError);
        QCOMPARE(resultError.error, QJsonParseError::NoError);
        QVERIFY(result.isObject());
        QVERIFY(result.object()["configured"].isBool());
        QCOMPARE(result.object()["configured"].toBool(), credential && enabledFlag != 0);
        QByteArray exposed = reportBytes + process.readAllStandardOutput() + process.readAllStandardError();
        for (const auto& request : model.requests) exposed += QJsonDocument(request).toJson();
        QVERIFY(!exposed.contains(PluginToken.toUtf8())); QVERIFY(!exposed.contains(ModelToken.toUtf8()));
        QVERIFY(!reportBytes.contains("{{CRED:"));
        if (enabledFlag != 0) QVERIFY(QJsonDocument(model.requests.last()).toJson().contains("# GitHub Plugin"));
    }
    void invalidConfigurationFailsBeforeProviderRequest_data() {
        QTest::addColumn<QString>("file"); QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("malformed-encrypted-store") << QString("plugin-credentials.json") << QByteArray("{invalid");
        QTest::newRow("malformed-preferences") << QString("preferences.json") << QByteArray("{invalid");
        QTest::newRow("invalid-enabled-type") << QString("preferences.json")
            << QByteArray(R"({"schemaVersion":1,"preferences":{"plugins":{"github":{"enabled":"true"}}}})");
        QTest::newRow("oversized-preferences") << QString("preferences.json") << QByteArray(2 * 1024 * 1024 + 1, ' ');
    }
    void invalidConfigurationFailsBeforeProviderRequest() {
        QFETCH(QString, file); QFETCH(QByteArray, bytes);
        Model model; QTemporaryDir data; QString error;
        QVERIFY(model.isListening()); QVERIFY(data.isValid()); QVERIFY2(profile(data.path(), model, true, &error), qPrintable(error));
        QVERIFY(writeFile(data.filePath(file), bytes));
        SmokeProcess process; process.launch(data.path()); QVERIFY(process.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), QProcess::NotRunning, 10000);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit); QCOMPARE(process.exitCode(), 2);
        QVERIFY(model.requests.isEmpty()); QVERIFY(!QFileInfo::exists(data.filePath("report.json")));
        const auto output = process.readAllStandardOutput() + process.readAllStandardError();
        QVERIFY(output.contains("Cannot configure smoke plugins:"));
        QVERIFY(!output.contains(PluginToken.toUtf8())); QVERIFY(!output.contains(ModelToken.toUtf8()));
        QCOMPARE(readFile(data.filePath(file)), bytes);
    }
};
QTEST_GUILESS_MAIN(SmokePluginsTest)
#include "test_smoke_plugins.moc"
