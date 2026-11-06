#include "editor/CodeEditor.h"
#include "git/GitPanel.h"
#include "git/ProcessRunner.h"
#include "ui/AppFonts.h"
#include "ui/GitDiffTab.h"
#include "ui/MainWindow.h"
#include "ui/AppStyle.h"
#include "ui/SettingsPage.h"
#include "ui/WorkbenchSplitter.h"
#include "ui/WorkspaceChangesController.h"
#include "ui/WorkspaceChangesView.h"
#include <QApplication>
#include <QFile>
#include <QListView>
#include <QProcess>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
bool put(const QString &path, const QByteArray &bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
struct Fixture {
  QTemporaryDir temporary;
  QString root, data, error;
  Fixture() {
    root = temporary.filePath("repository");
    data = temporary.filePath("profile");
    QDir().mkpath(root);
    QDir().mkpath(data);
    run({"init", "-b", "main"});
    run({"config", "user.name", "ADE fixture"});
    run({"config", "user.email", "fixture@example.invalid"});
    run({"config", "commit.gpgsign", "false"});
    run({"config", "core.autocrlf", "false"});
    run({"config", "core.hooksPath", temporary.filePath("hooks")});
    put(root + "/original.txt", "committed\n");
    run({"add", "."});
    run({"commit", "-m", "base"});
  }
  bool run(const QStringList &args) {
    QProcess process;
    auto env = gitdetail::safeEnvironment();
    env.insert("GIT_CONFIG_NOSYSTEM", "1");
    env.insert("GIT_CONFIG_GLOBAL", temporary.filePath("no-global"));
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(root);
    process.start("git", args);
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
      error = QString::fromUtf8(process.readAllStandardError());
      return false;
    }
    return true;
  }
};
void openChanges(MainWindow &window) {
  window.findChild<WorkbenchSplitter *>()->setFileTreeVisible(true);
  window.findChild<QToolButton *>("fileTreeChangesMode")->click();
}
} // namespace
class WorkspaceChangesHostTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { AppFonts::load(); }
  void sidebarOpensExactStagedRenameAndClosesTab();
  void modeRestoresAndNonGitReturnsToFiles();
  void sourceControlUsesSharedDiffTab();
  void currentGitRowsRefreshAndRender();
};
void WorkspaceChangesHostTests::sidebarOpensExactStagedRenameAndClosesTab() {
  Fixture fixture;
  QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
  QVERIFY(fixture.run({"mv", "original.txt", "renamed.txt"}));
  QVERIFY(put(fixture.root + "/renamed.txt", "working after stage\n"));
  MainWindow window(fixture.data);
  window.resize(1200, 800);
  window.connectWorkspace(fixture.root);
  window.show();
  openChanges(window);
  auto *view = window.findChild<WorkspaceChangesView *>();
  QVERIFY(view);
  QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 15000);
  const auto index = view->model()->index(0);
  QCOMPARE(index.data(WorkspaceChangesModel::OldPathRole).toString(),
           "original.txt");
  QVERIFY(index.data(WorkspaceChangesModel::StagedRole).toBool());
  QTest::mouseClick(view->list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                    view->list()->visualRect(index).center());
  QTRY_VERIFY_WITH_TIMEOUT(window.findChild<GitDiffTab *>(), 10000);
  auto *tab = window.findChild<GitDiffTab *>();
  QTRY_VERIFY_WITH_TIMEOUT(tab->ready(), 15000);
  QCOMPARE(tab->findChild<CodeEditor *>("gitDiffOriginal")->toPlainText(),
           "committed\n");
  QCOMPARE(tab->findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "committed\n");
  QVERIFY(tab->staged());
  auto *tabs = window.findChild<QTabWidget *>("contentTabs");
  QVERIFY(tabs);
  QCOMPARE(tabs->currentWidget(), tab);
  const auto count = tabs->count();
  auto *git = window.findChild<GitPanel *>();
  QVERIFY(git);
  emit git->openDiffRequested(fixture.root, "renamed.txt", true,
                              "original.txt");
  QCOMPARE(tabs->count(), count);
  QPointer<GitDiffTab> weak = tab;
  emit tabs->tabCloseRequested(tabs->indexOf(tab));
  QTRY_VERIFY(!weak);
  window.close();
}
void WorkspaceChangesHostTests::modeRestoresAndNonGitReturnsToFiles() {
  Fixture fixture;
  {
    MainWindow window(fixture.data);
    window.connectWorkspace(fixture.root);
    window.show();
    openChanges(window);
    QTRY_VERIFY_WITH_TIMEOUT(
        window.findChild<WorkspaceChangesController *>()->hasSnapshot(), 15000);
    window.close();
  }
  {
    MainWindow window(fixture.data);
    window.show();
    QVERIFY(
        window.findChild<QToolButton *>("fileTreeChangesMode")->isChecked());
    QVERIFY(window.findChild<WorkspaceChangesView *>()->isVisible());
    const auto plain = fixture.temporary.filePath("plain");
    QDir().mkpath(plain);
    window.connectWorkspace(plain);
    QTRY_VERIFY_WITH_TIMEOUT(
        window.findChild<QToolButton *>("fileTreeFilesMode")->isChecked(),
        15000);
    QVERIFY(!window.findChild<WorkspaceChangesView *>()->isVisible());
    window.close();
  }
  QSettings prefs(fixture.data + "/window.ini", QSettings::IniFormat);
  QCOMPARE(prefs.value("fileTreeViewMode").toString(), "files");
}
void WorkspaceChangesHostTests::sourceControlUsesSharedDiffTab() {
  Fixture fixture;
  QVERIFY(put(fixture.root + "/original.txt", "staged value\n"));
  QVERIFY(fixture.run({"add", "original.txt"}));
  MainWindow window(fixture.data);
  window.connectWorkspace(fixture.root);
  auto *git = window.findChild<GitPanel *>();
  QVERIFY(git);
  emit git->openDiffRequested(fixture.root, "original.txt", true, {});
  auto *tab = window.findChild<GitDiffTab *>();
  QVERIFY(tab);
  QTRY_VERIFY_WITH_TIMEOUT(tab->ready(), 15000);
  QCOMPARE(tab->findChild<CodeEditor *>("gitDiffModified")->toPlainText(),
           "staged value\n");
  const auto outside = fixture.temporary.filePath("outside");
  QDir().mkpath(outside);
  emit git->openDiffRequested(outside, "original.txt", false, {});
  QCOMPARE(window.findChildren<GitDiffTab *>().size(), 1);
  window.close();
}
void WorkspaceChangesHostTests::currentGitRowsRefreshAndRender() {
  Fixture fixture;
  QVERIFY(put(fixture.root + "/src/new-file.cpp", "int added = 42;\n"));
  QVERIFY(put(fixture.root + "/original.txt", "new working value\n"));
  MainWindow window(fixture.data);
  window.resize(1200, 800);
  window.connectWorkspace(fixture.root);
  window.show();
  openChanges(window);
  auto *view = window.findChild<WorkspaceChangesView *>();
  QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 2, 15000);
  auto *controller = window.findChild<WorkspaceChangesController *>();
  QVERIFY(controller);
  QVERIFY(put(fixture.root + "/another.txt", "external\n"));
  controller->requestRefresh();
  QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 3, 15000);
  const auto directory = qEnvironmentVariable("ADE_CHANGED_FILES_CAPTURE_DIR");
  if (!directory.isEmpty()) {
    QDir().mkpath(directory);
    QTest::qWait(60);
    QVERIFY(window.grab().save(directory + "/host-changed-files-sidebar.png"));
    const auto index =
        view->model()->index(view->model()->indexForKey("w:original.txt"));
    QVERIFY(index.isValid());
    QTest::mouseClick(view->list()->viewport(), Qt::LeftButton, Qt::NoModifier,
                      view->list()->visualRect(index).center());
    QTRY_VERIFY(window.findChild<GitDiffTab *>());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<GitDiffTab *>()->ready(), 15000);
    QTest::qWait(60);
    QVERIFY(window.grab().save(directory + "/host-changed-files-diff-tab.png"));
  }
  window.close();
}
int main(int argc,char** argv){QApplication app(argc,argv);ade::initializeAppStyle(app);WorkspaceChangesHostTests test;return QTest::qExec(&test,argc,argv);}
#include "test_workspace_changes_host.moc"
