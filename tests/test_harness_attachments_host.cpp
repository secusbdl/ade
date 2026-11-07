#include "attachments/AttachmentStore.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(const QString &kind) {
  ProviderConfig p;
  p.id = kind;
  p.name = kind;
  p.kind = kind;
  p.model = kind == "claude-code" ? "fixture-model" : "model-a";
  p.protocol = kind == "codex"      ? "codex-app-server"
               : kind == "opencode" ? "acp"
                                    : "claude-stream-json";
  p.harnessExecutable =
      QDir(QCoreApplication::applicationDirPath())
          .filePath(kind == "codex"      ? "attachment_codex_fixture.exe"
                    : kind == "opencode" ? "attachment_acp_fixture.exe"
                                         : "attachment_claude_fixture.exe");
  return p;
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
QJsonArray files(const QString &path) {
  QJsonArray result;
  for (const auto &row : rows(path))
    for (const auto &file : row["files"].toArray())
      result.append(file);
  return result;
}
QString sha(const QByteArray &b) {
  return QString::fromLatin1(
      QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
} // namespace
class HarnessAttachmentsHostTest : public QObject {
  Q_OBJECT
private slots:
  void summaryBoundaryExcludesMissingOlderAttachment_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void summaryBoundaryExcludesMissingOlderAttachment() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "old.bin", QByteArray("A\0B", 3), &error);
    const auto metadata = host.captureAttachments({ref.toJson()}, &error);
    QVERIFY(!metadata.isEmpty());
    QVERIFY2(source.removeAgent("a", &error), qPrintable(error));
    engine.setMessages(
        {QJsonObject{{"role", "user"},
                     {"content", "Older attached context"},
                     {"metadata", metadata}},
         QJsonObject{{"role", "assistant"}, {"content", "Older reply"}},
         QJsonObject{{"role", "user"},
                     {"content", "Summarized boundary"},
                     {"ade_compressedHistory", "Earlier context summarized"}},
         QJsonObject{{"role", "assistant"},
                     {"content", "Continue from summary"}}});
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("Current request after summary");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY2(engine.lastTurnSucceeded(),
             qPrintable(QJsonDocument(engine.messages()).toJson()));
    QVERIFY(files(log).isEmpty());
  }
  void cleanup() {
    qunsetenv("ADE_ATTACHMENT_LOG");
    qunsetenv("ADE_ATTACHMENT_ROOT");
    qunsetenv("ADE_ATTACHMENT_IMAGE_CAPABILITY");
  }
  void currentCapturedBytesReachRuntimeWithoutChangingHistory_data() {
    QTest::addColumn<QString>("kind");
    for (const auto &kind : QStringList{"codex", "opencode", "claude-code"})
      QTest::newRow(qPrintable(kind)) << kind;
  }
  void currentCapturedBytesReachRuntimeWithoutChangingHistory() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    const QByteArray bytes("original\0binary\xff", 16);
    AttachmentStore source(data.path());
    QString error;
    const auto ref = source.write("a", "example.bin", bytes, &error);
    QVERIFY2(!ref.isEmpty(), qPrintable(error));
    const auto metadata = host.captureAttachments({ref.toJson()}, &error);
    QVERIFY2(!metadata.isEmpty(), qPrintable(error));
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Read my file",
                                         metadata["attachments"].toArray(),
                                         metadata["pathReferences"].toObject(),
                                         metadata["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY2(engine.lastTurnSucceeded(),
             qPrintable(QJsonDocument(engine.messages()).toJson()));
    const auto seen = files(log);
    QCOMPARE(seen.size(), 1);
    QCOMPARE(seen.first().toObject()["sha256"].toString(), sha(bytes));
    const auto history = QJsonDocument(engine.messages()).toJson();
    QVERIFY(!history.contains("harness-attachments"));
    QVERIFY(!history.contains("ADE_ATTACHMENT_V1"));
    QCOMPARE(engine.messages()
                 .first()
                 .toObject()["metadata"]
                 .toObject()["attachments"],
             metadata["attachments"]);
  }
  void cancelBeforePreparedCompletionNeverSendsPrompt_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void cancelBeforePreparedCompletionNeverSendsPrompt() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    auto host =
        std::make_unique<AgentToolHost>(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "data.bin", QByteArray(1024 * 1024, 'x'), &error);
    const auto m = host->captureAttachments({ref.toJson()}, &error);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Cancel", m["attachments"].toArray(),
                                         m["pathReferences"].toObject(),
                                         m["pathMetadata"].toObject()));
    engine.cancel();
    QCOMPARE(done.size(), 1);
    QTest::qWait(150);
    QVERIFY(files(log).isEmpty());
    QVERIFY(!engine.isBusy());
    host.reset();
  }
  void originalSourceDeletionAndRestartRetainExactBytes_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void originalSourceDeletionAndRestartRetainExactBytes() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    QJsonArray saved;
    QJsonObject metadata;
    QString delivered;
    {
      AgentEngine engine;
      engine.setProvider(provider(kind));
      AgentToolHost host(&engine, nullptr, "a", data.path());
      AttachmentStore source(data.path());
      QString error;
      const auto ref =
          source.write("a", "original.bin", QByteArray("A\0B", 3), &error);
      metadata = host.captureAttachments({ref.toJson()}, &error);
      QSignalSpy done(&engine, &AgentEngine::finished);
      QVERIFY(engine.submitWithAttachments(
          "first", metadata["attachments"].toArray(),
          metadata["pathReferences"].toObject(),
          metadata["pathMetadata"].toObject()));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
      QVERIFY(engine.lastTurnSucceeded());
      saved = engine.messages();
      delivered = files(log).first().toObject()["path"].toString();
      QVERIFY(QFile::remove(source.resolve("a", ref, {}, &error)));
      QVERIFY2(source.removeAgent("a", &error), qPrintable(error));
    }
    {
      AgentEngine engine;
      engine.setProvider(provider(kind));
      engine.setMessages(saved);
      AgentToolHost host(&engine, nullptr, "a", data.path());
      QSignalSpy done(&engine, &AgentEngine::finished);
      QVERIFY(engine.submitWithAttachments(
          "again", metadata["attachments"].toArray(),
          metadata["pathReferences"].toObject(),
          metadata["pathMetadata"].toObject()));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
      QVERIFY(engine.lastTurnSucceeded());
      const auto seen = files(log);
      QCOMPARE(seen.size(), 2);
      QCOMPARE(seen.last().toObject()["path"].toString(), delivered);
      QCOMPARE(seen.last().toObject()["sha256"].toString(),
               sha(QByteArray("A\0B", 3)));
    }
  }
  void forkReceivesOnlyChildOwnedHistoricalFiles_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void forkReceivesOnlyChildOwnedHistoricalFiles() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "original.bin", QByteArray("A\0B", 3), &error);
    const auto m = host.captureAttachments({ref.toJson()}, &error);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("first", m["attachments"].toArray(),
                                         m["pathReferences"].toObject(),
                                         m["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(engine.lastTurnSucceeded());
    const auto parentPath = files(log).first().toObject()["path"].toString();
    QVERIFY2(host.cloneFileContext("b", &error), qPrintable(error));
    AgentEngine child;
    child.setProvider(provider(kind));
    child.setMessages(engine.messages());
    AgentToolHost childHost(&child, nullptr, "b", data.path());
    QSignalSpy childDone(&child, &AgentEngine::finished);
    child.submit("Continue from the captured attachment");
    QTRY_COMPARE_WITH_TIMEOUT(childDone.size(), 1, 15000);
    QVERIFY(child.lastTurnSucceeded());
    const auto delivered = files(log);
    QCOMPARE(delivered.size(), 2);
    const auto childPath = delivered.last().toObject()["path"].toString();
    QVERIFY(childPath != parentPath);
    QVERIFY(childPath.contains(sha("b")));
    QVERIFY(parentPath.contains(sha("a")));
    QCOMPARE(delivered.last().toObject()["sha256"].toString(),
             sha(QByteArray("A\0B", 3)));
    QVERIFY2(childHost.removeFileContext(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(childPath));
    QVERIFY(QFileInfo::exists(parentPath));
  }
  void supportedImageAndTextRemainPrepared_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void replacedHistoryDoesNotResurrectRemovedAttachments_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void replacedHistoryDoesNotResurrectRemovedAttachments() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "removed.bin", QByteArray("A\0B", 3), &error);
    const auto m = host.captureAttachments({ref.toJson()}, &error);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("first", m["attachments"].toArray(),
                                         m["pathReferences"].toObject(),
                                         m["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(files(log).size(), 1);
    engine.setMessages({});
    engine.submit("Fresh history without attachments");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 15000);
    QVERIFY(engine.lastTurnSucceeded());
    QCOMPARE(files(log).size(), 1);
  }
  void destroyingOwnerDuringPreparationSendsNoPrompt_data() {
    currentCapturedBytesReachRuntimeWithoutChangingHistory_data();
  }
  void destroyingOwnerDuringPreparationSendsNoPrompt() {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    auto engine = std::make_unique<AgentEngine>();
    engine->setProvider(provider(kind));
    auto *host = new AgentToolHost(engine.get(), nullptr, "a", data.path(),
                                   engine.get());
    AttachmentStore source(data.path());
    QString error;
    const auto ref =
        source.write("a", "pending.bin", QByteArray(1024 * 1024, 'a'), &error);
    const auto m = host->captureAttachments({ref.toJson()}, &error);
    QVERIFY(!m.isEmpty());
    QVERIFY(engine->submitWithAttachments(
        "Cancel owner", m["attachments"].toArray(),
        m["pathReferences"].toObject(), m["pathMetadata"].toObject()));
    engine.reset();
    QTest::qWait(200);
    QVERIFY(files(log).isEmpty());
  }
  void supportedImageAndTextRemainPrepared() {
    qputenv("ADE_ATTACHMENT_IMAGE_CAPABILITY", "1");
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    QImage image(3, 2, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(image.save(&buffer, "PNG"));
    const auto ref = source.write("a", "small.png", png, &error),
               text = source.write("a", "notes.txt", "visible text preview\r\n",
                                   &error);
    const auto m =
        host.captureAttachments({ref.toJson(), text.toJson()}, &error);
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments(
        "Keep my prompt intact", m["attachments"].toArray(),
        m["pathReferences"].toObject(), m["pathMetadata"].toObject()));
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(engine.lastTurnSucceeded());
    QByteArray wire;
    for (const auto &row : rows(log))
      wire += QJsonDocument(row["frame"].toObject()).toJson();
    QVERIFY(wire.contains("Keep my prompt intact"));
    QVERIFY(wire.contains("visible text preview"));
    QVERIFY(wire.contains("image"));
    QCOMPARE(files(log).size(), 1);
    QCOMPARE(files(log).first().toObject()["sha256"].toString(),
             sha("visible text preview\r\n"));
  }
};
QTEST_MAIN(HarnessAttachmentsHostTest)
#include "test_harness_attachments_host.moc"
