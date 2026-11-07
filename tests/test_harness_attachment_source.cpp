#include "runtime/HarnessAttachmentSource.h"
#include "runtime/HarnessInputPreparation.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>
using namespace ade;
using Status = HarnessAttachmentAvailability;
namespace {
QString hash(const QByteArray &bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void write(const QString &path, const QByteArray &bytes) {
  QFile f(path);
  QVERIFY(f.open(QIODevice::WriteOnly));
  QCOMPARE(f.write(bytes), bytes.size());
}
} // namespace
class AttachmentSourceTest : public QObject {
  Q_OBJECT
private slots:
  void absentDescendantIsDistinctFromUnavailableScope() {
    QTemporaryDir data;
    HarnessAttachmentSource source(data.path(), "a",
                                   {{"ws1", data.filePath("missing-root")}});
    QCOMPARE(source.original({"att/missing.bin", {}}, nullptr).status,
             Status::Missing);
    QCOMPARE(source.original({"ws1/missing.bin", {}}, nullptr).status,
             Status::Invalid);
    QCOMPARE(source.original({"unknown/missing.bin", {}}, nullptr).status,
             Status::Invalid);
    HarnessAttachmentSource missing(data.filePath("missing"), "a", {});
    QCOMPARE(missing.original({"att/missing.bin", {}}, nullptr).status,
             Status::Invalid);
  }
  void parentFileAndTraversalNeverDegrade() {
    QTemporaryDir data;
    write(data.filePath("attachments"), "file");
    HarnessAttachmentSource source(data.path(), "a", {});
    QCOMPARE(source.original({"att/a.bin", {}}, nullptr).status,
             Status::Invalid);
    for (const auto &path :
         QStringList{"att/../a", "att/a:b", "att/a/child", "att/NUL.txt",
                     "att/a. ", "/att/a", "att\\a"})
      QCOMPARE(source.original({path, {}}, nullptr).status, Status::Invalid);
  }
  void ownerIsolationAndCancelledReads() {
    QTemporaryDir data;
    QVERIFY(QDir().mkpath(data.filePath("attachments/b")));
    write(data.filePath("attachments/b/file.bin"), "foreign");
    HarnessAttachmentSource source(data.path(), "a", {});
    QCOMPARE(source.original({"att/file.bin", {}}, nullptr).status,
             Status::Missing);
    HarnessAttachmentSource foreign(data.path(), "b", {});
    QCOMPARE(foreign.original({"att/file.bin", {}}, nullptr).bytes,
             QByteArray("foreign"));
    std::atomic_bool stop{true};
    QCOMPARE(foreign.original({"att/file.bin", {}}, &stop).status,
             Status::Invalid);
  }
  void junctionAndRootAliasAreInvalid() {
    QTemporaryDir data, foreign;
    QVERIFY(QDir().mkpath(foreign.filePath("a")));
    QProcess junction;
    junction.start("cmd.exe",
                   {"/c", "mklink", "/J",
                    QDir::toNativeSeparators(data.filePath("attachments")),
                    QDir::toNativeSeparators(foreign.path())});
    QVERIFY(junction.waitForFinished());
    QCOMPARE(junction.exitCode(), 0);
    HarnessAttachmentSource source(data.path(), "a", {});
    QCOMPARE(source.original({"att/missing.bin", {}}, nullptr).status,
             Status::Invalid);
    HarnessAttachmentSource alias(data.filePath("attachments"), "a", {});
    QCOMPARE(alias.original({"att/missing.bin", {}}, nullptr).status,
             Status::Invalid);
    QVERIFY(QDir().rmdir(data.filePath("attachments")));
  }
  void capturedMountRemainsPinned() {
    QTemporaryDir data;
    QVERIFY(QDir().mkpath(data.filePath("workspace")));
    HarnessAttachmentSource source(data.path(), "a",
                                   {{"ws1", data.filePath("workspace")}});
    QVERIFY(!MoveFileExW(
        QDir::toNativeSeparators(data.filePath("workspace"))
            .toStdWString()
            .c_str(),
        QDir::toNativeSeparators(data.filePath("moved")).toStdWString().c_str(),
        0));
    QCOMPARE(source.original({"ws1/missing.bin", {}}, nullptr).status,
             Status::Missing);
  }
  void boundedNoticePreservesHistoryAndExplicitCount() {
    QTemporaryDir data;
    HarnessAttachmentStore store(data.path(), "a");
    QVERIFY(store.error().isEmpty());
    HarnessAttachmentSource source(store.profileDirectory(), "a", {});
    QJsonArray refs;
    QJsonObject hashes;
    for (int i = 0; i < 100; ++i) {
      const auto path =
          "att/" + QString(150, 'a') + QString::number(i) + ".bin";
      refs.append(QJsonObject{{"path", path}});
      hashes[path] = hash("missing");
    }
    const QJsonArray history{
        QJsonObject{{"role", "user"},
                    {"content", "Old"},
                    {"metadata", QJsonObject{{"attachments", refs},
                                             {"pathReferences", hashes}}}},
        QJsonObject{{"role", "assistant"}, {"content", "Old answer"}},
        QJsonObject{{"role", "user"}, {"content", "Current"}}};
    const auto prepared =
        prepareHarnessInput(store, source, history, {}, nullptr);
    QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
    QVERIFY(prepared.attachments.history.empty());
    QCOMPARE(prepared.input.size(), 2);
    const auto notice = prepared.input.last().toObject()["text"].toString();
    QVERIFY(notice.toUtf8().size() < 9000);
    const auto counts =
        QJsonDocument::fromJson(notice.section('\n', 1).toUtf8()).object();
    QCOMPARE(counts["unavailableCount"].toInt(), 100);
    QCOMPARE(counts["references"].toArray().size(), 32);
    QCOMPARE(counts["omittedReferenceNames"].toInt(), 68);
  }
};
QTEST_GUILESS_MAIN(AttachmentSourceTest)
#include "test_harness_attachment_source.moc"
