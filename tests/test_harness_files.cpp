#include "HarnessFileContext.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>
using namespace ade;
namespace {
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace
class HarnessFilesTest : public QObject {
  Q_OBJECT
private slots:
  void deferredCleanupAdoptsExistingAndNeverCreatesAbsent() {
    QTemporaryDir dir;
    QString error;
    QVERIFY(HarnessFileContext::removeAgent(dir.path(), "absent", &error));
    QVERIFY(!QFileInfo::exists(dir.filePath("harness-workspaces")));
    QString path;
    {
      HarnessFileContext owned(dir.path(), "owned");
      path = owned.rootDirectory();
      QVERIFY(!path.isEmpty());
      QVERIFY(write(path + "/kept.bin", "exact"));
    }
    QVERIFY2(HarnessFileContext::removeAgent(dir.path(), "owned", &error),
             qPrintable(error));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(HarnessFileContext::removeAgent(dir.path(), "owned", &error));
  }
  void binaryNestedCloneAndDeleteAreIndependent() {
    QTemporaryDir dir;
    HarnessFileContext source(dir.path(), "source");
    QVERIFY2(!source.rootDirectory().isEmpty(), qPrintable(source.error()));
    QVERIFY(QDir().mkpath(source.rootDirectory() + "/nested/empty"));
    const QByteArray data("A\0B\xff", 4);
    QVERIFY(write(source.rootDirectory() + "/nested/file.bin", data));
    QString error;
    QVERIFY2(source.cloneTo("target", &error), qPrintable(error));
    HarnessFileContext target(dir.path(), "target");
    QVERIFY2(!target.rootDirectory().isEmpty(), qPrintable(target.error()));
    QCOMPARE(read(target.rootDirectory() + "/nested/file.bin"), data);
    QVERIFY(QFileInfo::exists(target.rootDirectory() + "/nested/empty"));
    QVERIFY(write(target.rootDirectory() + "/nested/file.bin", "TARGET"));
    QCOMPARE(read(source.rootDirectory() + "/nested/file.bin"), data);
    const auto targetPath = target.rootDirectory();
    QVERIFY2(target.remove(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetPath));
    QVERIFY(target.remove(&error));
    QCOMPARE(read(source.rootDirectory() + "/nested/file.bin"), data);
  }
  void existingOrActiveTargetPreserved() {
    QTemporaryDir dir;
    HarnessFileContext source(dir.path(), "source"),
        target(dir.path(), "target");
    QVERIFY(write(target.rootDirectory() + "/keep.txt", "KEEP"));
    QString error;
    QVERIFY(!source.cloneTo("target", &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(read(target.rootDirectory() + "/keep.txt"), QByteArray("KEEP"));
    QVERIFY(!source.cloneTo("source", &error));
  }
  void duplicateOwnerCannotRemoveFiles() {
    QTemporaryDir dir;
    HarnessFileContext owner(dir.path(), "owner");
    QVERIFY(write(owner.rootDirectory() + "/keep", "KEEP"));
    HarnessFileContext duplicate(dir.path(), "owner");
    QVERIFY(duplicate.rootDirectory().isEmpty());
    QString error;
    QVERIFY(!duplicate.remove(&error));
    QCOMPARE(read(owner.rootDirectory() + "/keep"), QByteArray("KEEP"));
  }
  void invalidOwnerDoesNotEscape() {
    QTemporaryDir dir;
    HarnessFileContext invalid(dir.path(), "../escape");
    QVERIFY(invalid.rootDirectory().isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath("escape")));
  }
  void rootPinnedAndReplacementRefused() {
    QTemporaryDir dir;
    HarnessFileContext owner(dir.path(), "owner");
    const auto root = owner.rootDirectory();
    QVERIFY(!QDir().rename(root, root + "-moved"));
    QString error;
    QVERIFY(owner.remove(&error));
    QVERIFY(QDir().mkdir(root));
    QVERIFY(write(root + "/replacement", "NEW"));
    QVERIFY(!owner.remove(&error));
    QCOMPARE(read(root + "/replacement"), QByteArray("NEW"));
  }
  void linkedEntriesFailBeforeDeletingOrdinaryFiles() {
    QTemporaryDir dir;
    HarnessFileContext owner(dir.path(), "owner");
    const auto outside = dir.filePath("outside.bin");
    QVERIFY(write(outside, "OUTSIDE"));
    QVERIFY(write(owner.rootDirectory() + "/ordinary.bin", "ORDINARY"));
    const auto link = owner.rootDirectory() + "/linked.bin";
    QVERIFY(CreateHardLinkW(
        QDir::toNativeSeparators(link).toStdWString().c_str(),
        QDir::toNativeSeparators(outside).toStdWString().c_str(), nullptr));
    QString error;
    QVERIFY(!owner.cloneTo("target", &error));
    QVERIFY(!owner.remove(&error));
    QCOMPARE(read(outside), QByteArray("OUTSIDE"));
    QCOMPARE(read(owner.rootDirectory().isEmpty()
                      ? QFileInfo(link).absolutePath() + "/ordinary.bin"
                      : owner.rootDirectory() + "/ordinary.bin"),
             QByteArray("ORDINARY"));
    QVERIFY(QFile::remove(link));
    QVERIFY2(owner.remove(&error), qPrintable(error));
  }
  void directoryJunctionNeverFollowed() {
    QTemporaryDir dir;
    HarnessFileContext owner(dir.path(), "owner");
    const auto outside = dir.filePath("outside");
    QVERIFY(QDir().mkdir(outside));
    QVERIFY(write(outside + "/keep", "OUTSIDE"));
    const auto link = owner.rootDirectory() + "/alias";
    QCOMPARE(QProcess::execute("cmd.exe", {"/c", "mklink", "/J",
                                           QDir::toNativeSeparators(link),
                                           QDir::toNativeSeparators(outside)}),
             0);
    QString error;
    QVERIFY(!owner.cloneTo("target", &error));
    QVERIFY(!owner.remove(&error));
    QCOMPARE(read(outside + "/keep"), QByteArray("OUTSIDE"));
    QVERIFY(RemoveDirectoryW(
        QDir::toNativeSeparators(link).toStdWString().c_str()));
    QVERIFY(owner.remove(&error));
  }
  void sourceWriterRejectsCloneWithoutChangingSource() {
    QTemporaryDir dir;
    HarnessFileContext source(dir.path(), "source");
    const auto path = source.rootDirectory() + "/live";
    QFile writer(path);
    QVERIFY(writer.open(QIODevice::WriteOnly));
    QCOMPARE(writer.write("ACTIVE"), 6);
    QVERIFY(writer.flush());
    QString error;
    QVERIFY(!source.cloneTo("target", &error));
    writer.close();
    QCOMPARE(read(path), QByteArray("ACTIVE"));
    QVERIFY2(source.cloneTo("target", &error), qPrintable(error));
  }
  void preexistingCloneStageIsNeverOwnedOrRemoved() {
    QTemporaryDir dir;
    HarnessFileContext source(dir.path(), "source");
    QVERIFY(write(source.rootDirectory() + "/collision", "SOURCE"));
    QString foreign;
    source.beforeStageCreateForTest = [&](const QString &stage) {
      foreign = stage;
      QVERIFY(QDir().mkdir(stage));
      QVERIFY(write(stage + "/collision", "FOREIGN"));
    };
    QString error;
    QVERIFY(!source.cloneTo("target", &error));
    QVERIFY2(QFileInfo::exists(foreign),
             "A foreign precreated stage must survive failed clone.");
    QCOMPARE(read(foreign + "/collision"), QByteArray("FOREIGN"));
    QCOMPARE(read(source.rootDirectory() + "/collision"), QByteArray("SOURCE"));
  }
  void cloneBoundDoesNotLeavePublishedTarget() {
    QTemporaryDir dir;
    HarnessFileContext source(dir.path(), "source");
    const auto path = source.rootDirectory() + "/large";
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.resize(33 * 1024 * 1024));
    file.close();
    QString error;
    QVERIFY(!source.cloneTo("target", &error));
    QVERIFY(error.contains("32 MiB"));
    QVERIFY(QFile::remove(path));
    QVERIFY2(source.cloneTo("target", &error), qPrintable(error));
  }
};
QTEST_GUILESS_MAIN(HarnessFilesTest)
#include "test_harness_files.moc"
