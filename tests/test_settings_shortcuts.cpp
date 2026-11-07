#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/SidebarSplitter.h"
#include "ui/WorkbenchSplitter.h"
#include <QApplication>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>

using namespace ade;
namespace {
bool seed(const QString &directory) {
  Conversation chat;
  chat.id = "parent";
  chat.title = "Synthetic shortcut fixture";
  chat.draft = "Retained original draft";
  return SessionStore(directory).save({chat});
}
QByteArray read(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void activate(MainWindow &window) {
  window.setAttribute(Qt::WA_DontShowOnScreen);
  window.resize(1000, 700);
  window.show();
  QApplication::setActiveWindow(&window);
  QCoreApplication::processEvents();
}
QShortcut *shortcut(MainWindow &window, const QString &key) {
  for (auto *value : window.findChildren<QShortcut *>())
    if (value->keys().contains(QKeySequence(key)))
      return value;
  return nullptr;
}
SettingsPage *settings(MainWindow &window) {
  window.findChild<QToolButton *>("settingsButton")->click();
  auto *page = window.findChild<SettingsPage *>();
  if (page)
    page->findChild<QPushButton *>("settingsBack")->setFocus();
  return page;
}
void press(QWidget *widget, const QString &sequence) {
  const auto key = QKeySequence(sequence)[0];
  QTest::keyClick(widget, key.key(), key.keyboardModifiers());
  QCoreApplication::processEvents();
}
} // namespace
class SettingsShortcutsTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { initializeAppStyle(*qApp); }
  void settingsDoesNotMutateHiddenWorkbench_data() {
    QTest::addColumn<QString>("sequence");
    for (const auto *value :
         {"Ctrl+N", "Ctrl+T", "Ctrl+Shift+G", "Ctrl+B", "Ctrl+Alt+B",
          "Ctrl+Shift+B", "Ctrl+Shift+E", "Ctrl+W", "Ctrl+J"})
      QTest::newRow(value) << QString(value);
  }
  void settingsDoesNotMutateHiddenWorkbench() {
    QFETCH(QString, sequence);
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    activate(window);
    auto *input = window.findChild<Composer *>("composerInput");
    QVERIFY(input);
    input->setPlainText("Unsubmitted draft must stay unchanged");
    auto *page = settings(window);
    QVERIFY(page && page->isVisible());
    auto *binding = shortcut(window, sequence);
    QVERIFY(binding);
    QSignalSpy activated(binding, &QShortcut::activated);
    const auto id = window.currentSessionId();
    const auto draft = input->toPlainText();
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    const int count = tabs->count(), selected = tabs->currentIndex();
    const auto sessions = read(data.filePath("sessions.json"));
    QVERIFY(!sessions.isEmpty());
    press(page->findChild<QPushButton *>("settingsBack"), sequence);
    QCOMPARE(activated.size(), 0);
    QVERIFY(page->isVisible());
    QCOMPARE(window.currentSessionId(), id);
    QCOMPARE(input->toPlainText(), draft);
    QCOMPARE(tabs->count(), count);
    QCOMPARE(tabs->currentIndex(), selected);
    QCOMPARE(read(data.filePath("sessions.json")), sessions);
  }
  void allWorkbenchBindingsBelongToVisiblePage() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    activate(window);
    auto *page = settings(window);
    QVERIFY(page && page->isVisible());
    for (const auto *key :
         {"Ctrl+N", "Ctrl+B", "Ctrl+Shift+O", "Ctrl+T", "Ctrl+Alt+T", "Ctrl+J",
          "Ctrl+Shift+G", "Ctrl+Alt+B", "Ctrl+Shift+B", "Ctrl+Shift+E",
          "Ctrl+W"}) {
      auto *binding = shortcut(window, key);
      QVERIFY2(binding, key);
      auto *owner = qobject_cast<QWidget *>(binding->parent());
      QVERIFY2(owner && !owner->isVisible(), key);
    }
  }
  void returnRestoresActualShortcut_data() {
    QTest::addColumn<QString>("sequence");
    for (const auto *value : {"Ctrl+N", "Ctrl+T", "Ctrl+Shift+G"})
      QTest::newRow(value) << QString(value);
  }
  void returnRestoresActualShortcut() {
    QFETCH(QString, sequence);
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    activate(window);
    auto *input = window.findChild<Composer *>("composerInput");
    QVERIFY(input);
    input->setPlainText("New local draft survives Settings roundtrip");
    const auto draft = input->toPlainText();
    auto *page = settings(window);
    QVERIFY(page && page->isVisible());
    page->findChild<QPushButton *>("settingsBack")->click();
    QVERIFY(!page->isVisible());
    QCOMPARE(input->toPlainText(), draft);
    input->setFocus();
    auto *binding = shortcut(window, sequence);
    QVERIFY(binding);
    QSignalSpy activated(binding, &QShortcut::activated);
    const auto oldId = window.currentSessionId();
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    const int count = tabs->count();
    press(input, sequence);
    QCOMPARE(activated.size(), 1);
    QVERIFY(!page->isVisible());
    if (sequence == "Ctrl+N") {
      QVERIFY(window.currentSessionId() != oldId);
      QString error;
      const auto saved = SessionStore(data.path()).load(&error);
      QVERIFY2(error.isEmpty(), qPrintable(error));
      bool found = false;
      for (const auto &chat : saved)
        if (chat.id == oldId) {
          found = true;
          QCOMPARE(chat.draft, draft);
        }
      QVERIFY(found);
    } else {
      QCOMPARE(tabs->count(), count + 1);
      QVERIFY(tabs->currentWidget()->isVisible());
      QCOMPARE(input->toPlainText(), draft);
    }
  }
  void settingsShortcutRemainsGlobal() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    activate(window);
    auto *input = window.findChild<Composer *>("composerInput");
    QVERIFY(input);
    input->setFocus();
    auto *binding = shortcut(window, "Ctrl+,");
    QVERIFY(binding);
    QSignalSpy activated(binding, &QShortcut::activated);
    press(input, "Ctrl+,");
    QCOMPARE(activated.size(), 1);
    auto *page = window.findChild<SettingsPage *>();
    QVERIFY(page && page->isVisible());
    press(page->findChild<QPushButton *>("settingsBack"), "Ctrl+,");
    QCOMPARE(activated.size(), 2);
    QVERIFY(page->isVisible());
  }
  void compactNavigationRetainsWorkbenchBindings() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    activate(window);
    window.resize(350, 700);
    QCoreApplication::processEvents();
    auto *sidebar = window.findChild<SidebarSplitter *>();
    auto *workbench = window.findChild<WorkbenchSplitter *>();
    auto *agents = window.findChild<QListWidget *>("agentList");
    QVERIFY(sidebar && workbench && agents);
    QVERIFY(sidebar->isCompact());
    sidebar->setCollapsed(false);
    QCoreApplication::processEvents();
    QVERIFY(agents->isVisible());
    QVERIFY(!workbench->isVisible());
    agents->setFocus();
    auto *binding = shortcut(window, "Ctrl+Shift+B");
    QVERIFY(binding);
    QSignalSpy activated(binding, &QShortcut::activated);
    const bool visible = workbench->fileTreeVisible();
    press(agents, "Ctrl+Shift+B");
    QCOMPARE(activated.size(), 1);
    QCOMPARE(workbench->fileTreeVisible(), !visible);
  }
};
QTEST_MAIN(SettingsShortcutsTest)
#include "test_settings_shortcuts.moc"
