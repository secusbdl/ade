#include "harness/HarnessAttachmentStore.h"
#include "runtime/AgentToolHost.h"
#include "runtime/HarnessFileContext.h"
#include "ui/AppFonts.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include <QAction>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
#include <windows.h>
using namespace ade;
namespace {
QString root(const QString &data, const QString &id) {
  return data + "/harness-workspaces/" +
         QString::fromLatin1(
             QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256)
                 .toHex());
}
bool put(const QString &path, const QByteArray &bytes) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
Conversation chat(const QString &id) {
  Conversation c;
  c.id = id;
  c.title = id;
  c.titleLocked = true;
  c.providerId = "fixture";
  c.model = "fixture";
  return c;
}
bool fork(MainWindow &window) {
  auto *list = window.findChild<QListWidget *>("agentList");
  if (!list)
    return false;
  QListWidgetItem *item = nullptr;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->data(Qt::UserRole) == "source")
      item = list->item(i);
  if (!item)
    return false;
  list->resize(220, 600);
  list->doItemsLayout();
  list->scrollToItem(item);
  emit list->customContextMenuRequested(list->visualItemRect(item).center());
  QPointer<QMenu> menu = window.findChild<QMenu *>("conversationMenu");
  auto *action =
      menu ? menu->findChild<QAction *>("forkConversation") : nullptr;
  if (!action || !action->isEnabled())
    return false;
  action->trigger();
  if (menu)
    menu->close();
  return true;
}
} // namespace
class HarnessCleanupHostTest : public QObject {
  Q_OBJECT
private slots:
  void pendingCleanupRemovesOnlyOwnedFallbackRoot() {
    QTemporaryDir temp;
    const auto data = temp.filePath("data");
    QVERIFY(QDir().mkpath(data));
    const auto workspace = temp.filePath("user-workspace");
    QVERIFY(QDir().mkpath(workspace));
    QVERIFY(put(workspace + "/keep.txt", "keep"));
    {
      HarnessFileContext files(data, "deleted");
      QVERIFY(files.error().isEmpty());
      QVERIFY(put(files.rootDirectory() + "/owned.bin", QByteArray("a\0b", 3)));
    }
    {
      HarnessAttachmentStore inputs(data, "deleted");
      const QByteArray bytes("handoff");
      QVERIFY(inputs
                  .prepare({{"att/file.bin", "file.bin",
                             QString::fromLatin1(
                                 QCryptographicHash::hash(
                                     bytes, QCryptographicHash::Sha256)
                                     .toHex()),
                             bytes}},
                           nullptr)
                  .valid());
    }
    auto retained = chat("retained");
    retained.workspace = workspace;
    SessionStore sessions(data);
    QVERIFY(sessions.saveWithCleanup({retained}, {"deleted"}));
    {
      MainWindow window(data);
      QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root(data, "deleted")), 5000);
    }
    SessionStore reloaded(data);
    reloaded.load();
    QVERIFY(reloaded.pendingCleanup().isEmpty());
    QVERIFY(QFileInfo::exists(workspace + "/keep.txt"));
    QVERIFY(!QFileInfo::exists(
        QString(root(data, "deleted"))
            .replace("harness-workspaces/", "harness-attachments/")));
  }
  void failedForkDoesNotLeaveFallbackClone() {
    QTemporaryDir temp;
    const auto data = temp.filePath("data");
    QVERIFY(QDir().mkpath(data));
    QVERIFY(SessionStore(data).save({chat("source")}));
    {
      HarnessFileContext files(data, "source");
      QVERIFY(files.error().isEmpty());
      QVERIFY(put(files.rootDirectory() + "/source.bin", "source"));
    }
    {
      HarnessAttachmentStore inputs(data, "source");
      const QByteArray bytes("handoff");
      QVERIFY(inputs
                  .prepare({{"att/file.bin", "file.bin",
                             QString::fromLatin1(
                                 QCryptographicHash::hash(
                                     bytes, QCryptographicHash::Sha256)
                                     .toHex()),
                             bytes}},
                           nullptr)
                  .valid());
    }
    MainWindow window(data);
    const auto before = QDir(data + "/harness-workspaces")
                            .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    const auto beforeInputs = QDir(data + "/harness-attachments")
                                  .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    SessionStore other(data);
    auto chats = other.load();
    chats[0].title = "concurrent durable change";
    QVERIFY(other.save(chats));
    QVERIFY(fork(window));
    QCOMPARE(QDir(data + "/harness-workspaces")
                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot),
             before);
    QCOMPARE(other.load().size(), 1);
    QCOMPARE(QDir(data + "/harness-attachments")
                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot),
             beforeInputs);
    QVERIFY(QFileInfo::exists(root(data, "source") + "/source.bin"));
  }
  void blockedCleanupRetainsMarkerThenRestartRetries() {
    QTemporaryDir temp;
    const auto data = temp.filePath("data");
    QVERIFY(QDir().mkpath(data));
    {
      HarnessFileContext files(data, "deleted");
      QVERIFY(files.error().isEmpty());
      QVERIFY(put(files.rootDirectory() + "/held.txt", "held"));
    }
    const auto path =
        QDir::toNativeSeparators(root(data, "deleted") + "/held.txt")
            .toStdWString();
    auto h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(h != INVALID_HANDLE_VALUE);
    SessionStore sessions(data);
    QVERIFY(sessions.saveWithCleanup({chat("retained")}, {"deleted"}));
    {
      MainWindow window(data);
      QTest::qWait(150);
      SessionStore check(data);
      check.load();
      const auto pending = check.pendingCleanup();
      CloseHandle(h);
      h = INVALID_HANDLE_VALUE;
      QCOMPARE(pending, QStringList{"deleted"});
    }
    {
      MainWindow window(data);
      QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(root(data, "deleted")), 5000);
    }
    SessionStore check(data);
    check.load();
    QVERIFY(check.pendingCleanup().isEmpty());
  }
};
QTEST_MAIN(HarnessCleanupHostTest)
#include "test_harness_cleanup_host.moc"
