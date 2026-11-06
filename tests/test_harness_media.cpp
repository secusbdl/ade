#include "attachments/AttachmentPipeline.h"
#include "attachments/ModelMediaCatalog.h"
#include "core/AgentEngine.h"
#include "core/ProviderConfig.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class HarnessMediaTest : public QObject {
  Q_OBJECT
private slots:
  void cleanup() { qunsetenv("ADE_CLAUDE_LOG"); }
  void externalDefaultImageSurvivesPipeline_data() {
    QTest::addColumn<QString>("kind");
    QTest::newRow("claude") << QString("claude-code");
    QTest::newRow("codex") << QString("codex");
    QTest::newRow("opencode") << QString("opencode");
  }
  void externalDefaultImageSurvivesPipeline() {
    QFETCH(QString, kind);
    ProviderConfig p;
    p.id = "instance-1";
    p.kind = kind;
    p.model = "default";
    QTemporaryDir dir;
    AttachmentStore store(dir.path());
    AttachmentPipeline pipeline(store);
    QImage image(20, 10, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    QString error;
    const auto ref = store.write("owner", "red.png", bytes, &error);
    QVERIFY2(!ref.isEmpty(), qPrintable(error));
    const auto metadata = pipeline.capture("owner", {ref.toJson()}, {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QJsonArray original{QJsonObject{{"role", "user"},
                                          {"content", "Inspect this image"},
                                          {"metadata", metadata}}};
    const auto prepared = pipeline.prepare(
        original, "owner", {}, ModelMediaCatalog::resolve(p), {}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    bool imageFound = false;
    for (const auto &value :
         prepared.first().toObject().value("content").toArray())
      if (value.toObject().value("type") == "image")
        imageFound = true;
    QVERIFY(imageFound);
    QVERIFY(!original.first().toObject().value("content").isArray());
    QCOMPARE(store.read("owner", ref, {}, &error), bytes);
    auto http = p;
    http.kind = "api";
    const auto httpPrepared = pipeline.prepare(
        original, "owner", {}, ModelMediaCatalog::resolve(http), {}, &error);
    QVERIFY(error.isEmpty());
    for (const auto &value :
         httpPrepared.first().toObject().value("content").toArray())
      QVERIFY(value.toObject().value("type") != "image");
  }
  void storedImageReachesNativeClaudeThroughActualPipeline() {
    QTemporaryDir dir;
    const auto log = dir.filePath("wire");
    qputenv("ADE_CLAUDE_LOG", log.toUtf8());
    AttachmentStore store(dir.path());
    AttachmentPipeline pipeline(store);
    QImage image(20, 10, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    QString error;
    const auto ref = store.write("owner", "red.png", bytes, &error);
    QVERIFY(!ref.isEmpty());
    const auto metadata = pipeline.capture("owner", {ref.toJson()}, {}, &error);
    QVERIFY(error.isEmpty());
    ProviderConfig p;
    p.id = "claude-fixture";
    p.kind = "claude-code";
    p.protocol = "claude-control";
    p.model = "default";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("claude_fixture.exe");
    AgentEngine engine;
    engine.setProvider(p);
    engine.setHarnessContext("owner", dir.path());
    engine.setMessagePreparer([&](const QJsonArray &messages,
                                  const ProviderConfig &provider,
                                  QString *failure) {
      return pipeline.prepare(messages, "owner", {},
                              ModelMediaCatalog::resolve(provider), {},
                              failure);
    });
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments(
        "Inspect", metadata.value("attachments").toArray(),
        metadata.value("pathReferences").toObject(),
        metadata.value("pathMetadata").toObject()));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QFile wire(log);
    QVERIFY(wire.open(QIODevice::ReadOnly));
    QJsonObject source;
    for (const auto &line : wire.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row.value("type") != "user")
        continue;
      for (const auto &part :
           row.value("message").toObject().value("content").toArray())
        if (part.toObject().value("type") == "image")
          source = part.toObject().value("source").toObject();
    }
    QVERIFY(!source.isEmpty());
    QCOMPARE(source.value("type").toString(), QString("base64"));
    QImage decoded;
    QVERIFY(decoded.loadFromData(
        QByteArray::fromBase64(source.value("data").toString().toLatin1())));
    QCOMPARE(decoded.size(), image.size());
    QVERIFY(decoded.pixelColor(0, 0).red() > 240);
    QVERIFY(decoded.pixelColor(0, 0).green() < 10);
    QVERIFY(!QJsonDocument(engine.messages())
                 .toJson()
                 .contains(source.value("data").toString().toLatin1()));
    QCOMPARE(store.read("owner", ref, {}, &error), bytes);
  }
  void httpModelAliasesAndUnknownKindsRemainExact() {
    for (const auto &id :
         QStringList{"default", "quick", "claude-sonnet-5.5", "unknown"}) {
      ProviderConfig p;
      p.kind = "api";
      p.model = id;
      const auto old = ModelMediaCatalog::resolve(id),
                 now = ModelMediaCatalog::resolve(p);
      QCOMPARE(now.image, old.image);
      QCOMPARE(now.imageMimeTypes, old.imageMimeTypes);
      QCOMPARE(now.maxWidth, old.maxWidth);
      QCOMPARE(now.maxHeight, old.maxHeight);
      QCOMPARE(now.maxPixels, old.maxPixels);
      QCOMPARE(now.maxImageBytes, old.maxImageBytes);
    }
    ProviderConfig unknown;
    unknown.kind = "invented-harness";
    unknown.model = "default";
    QVERIFY(!ModelMediaCatalog::resolve(unknown).image);
    ProviderConfig harness;
    harness.kind = "claude-code";
    harness.model = "default";
    const auto caps = ModelMediaCatalog::resolve(harness);
    const ModelMediaCapabilities bounds;
    QCOMPARE(caps.maxImageBytes, bounds.maxImageBytes);
    QCOMPARE(caps.maxPixels, bounds.maxPixels);
    QCOMPARE(caps.maxWidth, bounds.maxWidth);
    QCOMPARE(caps.maxHeight, bounds.maxHeight);
  }
};
QTEST_GUILESS_MAIN(HarnessMediaTest)
#include "test_harness_media.moc"
