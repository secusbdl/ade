#include "attachments/AttachmentPipeline.h"
#include "attachments/ModelMediaCatalog.h"
#include "core/AgentEngine.h"
#include "core/ProviderProtocol.h"
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

using namespace ade;
namespace {
bool writeFile(const QString &path, const QByteArray &bytes) { QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(); }
QByteArray imageBytes() { QImage image(400, 200, QImage::Format_ARGB32); image.fill(QColor(40, 120, 230, 130)); QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes; }
QJsonObject userMessage(const QString &text, const QJsonObject &metadata) { return {{"role", "user"}, {"content", text}, {"metadata", metadata}}; }
void verifyStoredUserContext(const QJsonObject& message,const QJsonObject& expected,const QString& text){
    QCOMPARE(message["role"].toString(),QString("user"));QCOMPARE(message["content"].toString(),text);
    auto context=message["metadata"].toObject();const auto display=context.take("display").toObject();
    // The complete captured context still matches byte-for-byte at the JSON
    // value level; presentation is a separate, versioned addition.
    QCOMPARE(context,expected);QCOMPARE(display["version"].toInt(),1);QVERIFY(!display["id"].toString().isEmpty());
    const auto parts=display["parts"].toArray();QCOMPARE(parts.size(),1);
    QCOMPARE(parts[0].toObject(),QJsonObject({{"id",display["id"].toString()+"/0"},{"type","text"},{"text",text},{"state","done"}}));
    const auto projected=AgentEngine::displayHistory(QJsonArray{message});QCOMPARE(projected.size(),1);
    QCOMPARE(projected[0].toObject()["id"],display["id"]);QCOMPARE(projected[0].toObject()["parts"].toArray(),parts);
    QCOMPARE(projected[0].toObject()["attachments"].toArray(),expected["attachments"].toArray());
}
class ProviderFixture : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> bodies;
    QString protocol = "openai";
    bool overloadFirst = false;
    bool hold = false;
    ProviderFixture() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection(); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("done").toBool()) return;
                    auto bytes = socket->property("bytes").toByteArray() + socket->readAll(); socket->setProperty("bytes", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n"); if (end < 0) return;
                    int length = 0; for (const auto &line : bytes.left(end).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("done", true); bodies.append(QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
                    if (hold) return;
                    QByteArray response;
                    if (overloadFirst && bodies.size() == 1) response = "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
                    else {
                        QByteArray payload;
                        if (protocol == "anthropic") payload = R"({"id":"fixture","type":"message","model":"fixture-model","content":[{"type":"text","text":"done"}],"stop_reason":"end_turn"})";
                        else if (protocol == "google") payload = R"({"modelVersion":"fixture-model","candidates":[{"content":{"parts":[{"text":"done"}]},"finishReason":"STOP"}]})";
                        else if (protocol == "responses") payload = R"({"id":"fixture","status":"completed","model":"fixture-model","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"done"}]}]})";
                        else payload = R"({"model":"fixture-model","choices":[{"message":{"role":"assistant","content":"done"},"finish_reason":"stop"}]})";
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
                    }
                    socket->write(response); socket->disconnectFromHost();
                });
            }
        });
    }
    ProviderConfig provider() const { ProviderConfig result; result.id = "fixture"; result.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort()); result.model = "fixture-model"; result.protocol = protocol; return result; }
};
}
class AttachmentTest : public QObject {
    Q_OBJECT
private slots:
    void originalsReferencesCloneAndDelete();
    void scopeAndJunctions();
    void textSnapshotsAndBinaryFallback();
    void modelImageConstraintsAndFourSerializers();
    void toolMediaPreservesToolPairsAndSignedHistory();
    void requestBoundsAndMalformedMetadata();
    void pinnedModelCapabilities();
    void snapshotsPersistAfterDeletionAndStayAgentScoped();
    void nativeEngineImageRequest_data();
    void nativeEngineImageRequest();
    void fallbackRebuildsFromOriginalReferences();
    void preparationFailureAndCancellationKeepOriginalHistory();
    void structuredTextPreparationAndCapturedMetadata();
    void malformedCapturedMetadata_data();
    void malformedCapturedMetadata();
};

