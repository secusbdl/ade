#include "editor/CodeEditor.h"
#include "editor/EditorWidget.h"
#include "ui/AppStyle.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/WorkbenchSplitter.h"
#include "ui/WorkspaceFileTreeModel.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
using namespace ade;
namespace {
bool put(const QString &path, const QByteArray &bytes) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
bool junction(const QString &path, const QString &target) {
  QProcess p;
  p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
    a->flags |= CREATE_NO_WINDOW;
  });
  p.start("cmd.exe",
          {"/d", "/c", "mklink", "/J", QDir::toNativeSeparators(path),
           QDir::toNativeSeparators(target)});
  return p.waitForFinished(5000) && p.exitCode() == 0;
}
Conversation chat(const QString &id, const QString &workspace) {
  Conversation c;
  c.id = id;
  c.title = id;
  c.titleLocked = true;
  c.workspace = workspace;
  c.created = c.updated = QDateTime::currentMSecsSinceEpoch();
  return c;
}
bool seed(const QString &data, const QList<Conversation> &chats) {
  if (!SessionStore(data).save(chats))
    return false;
  QSettings s(data + "/window.ini", QSettings::IniFormat);
  s.setValue("lastConversationId", chats.first().id);
  s.sync();
  return s.status() == QSettings::NoError;
}
void expose(MainWindow &window) {
  window.setAttribute(Qt::WA_DontShowOnScreen);
  window.resize(1150, 760);
  window.show();
  window.findChild<WorkbenchSplitter *>()->setFileTreeVisible(true);
}
bool select(MainWindow &window, const QString &id) {
  auto *list = window.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole).toString() == id) {
      list->setCurrentRow(i);
      return true;
    }
  return false;
}
QModelIndex named(WorkspaceFileTreeModel *m, const QModelIndex &root,
                  const QString &name) {
  for (int i = 0; i < m->rowCount(root); ++i) {
    auto n = m->index(i, 0, root);
    if (m->fileName(n) == name)
      return n;
  }
  return {};
}
} // namespace
class WorkspaceTreeHostTest : public QObject {
  Q_OBJECT
private slots:
  void ordinaryOpenDeduplicatesAndRootSwitchRejectsOldIndex() {
    QTemporaryDir t;
    const auto a = t.filePath("a"), b = t.filePath("b");
    QVERIFY(QDir().mkdir(a));
    QVERIFY(QDir().mkdir(b));
    QVERIFY(put(a + "/same.txt", "A_ORIGINAL"));
    QVERIFY(put(b + "/same.txt", "B_CURRENT"));
    const auto data = t.filePath("data");
    QVERIFY(seed(data, {chat("a", a), chat("b", b), chat("empty", {})}));
    MainWindow window(data);
    expose(window);
    QCOMPARE(window.currentSessionId(), QString("a"));
    auto *tree = window.findChild<QTreeView *>("workspaceFiles");
    auto *model = qobject_cast<WorkspaceFileTreeModel *>(tree->model());
    QVERIFY(model);
    QTRY_VERIFY(named(model, tree->rootIndex(), "same.txt").isValid());
    const auto old = named(model, tree->rootIndex(), "same.txt");
    emit tree->doubleClicked(old);
    auto *tabs = window.findChild<QTabWidget *>("contentTabs");
    auto *editor = qobject_cast<EditorWidget *>(tabs->currentWidget());
    QVERIFY(editor);
    QCOMPARE(editor->editor()->toPlainText(), QString("A_ORIGINAL"));
    const auto count = tabs->count();
    emit tree->doubleClicked(old);
    QCOMPARE(tabs->count(), count);
    QCOMPARE(tabs->currentWidget(), editor);
    QVERIFY(select(window, "b"));
    QCOMPARE(window.currentSessionId(), QString("b"));
    QTRY_VERIFY(named(model, tree->rootIndex(), "same.txt").isValid());
    emit tree->doubleClicked(old);
    QCOMPARE(tabs->count(), count);
    emit tree->doubleClicked(named(model, tree->rootIndex(), "same.txt"));
    QCOMPARE(tabs->count(), count + 1);
    auto *current = qobject_cast<EditorWidget *>(tabs->currentWidget());
    QVERIFY(current);
    QCOMPARE(current->editor()->toPlainText(), QString("B_CURRENT"));
    QVERIFY(select(window, "empty"));
    QVERIFY(!tree->model());
    QVERIFY(model->rootPath().isEmpty());
    QCOMPARE(model->rowCount(), 0);
  }
  void outsideJunctionAndReplacedEntryCannotOpen() {
    QTemporaryDir t;
    const auto root = t.filePath("root"), foreign = t.filePath("foreign");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(foreign));
    QVERIFY(put(foreign + "/secret.txt", "OUTSIDE_SENTINEL"));
    QVERIFY(junction(root + "/foreign", foreign));
    QVERIFY(QDir().mkdir(root + "/nested"));
    QVERIFY(put(root + "/nested/secret.txt", "INSIDE"));
    QVERIFY(seed(t.filePath("data"), {chat("a", root)}));
    MainWindow window(t.filePath("data"));
    expose(window);
    auto *tree = window.findChild<QTreeView *>("workspaceFiles");
    auto *model = qobject_cast<WorkspaceFileTreeModel *>(tree->model());
    QVERIFY(model);
    QTRY_VERIFY(named(model, tree->rootIndex(), "foreign").isValid());
    auto *tabs = window.findChild<QTabWidget *>("contentTabs");
    const auto count = tabs->count();
    emit tree->doubleClicked(named(model, tree->rootIndex(), "foreign"));
    QCOMPARE(tabs->count(), count);
    QVERIFY(!model->index(root + "/foreign/secret.txt").isValid());
    const auto saved = model->index(root + "/nested/secret.txt");
    QVERIFY(saved.isValid());
    QVERIFY(QFile::remove(root + "/nested/secret.txt"));
    QVERIFY(QDir().rmdir(root + "/nested"));
    QVERIFY(junction(root + "/nested", foreign));
    emit tree->doubleClicked(saved);
    QCOMPARE(tabs->count(), count);
    QVERIFY(window.findChildren<EditorWidget *>().isEmpty());
  }
  void watchersRefreshAndSourceGitMetadataIsHidden() {
    QTemporaryDir t;
    const auto root = t.filePath("root");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(root + "/.git"));
    QVERIFY(put(root + "/.gitignore", "out/"));
    QVERIFY(seed(t.filePath("data"), {chat("a", root)}));
    MainWindow window(t.filePath("data"));
    expose(window);
    auto *tree = window.findChild<QTreeView *>("workspaceFiles");
    auto *model = qobject_cast<WorkspaceFileTreeModel *>(tree->model());
    QVERIFY(model);
    QTRY_VERIFY(named(model, tree->rootIndex(), ".gitignore").isValid());
    QVERIFY(!named(model, tree->rootIndex(), ".git").isValid());
    QVERIFY(put(root + "/created.txt", "text"));
    QTRY_VERIFY(named(model, tree->rootIndex(), "created.txt").isValid());
    QVERIFY(QFile::rename(root + "/created.txt", root + "/renamed.txt"));
    QTRY_VERIFY(named(model, tree->rootIndex(), "renamed.txt").isValid());
    QVERIFY(!named(model, tree->rootIndex(), "created.txt").isValid());
    QVERIFY(QFile::remove(root + "/renamed.txt"));
    QTRY_VERIFY(!named(model, tree->rootIndex(), "renamed.txt").isValid());
  }
  void rootChangeObserverMayCloseHost() {
    QTemporaryDir t;
    const auto a = t.filePath("a"), b = t.filePath("b");
    QVERIFY(QDir().mkdir(a));
    QVERIFY(QDir().mkdir(b));
    QVERIFY(seed(t.filePath("data"), {chat("a", a)}));
    auto *window = new MainWindow(t.filePath("data"));
    QPointer<MainWindow> weak(window);
    auto *model = window->findChild<WorkspaceFileTreeModel *>();
    QVERIFY(model);
    connect(model, &WorkspaceFileTreeModel::rootIndexChanged, model,
            [window] { delete window; });
    window->connectWorkspace(b);
    QVERIFY(!weak);
  }
  void realThemeRenders_data() {
    QTest::addColumn<QString>("theme");
    QTest::newRow("light") << QString("light");
    QTest::newRow("dark") << QString("dark");
  }
  void realThemeRenders() {
    QFETCH(QString, theme);
    const auto output = qEnvironmentVariable("ADE_TREE_HOST_RENDER_DIR");
    if (output.isEmpty())
      QSKIP("Optional actual host renders not requested.");
    QTemporaryDir t;
    const auto root = t.filePath("fixture-workspace");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(root + "/src"));
    QVERIFY(put(root + "/src/main.cpp", "int main() {}\n"));
    QVERIFY(put(root + "/README.md", "Fixture"));
    QVERIFY(seed(t.filePath("data"), {chat("fixture", root)}));
    MainWindow window(t.filePath("data"));
    auto *settings = window.findChild<SettingsPage *>();
    QVERIFY(settings);
    QVERIFY(settings->setPreference("personalization.appearance", theme));
    expose(window);
    auto *tree = window.findChild<QTreeView *>("workspaceFiles");
    auto *model = qobject_cast<WorkspaceFileTreeModel *>(tree->model());
    QVERIFY(model);
    QTRY_VERIFY(named(model, tree->rootIndex(), "src").isValid());
    tree->expand(named(model, tree->rootIndex(), "src"));
    QTRY_COMPARE(model->rowCount(named(model, tree->rootIndex(), "src")), 1);
    QTest::qWait(100);
    QCOMPARE(qApp->palette().color(QPalette::Base).lightness() < 128,
             theme == "dark");
    QDir().mkpath(output);
    QVERIFY(
        window.grab().save(QDir(output).filePath("host-" + theme + ".png")));
  }
};
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  initializeAppStyle(app);
  WorkspaceTreeHostTest test;
  return QTest::qExec(&test, argc, argv);
}
#include "test_workspace_tree_host.moc"
