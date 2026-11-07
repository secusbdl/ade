#include "editor/CodeEditor.h"
#include "git/GitService.h"
#include "git/ProcessRunner.h"
#include "ui/GitDiffTab.h"
#include "ui/WorkspaceChangesController.h"
#include "ui/WorkspaceChangesView.h"
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QListView>
#include <QProcess>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
bool put(const QString &path, const QByteArray &bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
struct Repo {
  QTemporaryDir temp;
  QString root, error;
  Repo() {
    root = temp.filePath("repo");
    QDir().mkpath(root);
    run({"init", "-b", "main"});
    run({"config", "user.name", "ADE fixture"});
    run({"config", "user.email", "fixture@example.invalid"});
    run({"config", "commit.gpgsign", "false"});
    run({"config", "core.autocrlf", "false"});
    run({"config", "core.hooksPath", temp.filePath("hooks")});
    put(root + "/one.txt", "base\n");
    run({"add", "."});
    run({"commit", "-m", "base"});
  }
  bool run(const QStringList &arguments) {
    QProcess process;
    auto env = gitdetail::safeEnvironment();
    env.insert("GIT_CONFIG_NOSYSTEM", "1");
    env.insert("GIT_CONFIG_GLOBAL", temp.filePath("empty-config"));
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(root);
    process.start("git", arguments);
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
      error = QString::fromUtf8(process.readAllStandardError());
      return false;
    }
    return true;
  }
  GitWorkspaceContext context() const {
    GitWorkspaceContext c;
    c.trustedRoots = {root};
    return c;
  }
};
GitResult waitResult(GitService &service, quint64 id) {
  QSignalSpy spy(&service, &GitService::completed);
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 10000) {
    for (const auto &event : spy)
      if (event[0].toULongLong() == id)
        return qvariant_cast<GitResult>(event[1]);
    QTest::qWait(5);
  }
  GitResult timeout;
  timeout.error.message = "Fixture timed out";
  return timeout;
}
} // namespace
class WorkspaceChangesTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    const auto font = QFINDTESTDATA("../assets/fonts/Geist-Variable.ttf");
    QVERIFY(!font.isEmpty());
    QVERIFY(QFontDatabase::addApplicationFont(font) >= 0);
    qApp->setFont(QFont("Geist", 10));
  }
  void realMergedSummaryAndStageSnapshots();
  void renameAndDeletedRows();
  void untrackedBinaryAndNonRepository();
  void refreshRetainsSnapshotOnError();
  void switchingWorkspaceRejectsOldRequests();
  void unchangedAliasCannotRetargetAuthority();
  void unavailableWorkspaceRefreshIsIdempotentAndCanRecover();
  void visibilityStopsPolling();
  void controllerObserverCanDeleteOrReplace();
  void diffRejectsTraversalAndUnknownRoot();
  void diffReplacementAndDestruction();
  void selectionAndScrollSurviveRefresh();
  void deletionAndWorkspaceSwitchFromSelection();
  void modelReentryDoesNotOverwriteNewerRows();
  void renderLightDarkNarrow();
};
void WorkspaceChangesTests::realMergedSummaryAndStageSnapshots() {
  Repo repo;
  QVERIFY2(repo.error.isEmpty(), qPrintable(repo.error));
  QVERIFY(put(repo.root + "/one.txt", "index\n"));
  QVERIFY(repo.run({"add", "one.txt"}));
  QVERIFY(put(repo.root + "/one.txt", "working\nextra\n"));
  GitService service;
  service.setWorkspaceContext(repo.context());
  WorkspaceChangesController controller(&service);
  controller.setWorkspace(repo.root);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  QCOMPARE(controller.summary().entries.size(), 1);
  const auto entry = controller.summary().entries.first();
  QVERIFY(entry.staged);
  QCOMPARE(entry.path, "one.txt");
  QCOMPARE(entry.added, 3);
  QCOMPARE(entry.deleted, 2);
  GitDiffTab tab(&service);
  tab.load(repo.root, "one.txt", true);
  QTRY_VERIFY_WITH_TIMEOUT(tab.ready(), 10000);
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffOriginal")->toPlainText(),
           "base\n");
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "index\n");
  QVERIFY(tab.findChild<CodeEditor *>("gitDiffModified")->isReadOnly());
  tab.load(repo.root, "one.txt", false);
  QTRY_VERIFY_WITH_TIMEOUT(tab.ready(), 10000);
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "working\nextra\n");
}
void WorkspaceChangesTests::renameAndDeletedRows() {
  Repo repo;
  QVERIFY(repo.run({"mv", "one.txt", "renamed.txt"}));
  GitService service;
  service.setWorkspaceContext(repo.context());
  auto result = waitResult(service, service.diff(repo.root));
  QVERIFY2(result.ok, qPrintable(result.error.message));
  QCOMPARE(result.diff.entries.size(), 1);
  const auto entry = result.diff.entries.first();
  QCOMPARE(entry.oldPath, "one.txt");
  QCOMPARE(entry.status, 'R');
  GitDiffTab tab(&service);
  tab.load(repo.root, entry.path, true, entry.oldPath);
  QTRY_VERIFY_WITH_TIMEOUT(tab.ready(), 10000);
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffOriginal")->toPlainText(),
           "base\n");
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "base\n");
  WorkspaceChangesView view;
  view.resize(300, 250);
  view.setSnapshot(repo.root, WorkspaceChangesController::Phase::Ready,
                   result.diff, true, {}, false);
  view.show();
  QSignalSpy opened(&view, &WorkspaceChangesView::openRequested);
  QTest::mouseClick(view.list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                    view.list()->visualRect(view.model()->index(0)).center());
  QCOMPARE(opened.size(), 1);
  QCOMPARE(opened[0][3].toString(), "one.txt");
  auto deleted = result.diff;
  deleted.entries[0].status = 'D';
  view.setSnapshot(repo.root, WorkspaceChangesController::Phase::Ready, deleted,
                   true, {}, false);
  QTest::mouseClick(view.list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                    view.list()->visualRect(view.model()->index(0)).center());
  QCOMPARE(opened.size(), 1);
}
void WorkspaceChangesTests::untrackedBinaryAndNonRepository() {
  Repo repo;
  QVERIFY(put(repo.root + "/new.bin", QByteArray("a\0b", 3)));
  GitService service;
  service.setWorkspaceContext(repo.context());
  WorkspaceChangesController controller(&service);
  controller.setWorkspace(repo.root);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  QCOMPARE(controller.summary().entries.first().status, '?');
  QVERIFY(controller.summary().entries.first().binary);
  GitDiffTab tab(&service);
  tab.load(repo.root, "new.bin", false);
  QTRY_VERIFY_WITH_TIMEOUT(!tab.busy(), 10000);
  QVERIFY(!tab.ready());
  QVERIFY(tab.error().contains("Binary"));
  const auto plain = repo.temp.filePath("plain");
  QDir().mkpath(plain);
  auto context = repo.context();
  context.trustedRoots.append(plain);
  service.setWorkspaceContext(context);
  controller.setWorkspace(plain);
  QTRY_COMPARE_WITH_TIMEOUT(controller.phase(),
                            WorkspaceChangesController::Phase::NotRepository,
                            10000);
  QVERIFY(!controller.hasSnapshot());
  QVERIFY(controller.summary().entries.isEmpty());
}
void WorkspaceChangesTests::refreshRetainsSnapshotOnError() {
  Repo repo;
  QVERIFY(put(repo.root + "/one.txt", "changed\n"));
  GitService service;
  service.setWorkspaceContext(repo.context());
  WorkspaceChangesController controller(&service);
  controller.setWorkspace(repo.root);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  service.setWorkspaceContext({});
  controller.requestRefresh();
  QTRY_COMPARE_WITH_TIMEOUT(controller.phase(),
                            WorkspaceChangesController::Phase::Error, 10000);
  QVERIFY(controller.hasSnapshot());
  QCOMPARE(controller.summary().entries.first().path, "one.txt");
}
void WorkspaceChangesTests::switchingWorkspaceRejectsOldRequests() {
  Repo first, second;
  QVERIFY(put(first.root + "/first.txt", "first"));
  QVERIFY(put(second.root + "/second.txt", "second"));
  GitService service;
  auto context = first.context();
  context.trustedRoots.append(second.root);
  service.setWorkspaceContext(context);
  WorkspaceChangesController controller(&service);
  controller.setWorkspace(first.root);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.busy(), 10000);
  controller.setWorkspace(second.root);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  QCOMPARE(controller.summary().entries.size(), 1);
  QCOMPARE(controller.summary().entries.first().path, "second.txt");
  QTest::qWait(150);
  QCOMPARE(controller.summary().entries.first().path, "second.txt");
}
void WorkspaceChangesTests::visibilityStopsPolling() {
  Repo repo;
  GitService service;
  service.setWorkspaceContext(repo.context());
  WorkspaceChangesController controller(&service);
  controller.setPollInterval(25);
  controller.setWorkspace(repo.root);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  controller.setActive(false);
  QTest::qWait(150);
  QSignalSpy completed(&service, &GitService::completed);
  QVERIFY(put(repo.root + "/new.txt", "new"));
  controller.requestRefresh();
  QTest::qWait(180);
  QCOMPARE(completed.size(), 0);
  controller.setActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(controller.summary().entries.size(), 1, 10000);
}
void WorkspaceChangesTests::unavailableWorkspaceRefreshIsIdempotentAndCanRecover() {
  QTemporaryDir directory;
  const auto missing = directory.filePath("temporarily-unavailable");
  GitService service;
  WorkspaceChangesController controller(&service);
  int notifications = 0;
  connect(&controller, &WorkspaceChangesController::changed, &controller, [&] {
    ++notifications;
    // MainWindow refreshes its workbench in response to this signal. Bound the
    // fixture so a regression reports a failure instead of overflowing its stack.
    if (notifications < 8) controller.setWorkspace(missing);
  });
  controller.setWorkspace(missing);
  QCOMPARE(notifications, 1);
  QCOMPARE(controller.phase(), WorkspaceChangesController::Phase::Error);
  QVERIFY(!controller.error().isEmpty());
  for (int i = 0; i < 10; ++i) controller.setWorkspace(missing);
  QCOMPARE(notifications, 1);
  QVERIFY(QDir().mkpath(missing));
  controller.setWorkspace(missing);
  QCOMPARE(notifications, 2);
  QCOMPARE(controller.phase(), WorkspaceChangesController::Phase::Loading);
  QCOMPARE(controller.workspace(), GitService::canonicalPath(missing));
  QVERIFY(controller.error().isEmpty());
}
void WorkspaceChangesTests::unchangedAliasCannotRetargetAuthority() {
  Repo first, second;
  QVERIFY(put(first.root + "/first.txt", "first"));
  QVERIFY(put(second.root + "/second.txt", "second"));
  const auto alias = first.temp.filePath("alias");
  const auto link = [&](const QString &target) {
    QProcess process;
    process.start("cmd.exe",
                  {"/c", "mklink", "/J", QDir::toNativeSeparators(alias),
                   QDir::toNativeSeparators(target)});
    return process.waitForFinished(5000) && process.exitCode() == 0;
  };
  QVERIFY(link(first.root));
  struct RemoveLink {
    QString path;
    ~RemoveLink() { QDir().rmdir(path); }
  } cleanup{alias};
  GitService service;
  GitWorkspaceContext context;
  context.trustedRoots = {alias};
  service.setWorkspaceContext(context);
  WorkspaceChangesController controller(&service);
  controller.setWorkspace(alias);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.hasSnapshot(), 10000);
  QCOMPARE(controller.summary().entries.first().path, "first.txt");
  controller.setActive(false);
  QVERIFY(QDir().rmdir(alias));
  QVERIFY(link(second.root));
  controller.setWorkspace(alias);
  controller.setActive(true);
  QTRY_VERIFY_WITH_TIMEOUT(controller.phase() !=
                                   WorkspaceChangesController::Phase::Loading &&
                               !controller.busy(),
                           10000);
  QCOMPARE(controller.phase(), WorkspaceChangesController::Phase::Error);
  QVERIFY(controller.summary().entries.isEmpty());
}
void WorkspaceChangesTests::controllerObserverCanDeleteOrReplace() {
  Repo repo;
  GitService service;
  service.setWorkspaceContext(repo.context());
  auto *destroyed = new WorkspaceChangesController(&service);
  QPointer<WorkspaceChangesController> weak = destroyed;
  connect(destroyed, &WorkspaceChangesController::changed, destroyed,
          [destroyed] { delete destroyed; });
  destroyed->setWorkspace(repo.root);
  QVERIFY(!weak);
  WorkspaceChangesController controller(&service);
  connect(&controller, &WorkspaceChangesController::changed, &controller, [&] {
    if (controller.hasSnapshot())
      controller.setWorkspace({});
  });
  controller.setWorkspace(repo.root);
  controller.setActive(true);
  QTRY_COMPARE_WITH_TIMEOUT(controller.phase(),
                            WorkspaceChangesController::Phase::NoWorkspace,
                            10000);
  QVERIFY(controller.summary().entries.isEmpty());
  auto *goneService = new GitService;
  auto *orphan = new WorkspaceChangesController(goneService);
  QPointer<WorkspaceChangesController> orphanWeak = orphan;
  connect(orphan, &WorkspaceChangesController::changed, orphan,
          [orphan] { delete orphan; });
  delete goneService;
  QVERIFY(!orphanWeak);
}
void WorkspaceChangesTests::diffRejectsTraversalAndUnknownRoot() {
  Repo repo;
  GitService service;
  service.setWorkspaceContext(repo.context());
  GitDiffTab tab(&service);
  tab.load(repo.root, "../outside.txt", false);
  QTRY_VERIFY_WITH_TIMEOUT(!tab.busy(), 10000);
  QVERIFY(!tab.ready());
  QVERIFY(!tab.error().isEmpty());
  const auto unknown = repo.temp.filePath("unknown");
  QDir().mkpath(unknown);
  tab.load(unknown, "one.txt", false);
  QTRY_VERIFY_WITH_TIMEOUT(!tab.busy(), 10000);
  QVERIFY(!tab.ready());
  QVERIFY(!tab.error().isEmpty());
}
void WorkspaceChangesTests::diffReplacementAndDestruction() {
  Repo repo;
  QVERIFY(put(repo.root + "/other.txt", "other\n"));
  GitService service;
  service.setWorkspaceContext(repo.context());
  GitDiffTab tab(&service);
  tab.load(repo.root, "one.txt", false);
  tab.load(repo.root, "other.txt", false);
  QTRY_VERIFY_WITH_TIMEOUT(tab.ready(), 10000);
  QCOMPARE(tab.findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "other\n");
  auto *gone = new GitDiffTab(&service);
  QPointer<GitDiffTab> weak = gone;
  connect(gone, &GitDiffTab::stateChanged, gone, [gone] { delete gone; });
  gone->load(repo.root, "one.txt", false);
  QVERIFY(!weak);
  QTest::qWait(100);
}
void WorkspaceChangesTests::selectionAndScrollSurviveRefresh() {
  GitDiffSummary data;
  for (int i = 0; i < 100; ++i) {
    GitDiffEntry e;
    e.path = QString("folder/file-%1.txt").arg(i, 3, 10, QChar('0'));
    e.added = i;
    data.entries.append(e);
  }
  WorkspaceChangesView view;
  view.resize(240, 250);
  view.setSnapshot("C:/fixture/a", WorkspaceChangesController::Phase::Ready,
                   data, true, {}, false);
  view.show();
  QTest::qWait(20);
  view.list()->setCurrentIndex(view.model()->index(70));
  view.list()->scrollTo(view.model()->index(70));
  const int scroll = view.list()->verticalScrollBar()->value();
  const auto viewport = view.list()->viewport()->geometry();
  view.setSnapshot("C:/fixture/a", WorkspaceChangesController::Phase::Ready,
                   data, true, {}, true);
  QApplication::processEvents();
  QCOMPARE(view.list()->viewport()->geometry(), viewport);
  QSignalSpy reset(view.model(), &QAbstractItemModel::modelReset);
  data.entries[0].added = 999;
  view.setSnapshot("C:/fixture/a", WorkspaceChangesController::Phase::Ready,
                   data, true, {}, false);
  QCOMPARE(reset.size(), 0);
  QCOMPARE(view.list()->currentIndex().row(), 70);
  QCOMPARE(view.list()->verticalScrollBar()->value(), scroll);
  GitDiffEntry extra;
  extra.path = "aaa.txt";
  data.entries.prepend(extra);
  view.setSnapshot("C:/fixture/a", WorkspaceChangesController::Phase::Ready,
                   data, true, {}, false);
  QCOMPARE(view.list()
               ->currentIndex()
               .data(WorkspaceChangesModel::PathRole)
               .toString(),
           "folder/file-070.txt");
  view.setSnapshot("C:/fixture/b", WorkspaceChangesController::Phase::Ready, {},
                   true, {}, false);
  QVERIFY(!view.list()->currentIndex().isValid());
  QCOMPARE(view.list()->verticalScrollBar()->value(), 0);
}
void WorkspaceChangesTests::deletionAndWorkspaceSwitchFromSelection() {
  GitDiffSummary data;
  GitDiffEntry item;
  item.path = "file.txt";
  data.entries = {item};
  auto *view = new WorkspaceChangesView;
  view->setSnapshot("a", WorkspaceChangesController::Phase::Ready, data, true,
                    {}, false);
  view->list()->setCurrentIndex(view->model()->index(0));
  QPointer<WorkspaceChangesView> weak = view;
  connect(view->list()->selectionModel(), &QItemSelectionModel::currentChanged,
          view, [view] { delete view; });
  view->setSnapshot("b", WorkspaceChangesController::Phase::Ready, data, true,
                    {}, false);
  QVERIFY(!weak);
}
void WorkspaceChangesTests::modelReentryDoesNotOverwriteNewerRows() {
  WorkspaceChangesModel model;
  GitDiffEntry first;
  first.path = "first";
  GitDiffEntry deferred;
  deferred.path = "deferred";
  GitDiffEntry newest;
  newest.path = "newest";
  bool once = false;
  connect(&model, &QAbstractItemModel::modelReset, &model, [&] {
    if (!once) {
      once = true;
      model.setEntries({deferred});
    }
  });
  model.setEntries({first});
  model.setEntries({newest});
  QTest::qWait(20);
  QCOMPARE(model.entry(0).path, "newest");
}
void WorkspaceChangesTests::renderLightDarkNarrow() {
  const auto directory = qEnvironmentVariable("ADE_CHANGED_FILES_CAPTURE_DIR");
  if (directory.isEmpty())
    return;
  QDir().mkpath(directory);
  GitDiffSummary data;
  for (int i = 0; i < 14; ++i) {
    GitDiffEntry e;
    e.path = QString("src/components/item-%1.cpp").arg(i);
    e.status = i == 0 ? 'R' : i == 1 ? '?' : 'M';
    e.oldPath = i == 0 ? "src/old-name.cpp" : QString();
    e.staged = i % 2 && e.status != '?';
    e.added = i + 2;
    e.deleted = i;
    data.entries.append(e);
  }
  WorkspaceChangesView view;
  view.resize(246, 430);
  view.setSnapshot("C:/fixture/project",
                   WorkspaceChangesController::Phase::Ready, data, true, {},
                   false);
  view.show();
  QTest::qWait(30);
  QVERIFY(view.grab().save(directory + "/changed-files-light-narrow.png"));
  QPalette palette = view.palette();
  palette.setColor(QPalette::Window, QColor("#181a1f"));
  palette.setColor(QPalette::Base, QColor("#181a1f"));
  palette.setColor(QPalette::Text, QColor("#e7e9ee"));
  palette.setColor(QPalette::WindowText, QColor("#e7e9ee"));
  view.setPalette(palette);
  view.setAutoFillBackground(true);
  view.list()->setPalette(palette);
  QTest::qWait(20);
  QVERIFY(view.grab().save(directory + "/changed-files-dark-narrow.png"));
}
QTEST_MAIN(WorkspaceChangesTests)
#include "test_workspace_changes.moc"
