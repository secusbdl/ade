#include "editor/CodeEditor.h"
#include "editor/EditorWidget.h"
#include "ui/WorkspaceFileTreeModel.h"
#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHeaderView>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
using namespace ade;
namespace {
bool put(const QString &p, const QByteArray &b) {
  QFile f(p);
  return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
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
bool removeJunction(const QString &path) {
  return RemoveDirectoryW(
      reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
}
QStringList names(WorkspaceFileTreeModel &m, const QModelIndex &p) {
  QStringList result;
  for (int i = 0; i < m.rowCount(p); ++i)
    result << m.fileName(m.index(i, 0, p));
  return result;
}
QModelIndex named(WorkspaceFileTreeModel &m, const QModelIndex &p,
                  const QString &name) {
  for (int i = 0; i < m.rowCount(p); ++i) {
    const auto row = m.index(i, 0, p);
    if (m.fileName(row) == name)
      return row;
  }
  return {};
}
} // namespace
class WorkspaceTreeTest : public QObject {
  Q_OBJECT
private slots:
  void outsideLinkIsLeafAndCannotOpen() {
    QTemporaryDir t;
    const auto root = t.filePath("root"), foreign = t.filePath("foreign");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(QDir().mkdir(foreign));
    QVERIFY(put(foreign + "/foreign.txt", "OUTSIDE_SENTINEL"));
    QVERIFY(junction(root + "/link", foreign));
    QVERIFY(QDir().mkdir(root + "/inside"));
    QVERIFY(put(root + "/inside/local.txt", "INSIDE_DIRECTORY"));
    QVERIFY(junction(root + "/inside-link", root + "/inside"));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto ri = model.setRootPath(root);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 3000);
    const auto link = named(model, ri, "link");
    QVERIFY(link.isValid());
    QVERIFY(link.data(WorkspaceFileTreeModel::LinkRole).toBool());
    QVERIFY(!model.hasChildren(link));
    QVERIFY(!model.canFetchMore(link));
    QVERIFY(!model.index(root + "/link/foreign.txt").isValid());
    QString error;
    QVERIFY(!model.pinFile(link, &error));
    QVERIFY(!error.isEmpty());
    const auto insideLink = named(model, ri, "inside-link");
    QVERIFY(insideLink.isValid());
    QVERIFY(insideLink.data(WorkspaceFileTreeModel::LinkRole).toBool());
    QVERIFY(!model.hasChildren(insideLink));
    QVERIFY(!model.canFetchMore(insideLink));
    QVERIFY(!model.pinFile(insideLink, &error));
    QVERIFY(error.contains("Directory links"));
  }
  void gitHiddenNaturalDirectoriesFirst() {
    QTemporaryDir t;
    QVERIFY(QDir().mkdir(t.filePath(".git")));
    QVERIFY(QDir().mkdir(t.filePath("z")));
    for (const auto &name : {"file10.txt", "file2.txt", ".gitignore"})
      QVERIFY(put(t.filePath(name), "yes"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = m.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(names(m, root),
             QStringList({"z", ".gitignore", "file2.txt", "file10.txt"}));
  }
  void ordinaryOpenUsesOwnedPhysicalIdentity() {
    QTemporaryDir t;
    QVERIFY(put(t.filePath("file.txt"), "INSIDE_SENTINEL"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = m.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    const auto file = named(m, root, "file.txt");
    QString error;
    auto loan = m.pinFile(file, &error);
    QVERIFY2(loan, qPrintable(error));
    EditorWidget editor;
    QVERIFY(editor.openFile(loan.absolutePath));
    QCOMPARE(editor.editor()->toPlainText(), QString("INSIDE_SENTINEL"));
    QCOMPARE(QFileInfo(editor.filePath()).canonicalFilePath(),
             QFileInfo(t.filePath("file.txt")).canonicalFilePath());
  }
  void safeFileLinkOpensInsideAndForeignFileLinkRejects() {
    QTemporaryDir t;
    const auto root = t.filePath("root");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(put(root + "/target.txt", "INSIDE_LINK"));
    QVERIFY(put(t.filePath("outside.txt"), "OUTSIDE_LINK"));
    const auto target = QDir::toNativeSeparators(root + "/target.txt"),
               link = QDir::toNativeSeparators(root + "/safe.txt");
    if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(link.utf16()),
                             reinterpret_cast<LPCWSTR>(target.utf16()),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
      QSKIP("File symlink creation unavailable on this Windows account.");
    const auto outside = QDir::toNativeSeparators(t.filePath("outside.txt")),
               bad = QDir::toNativeSeparators(root + "/bad.txt");
    QVERIFY(CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(bad.utf16()),
                                reinterpret_cast<LPCWSTR>(outside.utf16()),
                                SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto ri = m.setRootPath(root);
    QTRY_COMPARE(loaded.size(), 1);
    auto pin = m.pinFile(named(m, ri, "safe.txt"));
    QVERIFY(pin);
    QCOMPARE(keyPath(pin.absolutePath), keyPath(root + "/target.txt"));
    QVERIFY(!m.pinFile(named(m, ri, "bad.txt")));
  }
  void switchRootDropsInflightAndStaleIndex() {
    QTemporaryDir t;
    const auto a = t.filePath("a"), b = t.filePath("b");
    QVERIFY(QDir().mkdir(a));
    QVERIFY(QDir().mkdir(b));
    QVERIFY(put(a + "/OLD.txt", "a"));
    QVERIFY(put(b + "/NEW.txt", "b"));
    QSemaphore entered, release;
    WorkspaceFileTreeModel m;
    m.beforeScan = [&] {
      entered.release();
      release.acquire();
    };
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto old = m.setRootPath(a);
    QVERIFY(entered.tryAcquire(1, 3000));
    m.beforeScan = {};
    const auto current = m.setRootPath(b);
    release.release();
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(names(m, current), QStringList{"NEW.txt"});
    QCOMPARE(m.rowCount(old), 0);
    QVERIFY(m.filePath(old).isEmpty());
    QVERIFY(!m.pinFile(old));
  }
  void observedCreateRenameDeletePreservesStableIndex() {
    QTemporaryDir t;
    QVERIFY(put(t.filePath("keep.txt"), "keep"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = m.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    QPersistentModelIndex keep = named(m, root, "keep.txt");
    QVERIFY(put(t.filePath("new.txt"), "new"));
    QTRY_VERIFY_WITH_TIMEOUT(names(m, root).contains("new.txt"), 4000);
    QVERIFY(keep.isValid());
    QCOMPARE(m.fileName(keep), QString("keep.txt"));
    QVERIFY(QFile::rename(t.filePath("new.txt"), t.filePath("renamed.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(names(m, root).contains("renamed.txt") &&
                                 !names(m, root).contains("new.txt"),
                             4000);
    QVERIFY(QFile::remove(t.filePath("renamed.txt")));
    QTRY_VERIFY_WITH_TIMEOUT(!names(m, root).contains("renamed.txt"), 4000);
    QVERIFY(keep.isValid());
  }
  void junctionReplacementBeforeOpenIsRejected() {
    QTemporaryDir t;
    const auto root = t.filePath("root"), outside = t.filePath("outside");
    QVERIFY(QDir().mkpath(root + "/nested"));
    QVERIFY(QDir().mkdir(outside));
    QVERIFY(put(root + "/nested/item.txt", "inside"));
    QVERIFY(put(outside + "/item.txt", "outside"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto ri = m.setRootPath(root);
    QTRY_COMPARE(loaded.size(), 1);
    const auto dir = named(m, ri, "nested");
    m.fetchMore(dir);
    QTRY_COMPARE(loaded.size(), 2);
    const auto file = named(m, dir, "item.txt");
    QVERIFY(file.isValid());
    QVERIFY(QFile::remove(root + "/nested/item.txt"));
    QVERIFY(QDir().rmdir(root + "/nested"));
    QVERIFY(junction(root + "/nested", outside));
    QString error;
    QVERIFY(!m.pinFile(file, &error));
    QVERIFY(!error.isEmpty());
  }
  void rootReplacementWithSameNamesDoesNotAdopt() {
    QTemporaryDir t;
    const auto root = t.filePath("root"), old = t.filePath("old");
    QVERIFY(QDir().mkdir(root));
    QVERIFY(put(root + "/same.txt", "old"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto ri = m.setRootPath(root);
    QTRY_COMPARE(loaded.size(), 1);
    const auto file = named(m, ri, "same.txt");
    QVERIFY(QDir().rename(root, old));
    QVERIFY(QDir().mkdir(root));
    QVERIFY(put(root + "/same.txt", "foreign replacement"));
    QVERIFY(!m.pinFile(file));
    m.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(m.data(ri, WorkspaceFileTreeModel::ErrorRole)
                                 .toString()
                                 .contains("replaced"),
                             4000);
    QVERIFY(!names(m, ri).contains("same.txt"));
  }
  void observerMayCloseAndPendingWorkerMayOutlive() {
    QTemporaryDir t;
    QVERIFY(put(t.filePath("a.txt"), "a"));
    auto *m = new WorkspaceFileTreeModel;
    QPointer<WorkspaceFileTreeModel> p(m);
    connect(m, &WorkspaceFileTreeModel::directoryLoaded, m,
            [m](const QString &) { delete m; });
    m->setRootPath(t.path());
    QTRY_VERIFY(p.isNull());
    QSemaphore entered, release;
    auto *pending = new WorkspaceFileTreeModel;
    pending->beforeScan = [&] {
      entered.release();
      release.acquire();
    };
    pending->setRootPath(t.path());
    QVERIFY(entered.tryAcquire(1, 3000));
    delete pending;
    release.release();
  }
  void boundedFanoutPages() {
    QTemporaryDir t;
    for (int i = 0; i < 220; ++i)
      QVERIFY(put(t.filePath(QString("f%1.txt").arg(i)), "x"));
    WorkspaceFileTreeModel m;
    QSignalSpy loaded(&m, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = m.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(m.rowCount(root), 100);
    QVERIFY(m.canFetchMore(root));
    m.fetchMore(root);
    QCOMPARE(m.rowCount(root), 200);
    m.fetchMore(root);
    QCOMPARE(m.rowCount(root), 220);
    QVERIFY(!m.canFetchMore(root));
  }

  void rootResetObserverMayClose() {
    QTemporaryDir t;
    auto *model = new WorkspaceFileTreeModel;
    QPointer<WorkspaceFileTreeModel> guard(model);
    connect(model, &WorkspaceFileTreeModel::rootIndexChanged, model,
            [model](QModelIndex) { delete model; });
    QVERIFY(!model->setRootPath(t.path()).isValid());
    QVERIFY(guard.isNull());
  }
  void rootSwitchDuringRowsPublicationIsDeferred() {
    QTemporaryDir t;
    const auto a = t.filePath("a"), b = t.filePath("b");
    QVERIFY(QDir().mkdir(a));
    QVERIFY(QDir().mkdir(b));
    QVERIFY(put(a + "/old.txt", "old"));
    QVERIFY(put(b + "/new.txt", "new"));
    WorkspaceFileTreeModel model;
    bool switched = false;
    QModelIndex current;
    connect(&model, &WorkspaceFileTreeModel::rowsInserted, &model,
            [&](const QModelIndex &, int, int) {
              if (!switched) {
                switched = true;
                model.setRootPath(b);
              }
            });
    connect(&model, &WorkspaceFileTreeModel::rootIndexChanged, &model,
            [&](QModelIndex index) { current = index; });
    model.setRootPath(a);
    QTRY_COMPARE_WITH_TIMEOUT(model.rootPath(), b, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(names(model, current).contains("new.txt"), 3000);
    QVERIFY(!names(model, current).contains("old.txt"));
  }
  void explicitNavigationRevealsPagedRows() {
    QTemporaryDir t;
    for (int i = 0; i < 220; ++i)
      QVERIFY(put(t.filePath(QString("f%1.txt").arg(i)), "x"));
    WorkspaceFileTreeModel model;
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    tester.setUseFetchMore(false);
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = model.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    QCOMPARE(model.rowCount(root), 100);
    const auto last = model.index(t.filePath("f219.txt"));
    QVERIFY(last.isValid());
    QVERIFY(last.row() < model.rowCount(root));
    QCOMPARE(model.fileName(last), QString("f219.txt"));
    QVERIFY(put(t.filePath("f0a.txt"), "added"));
    QTRY_VERIFY_WITH_TIMEOUT(names(model, root).contains("f0a.txt"), 4000);
  }
  void entryLimitIsDisclosed() {
    QTemporaryDir t;
    for (int i = 0; i < WorkspaceFileTreeModel::MaxEntries + 3; ++i)
      QVERIFY(put(t.filePath(QString("f%1").arg(i)), {}));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = model.setRootPath(t.path());
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 15000);
    QVERIFY(root.data(WorkspaceFileTreeModel::LimitedRole).toBool());
    QVERIFY(model.rowCount(root) <= WorkspaceFileTreeModel::PageSize + 1);
    bool notice = false;
    for (int i = 0; i < model.rowCount(root); ++i)
      notice |= model.index(i, 0, root)
                    .data(WorkspaceFileTreeModel::LimitedRole)
                    .toBool();
    QVERIFY(notice);
  }
  void collapsedDetachedTreeReleasesOldWorkspace() {
    QTemporaryDir t;
    QVERIFY(put(t.filePath("old.txt"), "old"));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto old = model.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    const auto file = named(model, old, "old.txt");
    model.setRootPath({});
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(!model.pinFile(file));
    QVERIFY(put(t.filePath("new.txt"), "new"));
    QTest::qWait(180);
    QCOMPARE(loaded.size(), 1);
  }
  void boundedWatchCoverageIsDisclosedAndReset() {
    QTemporaryDir t;
    for (int i = 0; i < WorkspaceFileTreeModel::MaxWatches; ++i)
      QVERIFY(QDir().mkdir(t.filePath(QString("dir%1").arg(i))));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    QSignalSpy coverage(&model, &WorkspaceFileTreeModel::watchCoverageChanged);
    const auto root = model.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    while (model.canFetchMore(root))
      model.fetchMore(root);
    for (int i = 0; i < model.rowCount(root); ++i)
      model.refresh(model.index(i, 0, root));
    const auto uniqueLoaded = [&] {
      QSet<QString> paths;
      for (const auto &emission : loaded)
        paths.insert(emission.first().toString());
      return paths.size();
    };
    QTRY_COMPARE_WITH_TIMEOUT(uniqueLoaded(),
                              WorkspaceFileTreeModel::MaxWatches + 1, 15000);
    const auto *watcher = model.findChild<QFileSystemWatcher *>();
    QVERIFY(watcher);
    QVERIFY(watcher->directories().size() <=
            WorkspaceFileTreeModel::MaxWatches);
    QVERIFY(!model.watchCoverageComplete());
    QVERIFY(!coverage.isEmpty());
    QCOMPARE(coverage.last().first().toBool(), false);
    model.setRootPath({});
    QVERIFY(model.watchCoverageComplete());
    QCOMPARE(coverage.last().first().toBool(), true);
  }
  void openLoanPreventsReplacementOnlyUntilEditorLoad() {
    QTemporaryDir t;
    const auto path = t.filePath("file.txt");
    QVERIFY(put(path, "ORIGINAL"));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = model.setRootPath(t.path());
    QTRY_COMPARE(loaded.size(), 1);
    auto loan = model.pinFile(named(model, root, "file.txt"));
    QVERIFY(loan);
    const auto renamed = t.filePath("renamed.txt");
    QVERIFY(!MoveFileExW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(renamed).utf16()),
        0));
    EditorWidget editor;
    QVERIFY(editor.openFile(loan.absolutePath));
    QCOMPARE(editor.editor()->toPlainText(), QString("ORIGINAL"));
    loan = {};
    QVERIFY(MoveFileExW(
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(renamed).utf16()),
        0));
  }
  void explicitReconnectCapturesNewRootAndRetiresOldRows() {
    QTemporaryDir t;
    const auto rootPath = t.filePath("root"), retired = t.filePath("retired");
    QVERIFY(QDir().mkdir(rootPath));
    QVERIFY(put(rootPath + "/file.txt", "OLD"));
    WorkspaceFileTreeModel model;
    QSignalSpy loaded(&model, &WorkspaceFileTreeModel::directoryLoaded);
    const auto root = model.setRootPath(rootPath);
    QTRY_COMPARE(loaded.size(), 1);
    const auto old = named(model, root, "file.txt");
    QVERIFY(QDir().rename(rootPath, retired));
    QVERIFY(QDir().mkdir(rootPath));
    QVERIFY(put(rootPath + "/file.txt", "NEW"));
    QVERIFY(!model.pinFile(old));
    const auto replacement = model.setRootPath(rootPath, true);
    QTRY_VERIFY(named(model, replacement, "file.txt").isValid());
    QVERIFY(!model.pinFile(old));
    auto pin = model.pinFile(named(model, replacement, "file.txt"));
    QVERIFY(pin);
    EditorWidget editor;
    QVERIFY(editor.openFile(pin.absolutePath));
    QCOMPARE(editor.editor()->toPlainText(), QString("NEW"));
  }
  void deferredSameRootRefreshCannotAdoptReplacement() {
    QTemporaryDir t;
    const auto path = t.filePath("root"), oldPath = t.filePath("old");
    QVERIFY(QDir().mkdir(path));
    QVERIFY(put(path + "/file.txt", "OLD"));
    WorkspaceFileTreeModel model;
    bool replaced = false, failed = false;
    connect(&model, &QAbstractItemModel::rowsInserted, &model, [&] {
      if (replaced)
        return;
      replaced = true;
      if (!QDir().rename(path, oldPath) || !QDir().mkdir(path) ||
          !put(path + "/file.txt", "NEW")) {
        failed = true;
        return;
      }
      model.setRootPath(path);
    });
    model.setRootPath(path);
    QTRY_VERIFY(replaced);
    QVERIFY(!failed);
    QTRY_VERIFY(
        named(model, model.index(0, 0, QModelIndex{}), "file.txt").isValid());
    QVERIFY2(!model.pinFile(
                 named(model, model.index(0, 0, QModelIndex{}), "file.txt")),
             "A deferred background same-root setter must not authorize a "
             "replacement root.");
  }
  void componentRenders() {
    const auto output = qEnvironmentVariable("ADE_TREE_RENDER_DIR");
    if (output.isEmpty())
      QSKIP("Optional component images are not requested.");
    QTemporaryDir t;
    QVERIFY(QDir().mkdir(t.filePath("src")));
    QVERIFY(QDir().mkdir(t.filePath(".git")));
    QVERIFY(put(t.filePath("README.md"), "Readme"));
    QVERIFY(put(t.filePath(".gitignore"), "out/"));
    QVERIFY(put(t.filePath("src/main.cpp"), "int main() {}"));
    WorkspaceFileTreeModel model;
    QTreeView tree;
    tree.setModel(&model);
    tree.setHeaderHidden(true);
    tree.setRootIndex(model.setRootPath(t.path()));
    QTRY_VERIFY(model.rowCount(tree.rootIndex()) >= 3);
    tree.resize(300, 430);
    tree.setAttribute(Qt::WA_DontShowOnScreen);
    tree.show();
    tree.expand(named(model, tree.rootIndex(), "src"));
    QTRY_VERIFY(model.rowCount(named(model, tree.rootIndex(), "src")) == 1);
    const int font = QFontDatabase::addApplicationFont(
        QFINDTESTDATA("../assets/fonts/Geist-Variable.ttf"));
    QVERIFY(font >= 0);
    QFont face("Geist");
    face.setPixelSize(13);
    tree.setFont(face);
    QDir().mkpath(output);
    for (const bool dark : {false, true}) {
      QPalette palette;
      palette.setColor(QPalette::Base, QColor(dark ? "#21242b" : "#fdfdfd"));
      palette.setColor(QPalette::Text, QColor(dark ? "#eceef4" : "#292a2d"));
      palette.setColor(QPalette::Window, palette.color(QPalette::Base));
      palette.setColor(QPalette::WindowText, palette.color(QPalette::Text));
      tree.setPalette(palette);
      QTest::qWait(30);
      QImage image(tree.size(), QImage::Format_ARGB32_Premultiplied);
      image.fill(palette.color(QPalette::Base));
      tree.render(&image);
      QVERIFY(image.save(
          QDir(output).filePath(dark ? "tree-dark.png" : "tree-light.png")));
    }
  }

private:
  static QString keyPath(QString p) {
    return QDir::cleanPath(QDir::fromNativeSeparators(p)).toCaseFolded();
  }
};
QTEST_MAIN(WorkspaceTreeTest)
#include "test_workspace_tree.moc"
