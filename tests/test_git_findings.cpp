#include "git/GitParsing.h"
#include "git/GitService.h"
#include "git/ProcessRunner.h"
#include "git/WorktreeSetupRunner.h"
#include "ui/WorkspaceChangesView.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QListView>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace ade;
using namespace Qt::Literals::StringLiterals;
namespace {
bool write(const QString &path, const QByteArray &bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}
struct Fixture {
  QTemporaryDir base;
  QString root, managed, git, error;
  Fixture(bool commit = true) {
    root = base.filePath("repository with spaces");
    managed = base.filePath("managed");
    git = QStandardPaths::findExecutable("git");
    QDir().mkpath(root);
    if (!run({"init", "-b", "main"}))
      return;
    run({"config", "user.name", "ADE fixture"});
    run({"config", "user.email", "fixture@example.invalid"});
    run({"config", "commit.gpgsign", "false"});
    run({"config", "core.autocrlf", "false"});
    run({"config", "core.hooksPath", base.filePath("hooks")});
    if (commit) {
      write(root + "/one.txt", "base\n");
      write(root + "/two.txt", "two\n");
      run({"add", "--", "."});
      run({"commit", "-m", "base"});
    }
  }
  bool run(const QStringList &args, const QString &cwd = {},
           QByteArray *out = nullptr) {
    const auto dir = cwd.isEmpty() ? root : cwd;
    if (!gitdetail::inside(base.path(), dir, true)) {
      error = "Fixture command escaped temporary root.";
      return false;
    }
    auto env = gitdetail::safeEnvironment();
    env.insert("GIT_CONFIG_NOSYSTEM", "1");
    env.insert("GIT_CONFIG_GLOBAL", base.filePath("no-global-config"));
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) {
          a->flags |= CREATE_NO_WINDOW;
        });
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(dir);
    process.start(git, args);
    if (!process.waitForFinished(15000) || process.exitCode() != 0) {
      error = QString::fromUtf8(process.readAllStandardError());
      return false;
    }
    if (out)
      *out = process.readAllStandardOutput();
    return true;
  }
  GitServiceOptions options() const {
    GitServiceOptions o;
    o.gitExecutable = git;
    o.managedWorktreeRoot = managed;
    o.readTimeoutMs = 10000;
    return o;
  }
  GitWorkspaceContext context() const {
    GitWorkspaceContext c;
    c.trustedRoots = {root};
    c.usageKnown = true;
    return c;
  }
};
GitResult waitFor(GitService &service, quint64 id, int timeout = 20000) {
  QSignalSpy spy(&service, &GitService::completed);
  QElapsedTimer timer;
  timer.start();
  do {
    for (const auto &row : spy)
      if (row[0].toULongLong() == id)
        return qvariant_cast<GitResult>(row[1]);
    QCoreApplication::processEvents();
    QTest::qWait(5);
  } while (timer.elapsed() < timeout);
  GitResult result;
  result.error = {GitErrorCode::Timeout, "Test wait timed out.", -1};
  return result;
}
WorktreeSetupRun waitSetup(QSignalSpy &spy, quint64 id, int timeout = 15000) {
  QElapsedTimer timer;
  timer.start();
  do {
    for (const auto &row : spy) {
      const auto run = qvariant_cast<WorktreeSetupRun>(row[0]);
      if (run.id == id && run.status != WorktreeSetupRun::Status::Running)
        return run;
    }
    QCoreApplication::processEvents();
    QTest::qWait(5);
  } while (timer.elapsed() < timeout);
  WorktreeSetupRun run;
  run.message = "Test wait timed out.";
  return run;
}
bool ok(const GitResult &result) {
  if (!result.ok)
    qWarning().noquote() << result.error.message;
  return result.ok;
}
} // namespace

class ClaudeGitFindingsTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    const auto font = QFINDTESTDATA("../assets/fonts/Geist-Variable.ttf");
    QVERIFY(!font.isEmpty());
    QVERIFY(QFontDatabase::addApplicationFont(font) >= 0);
    qApp->setFont(QFont("Geist", 10));
  }
  void oversizedUntrackedDoesNotHideOrdinaryChanges() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(write(f.root + "/ordinary.txt", "one\ntwo\n"));
    QFile large(f.root + "/oversized.bin");
    QVERIFY(large.open(QIODevice::WriteOnly));
    QVERIFY(large.resize(33 * 1024 * 1024));
    large.close();
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    bool ordinary = false, oversized = false;
    for (const auto &e : result.diff.entries) {
      if (e.path == "ordinary.txt") {
        ordinary = true;
        QCOMPARE(e.added, 2);
      }
      if (e.path == "oversized.bin") {
        oversized = true;
        QCOMPARE(e.added, 0);
      }
    }
    QVERIFY(ordinary);
    QVERIFY(oversized);
  }
  void unreadableUntrackedDoesNotHideOrdinaryChanges() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(write(f.root + "/ordinary.txt", "one\n"));
    QVERIFY(write(f.root + "/locked.txt", "locked\n"));
    const auto p = QDir::toNativeSeparators(f.root + "/locked.txt");
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(p.utf16()), GENERIC_READ,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    const auto close = qScopeGuard([&] { CloseHandle(h); });
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    bool ordinary = false, locked = false;
    for (const auto &e : result.diff.entries) {
      ordinary |= e.path == "ordinary.txt";
      locked |= e.path == "locked.txt";
    }
    QVERIFY(ordinary);
    QVERIFY(locked);
  }
  void nestedRepositoryEntryDoesNotAbortSummary() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(QDir().mkdir(f.root + "/nested"));
    QVERIFY(f.run({"init", "-b", "main"}, f.root + "/nested"));
    QVERIFY(write(f.root + "/nested/inside.txt", "nested\n"));
    QVERIFY(write(f.root + "/ordinary.txt", "one\n"));
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    bool ordinary = false;
    for (const auto &e : result.diff.entries)
      ordinary |= e.path == "ordinary.txt";
    QVERIFY(ordinary);
  }
  void bareMainAllowsLinkedWorktreeInspection() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    const auto bare = f.base.filePath("bare.git"),
               linked = f.base.filePath("linked");
    QVERIFY(f.run({"clone", "--bare", f.root, bare}));
    QVERIFY(f.run({"worktree", "add", "-b", "topic", linked, "main"}, bare));
    GitService service(f.options());
    auto context = f.context();
    context.trustedRoots << linked << bare;
    service.setWorkspaceContext(context);
    auto result = waitFor(service, service.inspect(linked));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    QVERIFY(result.snapshot.isWorktree);
    QCOMPARE(result.snapshot.mainWorktreePath, GitService::canonicalPath(bare));
    result = waitFor(service, service.deletionInfo(linked));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    QVERIFY(!result.deletion.main);
    QCOMPARE(result.deletion.branch, QString("topic"));
    QCOMPARE(waitFor(service, service.inspect(bare)).error.code,
             GitErrorCode::NotRepository);
  }
  void linkedBareSetupSaveUsesVerifiedAdministrativeRoot() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    const auto bare = f.base.filePath("bare.git"),
               linked = f.base.filePath("linked");
    QVERIFY(f.run({"clone", "--bare", f.root, bare}));
    QVERIFY(f.run({"worktree", "add", "-b", "topic", linked, "main"}, bare));
    GitService service(f.options());
    auto context = f.context();
    context.trustedRoots << linked;
    service.setWorkspaceContext(context);
    const auto saved = waitFor(
        service, service.saveSetupScript(linked, GitSetupVariant::PowerShell,
                                         "Write-Output 'fixture'\n", {}));
    QVERIFY2(saved.ok, qPrintable(saved.error.message));
    QCOMPARE(saved.path, GitService::canonicalPath(bare) +
                             "/.stagewise/worktree-setup.ps1");
    QCOMPARE(read(saved.path), QByteArray("Write-Output 'fixture'\n"));
  }
  void aggregateSummaryBudgetKeepsEveryEntryAndWarns() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(write(f.root + "/a.txt", QByteArray(800, 'a')));
    QVERIFY(write(f.root + "/b.txt", QByteArray(800, 'b')));
    QVERIFY(write(f.root + "/empty.txt", {}));
    auto options = f.options();
    options.outputLimit = 1024;
    GitService service(options);
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    QCOMPARE(result.diff.entries.size(), 3);
    QCOMPARE(result.warnings.size(), 1);
    for (const auto &e : result.diff.entries) {
      if (e.path == "a.txt")
        QCOMPARE(e.added, 1);
      else
        QCOMPARE(e.added, 0);
    }
    WorkspaceChangesController controller(&service);
    controller.setWorkspace(f.root);
    controller.setActive(true);
    QTRY_VERIFY(controller.hasSnapshot());
    QCOMPARE(controller.phase(), WorkspaceChangesController::Phase::Ready);
    QVERIFY(!controller.error().isEmpty());
    WorkspaceChangesView view;
    view.setSnapshot(f.root, controller.phase(), controller.summary(),
                     controller.hasSnapshot(), controller.error(), false);
    view.show();
    QVERIFY(view.findChild<QLabel *>("workspaceChangesStatus")->isVisible());
    QVERIFY(view.list()->isVisible());
    QCOMPARE(view.model()->rowCount(), 3);
  }
  void binarySniffBoundaryPreservesPinnedLineCounting() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QByteArray early(8000, 'x');
    early[7999] = 0;
    QByteArray late(8001, 'x');
    late[8000] = 0;
    late += '\n';
    QVERIFY(write(f.root + "/early.bin", early));
    QVERIFY(write(f.root + "/late.txt", late));
    QVERIFY(write(f.root + "/ordinary.txt", "first\nlast"));
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY(result.ok);
    QVERIFY(result.warnings.isEmpty());
    for (const auto &e : result.diff.entries) {
      if (e.path == "early.bin") {
        QVERIFY(e.binary);
        QCOMPARE(e.added, 0);
      } else if (e.path == "late.txt") {
        QVERIFY(!e.binary);
        QCOMPARE(e.added, 1);
      } else if (e.path == "ordinary.txt") {
        QCOMPARE(e.added, 2);
      }
    }
  }
  void foreignJunctionSummaryDoesNotReadForeignLines() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    const auto outside = f.base.filePath("outside");
    QVERIFY(QDir().mkdir(outside));
    QVERIFY(write(outside + "/secret.txt", "DO_NOT_COUNT\none\ntwo\n"));
    QVERIFY(write(f.root + "/ordinary.txt", "local\n"));
    QProcess create;
    create.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) {
          a->flags |= CREATE_NO_WINDOW;
        });
    create.start("cmd.exe", {"/d", "/c", "mklink", "/J",
                             QDir::toNativeSeparators(f.root + "/foreign"),
                             QDir::toNativeSeparators(outside)});
    QVERIFY(create.waitForFinished(5000));
    QCOMPARE(create.exitCode(), 0);
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    bool local = false, foreign = false;
    for (const auto &e : result.diff.entries) {
      if (e.path == "ordinary.txt") {
        local = true;
        QCOMPARE(e.added, 1);
      }
      if (e.path.startsWith("foreign")) {
        foreign = true;
        QCOMPARE(e.added, 0);
      }
    }
    QVERIFY(local);
    QVERIFY(foreign);
    QVERIFY(!result.warnings.isEmpty());
    const auto denied =
        waitFor(service, service.fileDiff(f.root, "foreign/secret.txt", false));
    QCOMPARE(denied.error.code, GitErrorCode::InvalidPath);
  }
  void stagedRenameSurvivesUnstagedModification() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(f.run({"mv", "one.txt", "renamed.txt"}));
    QVERIFY(write(f.root + "/renamed.txt", "base\nextra\n"));
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    bool found = false;
    for (const auto &e : result.diff.entries)
      if (e.path == "renamed.txt") {
        found = true;
        QCOMPARE(e.status, 'R');
        QCOMPARE(e.oldPath, QString("one.txt"));
        QVERIFY(e.staged);
        QCOMPARE(e.added, 1);
      }
    QVERIFY(found);
  }
  void prunableExistingDirectoryIsNotAnActiveWorktree() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    const auto linked = f.base.filePath("ghost");
    QVERIFY(f.run({"worktree", "add", "-b", "ghost", linked, "main"}));
    QVERIFY(QFile::remove(linked + "/.git"));
    QVERIFY(QDir(linked).exists());
    QByteArray porcelain;
    QVERIFY(f.run({"worktree", "list", "--porcelain", "-z"}, {}, &porcelain));
    QVERIFY2(porcelain.contains("prunable"), porcelain.constData());
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.worktrees(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    QCOMPARE(result.worktrees.size(), 1);
    QVERIFY(result.worktrees.first().main);
  }
  void conflictAndUntrackedStatusesAreDistinct() {
    Fixture f;
    QVERIFY2(f.error.isEmpty(), qPrintable(f.error));
    QVERIFY(f.run({"checkout", "-b", "other"}));
    QVERIFY(write(f.root + "/one.txt", "other\n"));
    QVERIFY(f.run({"commit", "-am", "other"}));
    QVERIFY(f.run({"checkout", "main"}));
    QVERIFY(write(f.root + "/one.txt", "ours\n"));
    QVERIFY(f.run({"commit", "-am", "ours"}));
    QVERIFY(!f.run({"merge", "--no-edit", "other"}));
    QVERIFY(write(f.root + "/one.txt", "ours\n"));
    QVERIFY(write(f.root + "/untracked.txt", "new\n"));
    GitService service(f.options());
    service.setWorkspaceContext(f.context());
    const auto result = waitFor(service, service.diff(f.root));
    QVERIFY2(result.ok, qPrintable(result.error.message));
    char conflict = 0, untracked = 0;
    for (const auto &e : result.diff.entries) {
      if (e.path == "one.txt")
        conflict = e.status;
      if (e.path == "untracked.txt")
        untracked = e.status;
    }
    QVERIFY(conflict);
    QVERIFY(untracked);
    QVERIFY2(conflict != untracked,
             qPrintable(QString("conflict=%1 untracked=%2")
                            .arg(QChar(conflict))
                            .arg(QChar(untracked))));
    QCOMPARE(conflict, 'U');
    QCOMPARE(untracked, '?');
    WorkspaceChangesModel model;
    model.setEntries(result.diff.entries);
    for (int i = 0; i < model.rowCount(); ++i) {
      const auto row = model.index(i);
      if (row.data(WorkspaceChangesModel::PathRole).toString() == "one.txt") {
        QVERIFY(
            row.data(Qt::AccessibleTextRole).toString().contains("Conflicted"));
        QVERIFY(
            !row.data(Qt::AccessibleTextRole).toString().contains("Untracked"));
        QCOMPARE(row.data(WorkspaceChangesModel::StatusRole).toString(),
                 QString("!"));
      } else if (row.data(WorkspaceChangesModel::PathRole).toString() ==
                 "untracked.txt") {
        QVERIFY(
            row.data(Qt::AccessibleTextRole).toString().contains("Untracked"));
        QCOMPARE(row.data(WorkspaceChangesModel::StatusRole).toString(),
                 QString("U"));
      }
    }
    const auto output = qEnvironmentVariable("ADE_GIT_FINDINGS_RENDER_DIR");
    if (!output.isEmpty()) {
      QDir().mkpath(output);
      WorkspaceChangesView view;
      view.resize(320, 300);
      view.setSnapshot(f.root, WorkspaceChangesController::Phase::Ready,
                       result.diff, true, {}, false);
      view.show();
      QTest::qWait(30);
      QVERIFY(view.grab().save(output + "/conflict-and-untracked.png"));
    }
  }
};
QTEST_MAIN(ClaudeGitFindingsTest)
#include "test_git_findings.moc"
