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

#include <windows.h>
class HistoricalAttachmentTest : public QObject {
  Q_OBJECT
private slots:
  void cleanup() {
    qunsetenv("ADE_ATTACHMENT_LOG");
    qunsetenv("ADE_ATTACHMENT_ROOT");
  }
  void missingHistoricalContinuesWithNotice_data() { rowsData(); }
  void missingHistoricalContinuesWithNotice() { run("missing-history", true); }
  void missingCurrentStillFails_data() { rowsData(); }
  void missingCurrentStillFails() { run("missing-current", false); }
  void corruptSnapshotCannotFallBackToValidOriginal_data() { rowsData(); }
  void corruptSnapshotCannotFallBackToValidOriginal() {
    run("corrupt-snapshot", false);
  }
  void changedOriginalStillFails_data() { rowsData(); }
  void changedOriginalStillFails() { run("changed-original", false); }
  void inaccessibleHistoricalSnapshotStillFails_data() { rowsData(); }
  void inaccessibleHistoricalSnapshotStillFails() {
    run("locked-snapshot", false);
  }
  void currentReattachmentCannotDegradeAsHistory_data() { rowsData(); }
  void currentReattachmentCannotDegradeAsHistory() {
    run("reattached-current", false);
  }
  void summaryBoundaryOmitsOldMissingWithoutNotice_data() { rowsData(); }
  void summaryBoundaryOmitsOldMissingWithoutNotice() { run("summary", true); }
  void availableHistoricalSnapshotPreservesBytes_data() { rowsData(); }
  void availableHistoricalSnapshotPreservesBytes() { run("snapshot", true); }
  void deletedDurableHistoryStillFails_data() { rowsData(); }
  void deletedDurableHistoryStillFails() { durableFailure(false); }
  void corruptDurableHistoryStillFails_data() { rowsData(); }
  void corruptDurableHistoryStillFails() { durableFailure(true); }

private:
  void durableFailure(bool corrupt) {
    QFETCH(QString, kind);
    QTemporaryDir data;
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    QJsonArray saved;
    QString delivered;
    {
      AgentEngine engine;
      engine.setProvider(provider(kind));
      AgentToolHost host(&engine, nullptr, "a", data.path());
      AttachmentStore source(data.path());
      QString error;
      const auto ref =
          source.write("a", "durable.bin", "captured-original", &error);
      const auto metadata = host.captureAttachments({ref.toJson()}, &error);
      QVERIFY(!metadata.isEmpty());
      QSignalSpy done(&engine, &AgentEngine::finished);
      QVERIFY(engine.submitWithAttachments(
          "First", metadata["attachments"].toArray(),
          metadata["pathReferences"].toObject(),
          metadata["pathMetadata"].toObject()));
      QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
      QVERIFY(engine.lastTurnSucceeded());
      QCOMPARE(files(log).size(), 1);
      delivered = files(log).first().toObject()["path"].toString();
      saved = engine.messages();
    }
    if (corrupt) {
      QFile file(delivered);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(file.write("changed"), qint64(7));
      file.close();
    } else
      QVERIFY(QFile::remove(delivered));
    QFile logFile(log);
    QVERIFY(logFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    logFile.close();
    AgentEngine engine;
    engine.setProvider(provider(kind));
    engine.setMessages(saved);
    AgentToolHost host(&engine, nullptr, "a", data.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    engine.submit("Ordinary follow-up");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QVERIFY(!engine.lastTurnSucceeded());
    QVERIFY(rows(log).isEmpty());
  }
  void rowsData() {
    QTest::addColumn<QString>("kind");
    for (const auto &kind : QStringList{"codex", "opencode", "claude-code"})
      QTest::newRow(qPrintable(kind)) << kind;
  }
  void run(const QString &scenario, bool success) {
    QFETCH(QString, kind);
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const auto log = data.filePath("wire");
    qputenv("ADE_ATTACHMENT_LOG", log.toUtf8());
    qputenv("ADE_ATTACHMENT_ROOT", data.path().toUtf8());
    AgentEngine engine;
    engine.setProvider(provider(kind));
    AgentToolHost host(&engine, nullptr, "a", data.path());
    AttachmentStore source(data.path());
    QString error;
    const QByteArray bytes("old\0bytes", 9);
    const auto ref = source.write("a", "old.bin", bytes, &error);
    QVERIFY2(!ref.isEmpty(), qPrintable(error));
    const auto metadata = host.captureAttachments({ref.toJson()}, &error);
    QVERIFY2(!metadata.isEmpty(), qPrintable(error));
    const auto sourcePath = source.resolve("a", ref, {}, &error);
    const auto snapshotDir = data.filePath("attachment-snapshots/a");
    const auto snapshots = QDir(snapshotDir).entryList({"*.bin"}, QDir::Files);
    QCOMPARE(snapshots.size(), 1);
    const auto snapshotPath = QDir(snapshotDir).filePath(snapshots.first());
    HANDLE blocked = INVALID_HANDLE_VALUE;
    struct Guard {
      HANDLE &value;
      ~Guard() {
        if (value != INVALID_HANDLE_VALUE)
          CloseHandle(value);
      }
    } blocker{blocked};
    if (scenario == "corrupt-snapshot") {
      QFile file(snapshotPath);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(file.write("damaged"), qint64(7));
      file.close();
    } else if (scenario == "locked-snapshot") {
      blocked = CreateFileW(
          QDir::toNativeSeparators(snapshotPath).toStdWString().c_str(),
          GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      QVERIFY(blocked != INVALID_HANDLE_VALUE);
    } else if (scenario == "changed-original") {
      QVERIFY(QFile::remove(snapshotPath));
      QFile file(sourcePath);
      QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      QCOMPARE(file.write("changed"), qint64(7));
      file.close();
    } else if (scenario == "snapshot") {
      QVERIFY(QFile::remove(sourcePath));
    } else {
      QVERIFY(QFile::remove(snapshotPath));
      QVERIFY(QFile::remove(sourcePath));
    }
    const QJsonArray history{
        QJsonObject{{"role", "user"},
                    {"content", "Older request"},
                    {"metadata", metadata}},
        QJsonObject{{"role", "assistant"}, {"content", "Older response"}}};
    engine.setMessages(history);
    if (scenario == "summary") {
      auto summarized = history;
      summarized.append(
          QJsonObject{{"role", "user"},
                      {"content", "Summary boundary"},
                      {"ade_compressedHistory", "Retained summary"}});
      summarized.append(
          QJsonObject{{"role", "assistant"}, {"content", "Summary accepted"}});
      engine.setMessages(summarized);
    }
    QSignalSpy done(&engine, &AgentEngine::finished);
    if (scenario == "missing-current" || scenario == "reattached-current") {
      if (scenario == "missing-current")
        engine.setMessages({});
      QVERIFY(engine.submitWithAttachments(
          "Current request", metadata["attachments"].toArray(),
          metadata["pathReferences"].toObject(),
          metadata["pathMetadata"].toObject()));
    } else
      engine.submit("Current request");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
    QCOMPARE(engine.lastTurnSucceeded(), success);
    if (!success) {
      QVERIFY(rows(log).isEmpty());
      return;
    }
    QByteArray wire;
    for (const auto &r : rows(log))
      wire += QJsonDocument(r["frame"].toObject()).toJson();
    QVERIFY(wire.contains("Current request"));
    if (scenario == "missing-history") {
      QVERIFY(wire.contains("Historical attachment unavailable"));
      QVERIFY(wire.contains(ref.path.toUtf8()));
      QVERIFY(files(log).isEmpty());
      QCOMPARE(engine.messages().first().toObject()["metadata"], metadata);
    } else if (scenario == "summary") {
      QVERIFY(!wire.contains("Historical attachment unavailable"));
      QVERIFY(files(log).isEmpty());
    } else if (scenario == "snapshot") {
      QCOMPARE(files(log).size(), 1);
      QCOMPARE(files(log).first().toObject()["sha256"].toString(), sha(bytes));
    }
  }
};
QTEST_MAIN(HistoricalAttachmentTest)
#include "test_historical_attachments.moc"