void AttachmentTest::originalsReferencesCloneAndDelete() {
    QTemporaryDir data, outside, workspace; AttachmentStore store(data.path()); QString error;
    const QByteArray binary = QByteArray::fromHex("00ff000102e282"); const auto source = outside.path() + "/Selected File.bin";
    QVERIFY(writeFile(source, binary)); const auto blob = store.importFile("chat-a", source, {}, &error); QVERIFY2(!blob.isEmpty(), qPrintable(error));
    QVERIFY(blob.path.startsWith("att/selected_f_")); QCOMPARE(blob.originalFileName, QString("Selected File.bin"));
    QCOMPARE(store.read("chat-a", blob, {}, &error), binary); QVERIFY(error.isEmpty());
    QVERIFY(writeFile(workspace.path() + "/source.txt", "workspace source"));
    const AttachmentMounts mounts{{"ws1", workspace.path()}};
    const auto reference = store.importFile("chat-a", workspace.path() + "/source.txt", mounts, &error);
    QCOMPARE(reference.path, QString("ws1/source.txt")); QVERIFY(reference.originalFileName.isEmpty());
    QVERIFY(store.cloneAgent("chat-a", "chat-b", &error));
    QVERIFY(!store.cloneAgent("chat-a", "chat-b", &error));
    AttachmentStore reopened(data.path()); QCOMPARE(reopened.read("chat-b", blob, {}, &error), binary);
    QVERIFY(reopened.removeAgent("chat-a", &error)); QCOMPARE(reopened.read("chat-b", blob, {}, &error), binary);
    QFile original(source); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), binary);
    QVERIFY(reopened.removeAgent("chat-b", &error)); QVERIFY(!QFileInfo::exists(data.path() + "/attachments/chat-b"));
}
void AttachmentTest::scopeAndJunctions() {
    QTemporaryDir data, workspace, outside; AttachmentStore store(data.path()); QString error;
    const auto reference = store.write("chat-a", "safe.txt", "private fixture", &error); QVERIFY(!reference.isEmpty());
    QVERIFY(store.read("chat-b", reference, {}, &error).isEmpty()); QVERIFY(!error.isEmpty());
    for (const auto &path : QStringList{"att/../secret", "att/C:secret", "att/sub/file", "ws1/../../secret", "C:/outside"}) {
        QVERIFY(store.resolve("chat-a", {path, {}}, {{"ws1", workspace.path()}}, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    QVERIFY(store.agentDirectory("../chat", &error, true).isEmpty());
#ifdef Q_OS_WIN
    const auto link = workspace.path() + "/escape"; QProcess process;
    process.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(outside.path())});
    QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 0); QVERIFY(writeFile(outside.path() + "/secret.txt", "outside fixture"));
    const auto resolved = store.resolve("chat-a", {"ws1/escape/secret.txt", {}}, {{"ws1", workspace.path()}}, &error);
    QVERIFY(QDir().rmdir(link)); QVERIFY(resolved.isEmpty()); QVERIFY(!error.isEmpty());
#endif
}
void AttachmentTest::textSnapshotsAndBinaryFallback() {
    QTemporaryDir data, workspace; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    QByteArray lines; for (int i = 1; i <= 40; ++i) lines += QByteArray::number(i) + " original\n";
    QVERIFY(writeFile(workspace.path() + "/file.txt", lines)); const AttachmentMounts mounts{{"ws1", workspace.path()}};
    const auto workspaceRef = store.importFile("chat-a", workspace.path() + "/file.txt", mounts, &error);
    const auto uploaded = store.write("chat-a", "clip.textclip", lines, &error);
    const auto invalidUtf8 = store.write("chat-a", "bad.txt", QByteArray::fromHex("41e282"), &error);
    const auto pdf = store.write("chat-a", "file.pdf", "%PDF-1.7 native parser pending", &error);
    QJsonArray refs{workspaceRef.toJson(), uploaded.toJson(), invalidUtf8.toJson(), pdf.toJson()};
    const auto captured = pipeline.capture("chat-a", refs, mounts, &error); QVERIFY(error.isEmpty());
    const QJsonArray history{userMessage("review", captured)}; const auto immutable = QJsonDocument(history).toJson();
    QVERIFY(writeFile(workspace.path() + "/file.txt", "changed workspace content"));
    const auto prepared = pipeline.prepare(history, "chat-a", mounts, {}, {}, &error); QVERIFY(error.isEmpty());
    const auto encoded = QJsonDocument(prepared).toJson(); QVERIFY(encoded.contains("30|30 original")); QVERIFY(encoded.contains("40|40 original"));
    QVERIFY(!encoded.contains("changed workspace content")); QVERIFY(encoded.contains("Binary file")); QVERIFY(encoded.contains("outside the structured-text slice"));
    QCOMPARE(QJsonDocument(history).toJson(), immutable);
    AttachmentPipeline reopened(store); const auto afterRestart = reopened.prepare(history, "chat-a", mounts, {}, {}, &error);
    QVERIFY(QJsonDocument(afterRestart).toJson().contains("30|30 original"));
    QVERIFY(!QJsonDocument(afterRestart).toJson().contains("changed workspace content"));
}
void AttachmentTest::pinnedModelCapabilities() {
    QCOMPARE(ModelMediaCatalog::modelIds().size(), 54);
    QVERIFY(!ModelMediaCatalog::resolve("unknown-custom-image-name").image);
    QVERIFY(!ModelMediaCatalog::resolve("deepseek-v4-pro").image);
    QVERIFY(!ModelMediaCatalog::resolve("default").image);
    QVERIFY(ModelMediaCatalog::resolve("quick").image);
    const auto anthropic = ModelMediaCatalog::resolve("claude-sonnet-4.6"); QVERIFY(anthropic.image);
    QCOMPARE(anthropic.maxPixels, qint64(1200000)); QCOMPARE(anthropic.maxWidth, 8000);
    const auto google = ModelMediaCatalog::resolve("models/gemini-3.1-flash-lite"); QVERIFY(google.image); QCOMPARE(google.maxPixels, qint64(2500000));
    QCOMPARE(ModelMediaCatalog::resolve("gpt-5.4").maxPixels, qint64(2560000));
}
void AttachmentTest::snapshotsPersistAfterDeletionAndStayAgentScoped() {
    QTemporaryDir data, workspace; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    QVERIFY(writeFile(workspace.path() + "/original.txt", "original history content")); const AttachmentMounts mounts{{"ws1", workspace.path()}};
    const auto reference = store.importFile("chat-a", workspace.path() + "/original.txt", mounts, &error);
    const auto metadata = pipeline.capture("chat-a", {reference.toJson()}, mounts, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto digest = metadata["pathReferences"].toObject()[reference.path].toString();
    QVERIFY(QFile::remove(workspace.path() + "/original.txt"));
    AttachmentPipeline reopened(store); const QJsonArray history{userMessage("review", metadata)};
    QVERIFY(QJsonDocument(reopened.prepare(history, "chat-a", mounts, {}, {}, &error)).toJson().contains("original history content"));
    QVERIFY(store.readSnapshot("chat-b", reference, digest, &error).isEmpty()); QVERIFY(!error.isEmpty());
    QVERIFY(store.readSnapshot("chat-a", {"ws1/uncaptured.txt", {}}, digest, &error).isEmpty());
    QVERIFY(store.cloneAgent("chat-a", "chat-b", &error)); QVERIFY(store.removeAgent("chat-a", &error));
    AttachmentPipeline forked(store); QVERIFY(QJsonDocument(forked.prepare(history, "chat-b", mounts, {}, {}, &error)).toJson().contains("original history content"));
    const auto snapshots = QDir(data.path() + "/attachment-snapshots/chat-b").entryList({"*.bin"}, QDir::Files);
    QCOMPARE(snapshots.size(), 1); QVERIFY(writeFile(data.path() + "/attachment-snapshots/chat-b/" + snapshots.first(), "corrupt injected content"));
    AttachmentPipeline corrupted(store); const auto output = QJsonDocument(corrupted.prepare(history, "chat-b", mounts, {}, {}, &error)).toJson();
    QVERIFY(!output.contains("corrupt injected content")); QVERIFY(output.contains("could not be included"));
    QVERIFY(store.removeAgent("chat-b", &error)); QVERIFY(!QFileInfo::exists(data.path() + "/attachment-snapshots/chat-b"));
    const auto empty = store.write("chat-empty", "empty.txt", {}, &error);
    const auto emptyMetadata = pipeline.capture("chat-empty", {empty.toJson()}, {}, &error); QVERIFY(error.isEmpty());
    QVERIFY(QFile::remove(store.resolve("chat-empty", empty, {}, &error)));
    AttachmentPipeline emptyReopened(store); const auto emptyOutput = QJsonDocument(emptyReopened.prepare({userMessage("empty file", emptyMetadata)}, "chat-empty", {}, {}, {}, &error)).toJson();
    QVERIFY(!emptyOutput.contains("could not be included"));
}
void AttachmentTest::modelImageConstraintsAndFourSerializers() {
    QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto image = store.write("chat-a", "screenshot.png", imageBytes(), &error);
    const auto captured = pipeline.capture("chat-a", {image.toJson()}, {}, &error); const QJsonArray history{userMessage("What is shown?", captured)};
    ModelMediaCapabilities caps; caps.image = true; caps.imageMimeTypes = {"image/png"}; caps.maxWidth = 100; caps.maxHeight = 100;
    const auto prepared = pipeline.prepare(history, "chat-a", {}, caps, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    QJsonObject media; for (const auto &part : prepared[0].toObject()["content"].toArray()) if (part.toObject()["type"] == "image") media = part.toObject();
    QVERIFY(!media.isEmpty()); QImage scaled; QVERIFY(scaled.loadFromData(QByteArray::fromBase64(media["dataBase64"].toString().toLatin1())));
    QCOMPARE(scaled.size(), QSize(100, 50));
    for (const auto &protocol : QStringList{"openai", "responses", "anthropic", "google"}) {
        ProviderConfig provider; provider.baseUrl = "https://fixture.invalid/v1"; provider.model = "fixture"; provider.protocol = protocol;
        const auto body = ProviderProtocol::prepare(provider, prepared, "system", {}).body;
        const auto bytes = QJsonDocument(body).toJson(); QVERIFY(!bytes.contains("\"metadata\"")); QVERIFY(!bytes.contains("pathReferences"));
        QVERIFY(bytes.contains(media["dataBase64"].toString().toLatin1()));
        QVERIFY(bytes.contains(protocol == "google" ? "inlineData" : protocol == "anthropic" ? "media_type" : "image_url"));
    }
    caps.image = false; const auto textOnly = QJsonDocument(pipeline.prepare(history, "chat-a", {}, caps, {}, &error)).toJson();
    QVERIFY(textOnly.contains("Image content not available")); QVERIFY(!textOnly.contains("dataBase64"));
    caps.image = true; caps.maxImageBytes = 1; QVERIFY(QJsonDocument(pipeline.prepare(history, "chat-a", {}, caps, {}, &error)).toJson().contains("cannot fit"));
}
void AttachmentTest::toolMediaPreservesToolPairsAndSignedHistory() {
    QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto file = store.write("chat-a", "tool-image.png", imageBytes(), &error); const auto captured = pipeline.capture("chat-a", {file.toJson()}, {}, &error);
    const QJsonObject function{{"name", "fixture"}, {"arguments", "{}"}};
    const QJsonArray calls{QJsonObject{{"id", "call-a"}, {"type", "function"}, {"function", function}}, QJsonObject{{"id", "call-b"}, {"type", "function"}, {"function", function}}};
    const QJsonArray native{QJsonObject{{"type", "thinking"}, {"thinking", "fixture reasoning"}, {"signature", "opaque-signature"}}};
    const QJsonArray history{QJsonObject{{"role", "assistant"}, {"content", ""}, {"tool_calls", calls}, {"ade_native", QJsonObject{{"protocol", "anthropic"}, {"providerId", "p"}, {"model", "m"}, {"content", native}}}},
        QJsonObject{{"role", "tool"}, {"tool_call_id", "call-a"}, {"content", "created image"}, {"metadata", captured}},
        QJsonObject{{"role", "tool"}, {"tool_call_id", "call-b"}, {"content", "done"}}};
    ModelMediaCapabilities caps; caps.image = true; const auto prepared = pipeline.prepare(history, "chat-a", {}, caps, {}, &error);
    QCOMPARE(prepared.size(), 4); QCOMPARE(prepared[1].toObject()["tool_call_id"].toString(), QString("call-a"));
    QCOMPARE(prepared[2].toObject()["tool_call_id"].toString(), QString("call-b")); QCOMPARE(prepared[3].toObject()["role"].toString(), QString("user"));
    QCOMPARE(prepared[0], history[0]);
    ProviderConfig provider; provider.id = "p"; provider.model = "m"; provider.baseUrl = "https://fixture.invalid/v1"; provider.protocol = "anthropic";
    QVERIFY(QJsonDocument(ProviderProtocol::prepare(provider, prepared, "system", {}).body).toJson().contains("opaque-signature"));
}
void AttachmentTest::requestBoundsAndMalformedMetadata() {
    QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    QVERIFY(pipeline.capture("chat-a", {QJsonObject{{"path", 5}}}, {}, &error).isEmpty()); QVERIFY(!error.isEmpty());
    const auto image = store.write("chat-a", "image.png", imageBytes(), &error);
    const auto captured = pipeline.capture("chat-a", {image.toJson()}, {}, &error);
    ModelMediaCapabilities caps; caps.image = true; AttachmentLimits limits; limits.maxRequestImageBytes = 1;
    QVERIFY(pipeline.prepare({userMessage("test", captured)}, "chat-a", {}, caps, limits, &error).isEmpty()); QVERIFY(error.contains("budget"));
    limits.maxRequestImageBytes = 1024 * 1024; limits.maxTextChars = 0;
    QVERIFY(pipeline.prepare({userMessage("test", captured)}, "chat-a", {}, caps, limits, &error).isEmpty()); QVERIFY(error.contains("limits"));
}
void AttachmentTest::nativeEngineImageRequest_data() {
    QTest::addColumn<QString>("protocol"); for (const auto &value : QStringList{"openai", "responses", "anthropic", "google"}) QTest::newRow(qPrintable(value)) << value;
}
void AttachmentTest::nativeEngineImageRequest() {
    QFETCH(QString, protocol); ProviderFixture server; server.protocol = protocol;
    QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto file = store.write("chat-a", "input.png", imageBytes(), &error); const auto metadata = pipeline.capture("chat-a", {file.toJson()}, {}, &error);
    AgentEngine engine; engine.setProvider(server.provider()); engine.setMessagePreparer([&](const QJsonArray &history, const ProviderConfig &, QString *failure) {
        ModelMediaCapabilities caps; caps.image = true; return pipeline.prepare(history, "chat-a", {}, caps, {}, failure);
    });
    QSignalSpy finished(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.submitWithAttachments("", metadata["attachments"].toArray(), metadata["pathReferences"].toObject(), metadata["pathMetadata"].toObject());
    QTRY_COMPARE(finished.size(), 1); QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString())); QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(server.bodies.size(), 1); const auto request = QJsonDocument(server.bodies[0]).toJson(); QVERIFY(request.contains(protocol == "google" ? "inlineData" : protocol == "anthropic" ? "media_type" : "image_url"));
    verifyStoredUserContext(engine.messages()[0].toObject(),metadata,"");
    QVERIFY(!request.contains("\"metadata\""));QVERIFY(!request.contains("\"display\""));
    QVERIFY(!request.contains(engine.messages()[0].toObject()["metadata"].toObject()["display"].toObject()["id"].toString().toUtf8()));
    QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("dataBase64"));
}
void AttachmentTest::fallbackRebuildsFromOriginalReferences() {
    ProviderFixture server; server.overloadFirst = true; QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto file = store.write("chat-a", "input.png", imageBytes(), &error); const auto metadata = pipeline.capture("chat-a", {file.toJson()}, {}, &error);
    auto first = server.provider(), second = first; first.model = "image-capable"; second.model = "text-only";
    AgentEngine engine; engine.setModelChain({ModelConfig{first}, ModelConfig{second}}, "fixture-preset"); QStringList models;
    engine.setMessagePreparer([&](const QJsonArray &history, const ProviderConfig &provider, QString *failure) {
        models.append(provider.model); ModelMediaCapabilities caps; caps.image = provider.model == "image-capable"; return pipeline.prepare(history, "chat-a", {}, caps, {}, failure);
    });
    QSignalSpy done(&engine, &AgentEngine::finished); engine.submitWithAttachments("describe", metadata["attachments"].toArray(), metadata["pathReferences"].toObject(), metadata["pathMetadata"].toObject());
    QTRY_COMPARE(done.size(), 1); QVERIFY(engine.lastTurnSucceeded()); QCOMPARE(models, QStringList({"image-capable", "text-only"}));
    QCOMPARE(server.bodies.size(), 2); QVERIFY(QJsonDocument(server.bodies[0]).toJson().contains("image_url"));
    QVERIFY(!QJsonDocument(server.bodies[1]).toJson().contains("image_url")); QVERIFY(QJsonDocument(server.bodies[1]).toJson().contains("Image content not available"));
    verifyStoredUserContext(engine.messages()[0].toObject(),metadata,"describe");
    for(const auto& body:server.bodies){const auto bytes=QJsonDocument(body).toJson();QVERIFY(!bytes.contains("\"metadata\""));QVERIFY(!bytes.contains("\"display\""));}
}
void AttachmentTest::preparationFailureAndCancellationKeepOriginalHistory() {
    ProviderFixture server; server.hold = true; QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto file = store.write("chat-a", "file.txt", "original", &error); const auto metadata = pipeline.capture("chat-a", {file.toJson()}, {}, &error);
    AgentEngine engine; engine.setProvider(server.provider()); QSignalSpy done(&engine, &AgentEngine::finished), failed(&engine, &AgentEngine::failed);
    engine.setMessagePreparer([](const QJsonArray &, const ProviderConfig &, QString *failure) { *failure = "fixture preparation failure"; return QJsonArray{}; });
    engine.submitWithAttachments("review", metadata["attachments"].toArray(), metadata["pathReferences"].toObject(), metadata["pathMetadata"].toObject());
    QCOMPARE(done.size(), 1); QCOMPARE(failed.size(), 1); QVERIFY(!engine.lastTurnSucceeded()); QCOMPARE(server.bodies.size(), 0);
    verifyStoredUserContext(engine.messages()[0].toObject(),metadata,"review");const QJsonValue retained=engine.messages()[0];
    engine.setMessagePreparer([&](const QJsonArray &history, const ProviderConfig &, QString *failure) { return pipeline.prepare(history, "chat-a", {}, {}, {}, failure); });
    engine.submit("continue"); QTRY_COMPARE(server.bodies.size(), 1); engine.cancel(); QCOMPARE(done.size(), 2); QVERIFY(!engine.lastTurnSucceeded());
    verifyStoredUserContext(engine.messages()[0].toObject(),metadata,"review");QCOMPARE(engine.messages()[0],retained);QCOMPARE(engine.messages().size(), 2);
    engine.completeExternalToolWithAttachments("stale-request", "late media", false, {file.toJson()}); QCOMPARE(engine.messages().size(), 2);
}
void AttachmentTest::structuredTextPreparationAndCapturedMetadata() {
    QTemporaryDir data, workspace; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const AttachmentMounts mounts{{"ws1", workspace.path()}};
    const QByteArray small = QByteArray("short line\n").repeated(40);
    const QByteArray large = QByteArray("# Root\n") + QByteArray("body\n").repeated(79) + "## Child\n" + QByteArray("tail\n").repeated(80);
    QVERIFY(writeFile(workspace.path() + "/small.txt", small)); QVERIFY(writeFile(workspace.path() + "/outline.md", large));
    const auto modified = QDateTime::fromString("2024-01-02T03:04:05.006Z", Qt::ISODateWithMs);
    QFile timed(workspace.path() + "/outline.md"); QVERIFY(timed.open(QIODevice::ReadWrite));
    QVERIFY(timed.setFileTime(modified, QFileDevice::FileModificationTime)); timed.close();
    const auto smallRef = store.importFile("chat-a", workspace.path() + "/small.txt", mounts, &error);
    const auto mdRef = store.importFile("chat-a", workspace.path() + "/outline.md", mounts, &error);
    const auto uploaded = store.write("chat-a", "uploaded.md", large, &error);
    const auto office = store.write("chat-a", "document.docx", "ASCII pretending to be a document", &error);
    const auto metadata = pipeline.capture("chat-a", {smallRef.toJson(), mdRef.toJson(), uploaded.toJson(), office.toJson()}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto sourceMeta = metadata["pathMetadata"].toObject()[mdRef.path].toObject();
    QCOMPARE(sourceMeta["modified"].toString(), modified.toString(Qt::ISODateWithMs)); QCOMPARE(sourceMeta["size"].toInteger(), qint64(large.size()));
    const QJsonArray history{userMessage("inspect", metadata)}; const auto original = QJsonDocument(history).toJson();
    const auto prepared = pipeline.prepare(history, "chat-a", mounts, {}, {}, &error); QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto parts = prepared[0].toObject()["content"].toArray(); QCOMPARE(parts.size(), 5);
    const auto promoted = parts[0].toObject()["text"].toString();
    QVERIFY(promoted.contains("40|short line")); QVERIFY(promoted.contains("<content>")); QVERIFY(!promoted.contains("<preview>"));
    const auto markdown = parts[1].toObject()["text"].toString();
    QVERIFY(markdown.contains("<preview>\n<outline>")); QVERIFY(markdown.contains("# Root [lines 1–162]"));
    QVERIFY(markdown.contains("## Child [lines 81–162]")); QVERIFY(markdown.contains("endLine=\"15\""));
    QVERIFY(markdown.contains("modified:2024-01-02T03:04:05.006Z")); QVERIFY(markdown.contains("format:markdown"));
    const auto full = parts[2].toObject()["text"].toString(); QVERIFY(!full.contains("<preview>")); QVERIFY(full.contains("161|tail"));
    const auto binary = parts[3].toObject()["text"].toString(); QVERIFY(binary.contains("Unsupported binary file (.docx)"));
    QVERIFY(!binary.contains("ASCII pretending")); QCOMPARE(QJsonDocument(history).toJson(), original);
    // A JSON save/reload and chat fork retain the captured metadata, even when
    // the source is gone; no current-file or snapshot-cache timestamp is used.
    QVERIFY(QFile::remove(workspace.path() + "/small.txt")); QVERIFY(QFile::remove(workspace.path() + "/outline.md"));
    QVERIFY(store.cloneAgent("chat-a", "chat-b", &error)); QVERIFY(store.removeAgent("chat-a", &error));
    AttachmentPipeline reopened(store); const auto reloaded = QJsonDocument::fromJson(original).array();
    QCOMPARE(reopened.prepare(reloaded, "chat-b", mounts, {}, {}, &error), prepared); QVERIFY(error.isEmpty());
    auto legacyMetadata = metadata; legacyMetadata.remove("pathMetadata");
    const auto legacy = reopened.prepare({userMessage("inspect", legacyMetadata)}, "chat-b", mounts, {}, {}, &error);
    QVERIFY(!legacy[0].toObject()["content"].toArray()[1].toObject()["text"].toString().contains("modified:"));
}
void AttachmentTest::malformedCapturedMetadata_data() {
    QTest::addColumn<QJsonValue>("entry");
    QTest::newRow("not-object") << QJsonValue("invalid");
    QTest::newRow("empty") << QJsonValue(QJsonObject{});
    QTest::newRow("wrong-size") << QJsonValue(QJsonObject{{"size", 99}});
    QTest::newRow("string-size") << QJsonValue(QJsonObject{{"size", "4"}});
    QTest::newRow("invalid-time") << QJsonValue(QJsonObject{{"size", 4}, {"modified", "not-a-date"}});
    QTest::newRow("wrong-time-type") << QJsonValue(QJsonObject{{"size", 4}, {"modified", 123}});
}
void AttachmentTest::malformedCapturedMetadata() {
    QFETCH(QJsonValue, entry); QTemporaryDir data; AttachmentStore store(data.path()); AttachmentPipeline pipeline(store); QString error;
    const auto ref = store.write("chat-a", "small.txt", "text", &error);
    auto metadata = pipeline.capture("chat-a", {ref.toJson()}, {}, &error);
    metadata["pathMetadata"] = QJsonObject{{ref.path, entry}};
    const auto result = pipeline.prepare({userMessage("inspect", metadata)}, "chat-a", {}, {}, {}, &error);
    QVERIFY(error.isEmpty()); const auto output = result[0].toObject()["content"].toArray()[0].toObject()["text"].toString();
    QVERIFY(output.contains("Attachment could not be included")); QVERIFY(!output.contains("1|text"));
}
QTEST_GUILESS_MAIN(AttachmentTest)
#include "test_attachments.moc"
