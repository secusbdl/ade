#include "core/AgentEngine.h"
#include "harness/AcpHarnessSession.h"
#include "harness/HarnessAttachmentStore.h"
#include "runtime/AgentToolHost.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QString digest(const QByteArray &b) {
  return QString::fromLatin1(
      QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
QList<QJsonObject> rows(const QString &path) {
  QFile f(path);
  QList<QJsonObject> out;
  if (f.open(QIODevice::ReadOnly))
    for (const auto &line : f.readAll().split('\n'))
      if (!line.trimmed().isEmpty())
        out.append(QJsonDocument::fromJson(line).object());
  return out;
}
QJsonArray prompt(const QString &path) {
  for (const auto &row : rows(path)) {
    const auto frame = row["frame"].toObject();
    if (frame["method"] == "session/prompt")
      return frame["params"].toObject()["prompt"].toArray();
  }
  return {};
}
QString fixture() {
  return QDir(QCoreApplication::applicationDirPath())
      .filePath("attachment_acp_fixture.exe");
}
} // namespace
class HarnessAttachmentMimeTest : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_ATTACHMENT_LOG");
    qunsetenv("ADE_ATTACHMENT_ROOT");
    qunsetenv("ADE_ATTACHMENT_IMAGE_CAPABILITY");
  }
  void extensionHintReachesActualAcpWire_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("mime");
    for (const auto &p :
         QList<QPair<QString, QString>>{{"a.gif", "image/gif"},
                                        {"a.jpeg", "image/jpeg"},
                                        {"a.jpg", "image/jpeg"},
                                        {"a.png", "image/png"},
                                        {"a.webp", "image/webp"},
                                        {"Unicode-é.PNG", "image/png"},
                                        {"UPPER.JPEG", "image/jpeg"},
                                        {"a.txt", {}},
                                        {"a.svg", {}},
                                        {"a.png.exe", {}},
                                        {"a.png?mimeType=image/png", {}},
                                        {"no-extension", {}},
                                        {".png", {}},
                                        {".hidden.PNG", "image/png"}})
      QTest::newRow(qPrintable(p.first)) << p.first << p.second;
  }
  void extensionHintReachesActualAcpWire() {
    QFETCH(QString, name);
    QFETCH(QString, mime);
    run(name, mime, false, false);
  }
  void unsupportedInlineImageUsesCapturedNameNotInputMime() {
    run("captured.PNG", "image/png", true, false);
  }
  void restartReconstructsHintWithoutSchemaChange() {
    run("restored.webp", "image/webp", false, true);
  }
  void explicitMetadataCannotInventMime() {
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    ProviderConfig p;
    p.id = "fixture";
    p.kind = "opencode";
    p.protocol = "acp";
    p.model = "model-a";
    p.harnessExecutable = fixture();
    engine.setProvider(p);
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "untrusted.txt", "plain captured bytes", &error);
    auto metadata = host.captureAttachments({ref.toJson()}, &error);
    QVERIFY(!metadata.isEmpty());
    auto reference = metadata["attachments"].toArray().first().toObject();
    reference["mimeType"] = "image/png";
    reference["mediaType"] = "image/png";
    reference["uri"] = "file:///foreign/authority.png";
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Read", {reference},
                                         metadata["pathReferences"].toObject(),
                                         metadata["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
    QVERIFY(engine.lastTurnSucceeded());
    int count = 0;
    for (const auto &part : prompt(log)) {
      const auto object = part.toObject();
      if (object["type"] == "resource_link") {
        ++count;
        QVERIFY(!object.contains("mimeType"));
        QVERIFY(!object["uri"].toString().contains("foreign"));
      }
    }
    QCOMPARE(count, 1);
  }
  void transportBoundRemainsFourMiB() {
    QTemporaryDir data;
    HarnessAttachmentStore store(data.path(), "a");
    const QByteArray bytes("x");
    const auto lease =
        store.prepare({{"att/a.png", "a.png", digest(bytes), bytes}}, nullptr);
    QVERIFY(lease.valid());
    QString error;
    const auto parts = mapHarnessAttachments(
        {QJsonObject{{"type", "text"},
                     {"text", QString(4 * 1024 * 1024, 'x')}}},
        {lease, {}, {}}, true, false, true, &error);
    QVERIFY(parts.isEmpty());
    QVERIFY(error.contains("4 MiB"));
  }

private:
  void run(const QString &name, const QString &expected, bool inlineUnsupported,
           bool restart) {
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    const QByteArray bytes("exact\0original\xff", 15);
    HarnessAttachmentLease lease;
    if (restart) {
      HarnessAttachmentStore store(data.path(), "a");
      QVERIFY(store
                  .prepare({{"att/source.bin", name, digest(bytes), bytes}},
                           nullptr)
                  .valid());
    }
    HarnessAttachmentStore store(data.path(), "a");
    QString error;
    lease =
        restart
            ? store.retained(&error)
            : store.prepare({{"att/source.bin", name, digest(bytes), bytes}},
                            nullptr, &error);
    QVERIFY2(lease.valid(), qPrintable(error));
    AcpHarnessSession session;
    AcpHarnessOptions options;
    options.executable = fixture();
    options.providerId = "fixture";
    options.workspace = data.path();
    options.model = "model-a";
    options.attachments = {lease,
                           {},
                           inlineUnsupported ? QStringList{"att/source.bin"}
                                             : QStringList{}};
    QJsonArray input{
        QJsonObject{{"type", "text"}, {"text", "Read captured image file"}}};
    if (inlineUnsupported)
      input.append(QJsonObject{{"type", "image"},
                               {"data", "YQ=="},
                               {"mimeType", "image/untrusted"}});
    QSignalSpy done(&session, &AcpHarnessSession::finished);
    QVERIFY(session.startTurn(options, input));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
    QCOMPARE(done.first().first().toJsonObject()["status"].toString(),
             "completed");
    session.shutdown();
    int links = 0;
    for (const auto &part : prompt(log)) {
      const auto object = part.toObject();
      QVERIFY(object["type"] != "image");
      if (object["type"] == "resource_link") {
        ++links;
        QCOMPARE(object["name"].toString(), name);
        if (expected.isEmpty())
          QVERIFY(!object.contains("mimeType"));
        else
          QCOMPARE(object["mimeType"].toString(), expected);
      }
    }
    QCOMPARE(links, 1);
    int readCount = 0;
    for (const auto &row : rows(log)) {
      const auto frame = row["frame"].toObject();
      if (frame["method"] == "session/new")
        QVERIFY(!frame["params"].toObject().contains("additionalDirectories"));
      for (const auto &file : row["files"].toArray()) {
        ++readCount;
        QCOMPARE(file.toObject()["sha256"].toString(), digest(bytes));
        QCOMPARE(file.toObject()["size"].toInteger(), bytes.size());
      }
    }
    QCOMPARE(readCount, 1);
  }
};
QTEST_MAIN(HarnessAttachmentMimeTest)
#include "test_harness_attachment_mime.moc"
