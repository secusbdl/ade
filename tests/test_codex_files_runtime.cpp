#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include "runtime/HarnessFileContext.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider() {
  ProviderConfig p;
  p.id = "codex";
  p.name = "Codex fixture";
  p.kind = "codex";
  p.protocol = "codex-app-server";
  p.model = "model-a";
  p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                            .filePath("harness_fixture.exe");
  return p;
}
QString workspace(const AgentEngine &engine) {
  for (qsizetype i = engine.messages().size(); i-- > 0;) {
    const auto p = engine.messages()[i]
                       .toObject()["metadata"]
                       .toObject()["harness"]
                       .toObject()["workspace"]
                       .toString();
    if (!p.isEmpty())
      return p;
  }
  return {};
}
bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace
class CodexFilesRuntimeTest : public QObject {
  Q_OBJECT
private slots:
  void forkCopiesFallbackFilesAndDeleteStopsIdleHarness() {
    QTemporaryDir dir;
    AgentEngine source;
    source.setProvider(provider());
    AgentToolHost sourceHost(&source, nullptr, "source", dir.path());
    QSignalSpy done(&source, &AgentEngine::finished);
    QVERIFY(source.submitWithAttachments("first", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(source.lastTurnSucceeded());
    const auto root = workspace(source);
    QVERIFY(!root.isEmpty());
    QVERIFY(QDir().mkpath(root + "/nested"));
    QVERIFY(write(root + "/nested/file.bin", QByteArray("A\0B", 3)));
    const auto original = source.messages();
    QString error;
    QVERIFY2(sourceHost.cloneFileContext("fork", &error), qPrintable(error));
    QCOMPARE(source.messages(), original);
    AgentEngine fork;
    fork.setProvider(provider());
    fork.setMessages(original);
    AgentToolHost forkHost(&fork, nullptr, "fork", dir.path());
    QSignalSpy forkDone(&fork, &AgentEngine::finished);
    QVERIFY(fork.submitWithAttachments("fork", {}));
    QTRY_COMPARE(forkDone.size(), 1);
    QVERIFY(fork.lastTurnSucceeded());
    const auto forkRoot = workspace(fork);
    QVERIFY(forkRoot != root);
    QCOMPARE(read(forkRoot + "/nested/file.bin"), QByteArray("A\0B", 3));
    QVERIFY2(sourceHost.removeFileContext(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(root));
    QCOMPARE(source.messages(), original);
    QVERIFY(!source.submitWithAttachments("deleted owner", {}));
    QCOMPARE(source.messages(), original);
    QCOMPARE(read(forkRoot + "/nested/file.bin"), QByteArray("A\0B", 3));
    QVERIFY2(forkHost.removeFileContext(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(forkRoot));
  }
  void existingTargetSurvivesFailedFork() {
    QTemporaryDir dir;
    AgentEngine source;
    AgentToolHost host(&source, nullptr, "source", dir.path());
    QString targetPath;
    {
      HarnessFileContext target(dir.path(), "target");
      targetPath = target.rootDirectory();
      QVERIFY(write(targetPath + "/keep", "KEEP"));
    }
    QString error;
    QVERIFY(!host.cloneFileContext("target", &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(read(targetPath + "/keep"), QByteArray("KEEP"));
  }
  void deletingChatNeverDeletesSelectedWorkspace() {
    QTemporaryDir data, selected;
    QVERIFY(write(selected.filePath("keep"), "USER_WORKSPACE"));
    AgentEngine source;
    source.setProvider(provider());
    source.setWorkspace(selected.path());
    AgentToolHost host(&source, nullptr, "source", data.path());
    QVERIFY(host.setWorkspace(source.workspace()));
    QSignalSpy done(&source, &AgentEngine::finished);
    QVERIFY(source.submitWithAttachments("selected workspace", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(source.lastTurnSucceeded());
    QString error;
    QVERIFY2(host.removeFileContext(&error), qPrintable(error));
    QCOMPARE(read(selected.filePath("keep")), QByteArray("USER_WORKSPACE"));
  }
};
QTEST_GUILESS_MAIN(CodexFilesRuntimeTest)
#include "test_codex_files_runtime.moc"
