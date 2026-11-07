#include "attachments/AttachmentStore.h"
#include "core/ProviderStore.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/ProviderDialog.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/SideChatPane.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLockFile>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
using namespace ade;
namespace {
bool write(const QString &path, const QByteArray &value) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         f.write(value) == value.size();
}
QByteArray bytes(const QString &path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
Conversation chat(QString id, QString parent = {}) {
  Conversation c;
  c.id = id;
  c.sideChatParentId = parent;
  c.title = id;
  c.titleLocked = true;
  c.providerId = "fixture";
  c.model = "old-model";
  c.draft = "Retained " + id + " draft";
  return c;
}
ProviderConfig config() {
  return {"fixture",
          "Fixture provider",
          "http://127.0.0.1:9/v1",
          "synthetic-local-fixture",
          "old-model",
          "openai"};
}
bool seed(const QString &data, bool child = true) {
  QString error;
  SessionStore store(data);
  store.load(&error);
  if (!error.isEmpty() ||
      !store.save(
          child ? QList<Conversation>{chat("parent"), chat("child", "parent")}
                : QList<Conversation>{chat("parent")}))
    return false;
  return ProviderStore::save(data + "/providers.json", {config()});
}
void show(MainWindow &w) {
  w.setAttribute(Qt::WA_DontShowOnScreen);
  w.resize(1000, 700);
  w.show();
  QCoreApplication::processEvents();
}
QComboBox *models(MainWindow &w) {
  auto *pane = w.findChild<SideChatPane *>();
  return pane ? pane->findChild<QComboBox *>("sideChatModel") : nullptr;
}
bool hasModel(QComboBox *c, const QString &model) {
  if (!c)
    return false;
  for (int i = 0; i < c->count(); ++i)
    if (c->itemData(i).toJsonObject().value("modelId") == model)
      return true;
  return false;
}
QStringList pending(const QString &data) {
  SessionStore store(data);
  QString error;
  store.load(&error);
  return error.isEmpty() ? store.pendingCleanup() : QStringList{"invalid"};
}
QList<Conversation> sessions(const QString &data) {
  SessionStore store(data);
  return store.load();
}
QJsonArray entries(QComboBox *c) {
  QJsonArray a;
  for (int i = 0; i < c->count(); ++i)
    a.append(c->itemData(i).toJsonObject());
  return a;
}
} // namespace
class DesktopProviderTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { initializeAppStyle(*qApp); }
  void providerErrorDoesNotSkipOwnedCleanup_data() {
    QTest::addColumn<bool>("corruptProvider");
    QTest::newRow("valid-provider-control") << false;
    QTest::newRow("invalid-provider") << true;
  }
  void providerErrorDoesNotSkipOwnedCleanup() {
    QFETCH(bool, corruptProvider);
    QTemporaryDir data;
    QVERIFY(seed(data.path(), false));
    QString error;
    AttachmentStore attachments(data.path());
    const auto ref = attachments.write("deleted", "fixture.txt",
                                       "owned deleted bytes", &error);
    QVERIFY2(!ref.isEmpty(), qPrintable(error));
    const auto path = attachments.resolve("deleted", ref, {}, &error);
    QVERIFY(QFileInfo::exists(path));
    SessionStore store(data.path());
    auto retained = store.load(&error);
    QVERIFY(store.saveWithCleanup(retained, {"deleted"}, &error));
    if (corruptProvider)
      QVERIFY(write(data.filePath("providers.json"),
                    "{invalid synthetic provider json"));
    const auto providerBefore = bytes(data.filePath("providers.json"));
    {
      MainWindow window(data.path());
      show(window);
      QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 3000);
      QCOMPARE(pending(data.path()), QStringList());
      QCOMPARE(bytes(data.filePath("providers.json")), providerBefore);
      QCOMPARE(window.currentSessionId(), QString("parent"));
    }
    const auto saved = sessions(data.path());
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().draft, QString("Retained parent draft"));
  }
  void failedCleanupRemainsPendingAndRetriesWithProviderError() {
    QTemporaryDir data;
    QVERIFY(seed(data.path(), false));
    QString error;
    AttachmentStore attachments(data.path());
    const auto ref =
        attachments.write("deleted", "fixture.txt", "owned bytes", &error);
    QVERIFY(!ref.isEmpty());
    const auto path = attachments.resolve("deleted", ref, {}, &error);
    SessionStore store(data.path());
    auto saved = store.load();
    QVERIFY(store.saveWithCleanup(saved, {"deleted"}, &error));
    QVERIFY(
        write(data.filePath("providers.json"), "{synthetic corrupt provider"));
    // An independent writer lock makes the initial cleanup transaction refuse.
    QLockFile lock(data.filePath("sessions.lock"));
    QVERIFY(lock.tryLock());
    {
      MainWindow window(data.path());
      show(window);
      QTest::qWait(100);
      QVERIFY(QFileInfo::exists(path));
      QCOMPARE(pending(data.path()), QStringList{"deleted"});
    }
    lock.unlock();
    {
      MainWindow restarted(data.path());
      show(restarted);
      QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path), 3000);
      QVERIFY(pending(data.path()).isEmpty());
    }
  }
  void invalidSessionEnvelopeNeverAuthorizesCleanup() {
    QTemporaryDir data;
    QVERIFY(seed(data.path(), false));
    QString error;
    AttachmentStore attachments(data.path());
    const auto ref =
        attachments.write("deleted", "fixture.txt", "must remain", &error);
    QVERIFY(!ref.isEmpty());
    const auto path = attachments.resolve("deleted", ref, {}, &error);
    const auto invalid =
        QJsonDocument(QJsonObject{{"schemaVersion", 999},
                                  {"sessions", QJsonArray{}},
                                  {"pendingCleanup", QJsonArray{"deleted"}}})
            .toJson();
    QVERIFY(write(data.filePath("sessions.json"), invalid));
    QVERIFY(write(data.filePath("providers.json"), "{bad providers"));
    {
      MainWindow window(data.path());
      show(window);
      QTest::qWait(150);
      QVERIFY(QFileInfo::exists(path));
      QCOMPARE(bytes(data.filePath("sessions.json")), invalid);
    }
  }
  void importRefreshesOpenSideChatAndPreservesDraftSelection() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    show(window);
    auto *pane = window.findChild<SideChatPane *>();
    QVERIFY(pane);
    auto *combo = models(window);
    QVERIFY(hasModel(combo, "old-model"));
    QVERIFY(!hasModel(combo, "gpt-4o-mini"));
    const auto selected = combo->currentData();
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    tabs->setCurrentWidget(pane);
    const auto file = data.filePath("synthetic.env");
    QVERIFY(write(file, "OPENAI_API_KEY=fixture-not-a-live-key\n"));
    window.importKeys(file);
    QVERIFY2(hasModel(combo, "gpt-4o-mini"),
             "Imported model must be offered in the already open side chat");
    QCOMPARE(combo->currentData(), selected);
    QCOMPARE(tabs->currentWidget(), pane);
    QCOMPARE(pane->composer()->toPlainText(), QString("Retained child draft"));
    const auto saved = sessions(data.path());
    for (const auto &c : saved)
      if (c.id == "child") {
        QCOMPARE(c.model, QString("old-model"));
        QCOMPARE(c.draft, QString("Retained child draft"));
      }
  }
  void dialogSaveRefreshesOpenSideChat_data() {
    QTest::addColumn<bool>("accept");
    QTest::newRow("save") << true;
    QTest::newRow("cancel") << false;
  }
  void dialogSaveRefreshesOpenSideChat() {
    QFETCH(bool, accept);
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    show(window);
    auto *combo = models(window);
    QVERIFY(combo);
    const auto initial = entries(combo);
    const auto disk = bytes(data.filePath("providers.json"));
    bool edited = false;
    QTimer::singleShot(0, &window, [&] {
      auto *dialog = window.findChild<ProviderDialog *>();
      if (!dialog)
        return;
      auto *model = dialog->findChild<QLineEdit *>("providerModel");
      if (!model) {
        dialog->reject();
        return;
      }
      model->setText("new-model");
      edited = true;
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      buttons
          ->button(accept ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)
          ->click();
    });
    emit window.findChild<SettingsPage *>()->providerEditRequested("fixture");
    QVERIFY(edited);
    if (accept) {
      QVERIFY2(hasModel(combo, "new-model"),
               "Saved provider model must refresh the already open side chat");
      QCOMPARE(
          ProviderStore::load(data.filePath("providers.json")).first().model,
          QString("new-model"));
      QCOMPARE(combo->currentData().toJsonObject().value("modelId").toString(),
               QString("old-model"));
    } else {
      QCOMPARE(entries(combo), initial);
      QCOMPARE(bytes(data.filePath("providers.json")), disk);
    }
    QCOMPARE(window.findChild<SideChatPane *>()->composer()->toPlainText(),
             QString("Retained child draft"));
  }
  void failedImportDoesNotPublishUncommittedChoices() {
    QTemporaryDir data;
    QVERIFY(seed(data.path()));
    MainWindow window(data.path());
    show(window);
    auto *combo = models(window);
    QVERIFY(combo);
    const auto initial = entries(combo);
    const auto original = bytes(data.filePath("providers.json"));
    const auto path = data.filePath("synthetic.env");
    QVERIFY(write(path, "OPENAI_API_KEY=fixture-not-a-live-key\n"));
    QLockFile lock(data.filePath("providers.json.lock"));
    QVERIFY(lock.tryLock());
    window.importKeys(path);
    QCOMPARE(entries(combo), initial);
    QCOMPARE(bytes(data.filePath("providers.json")), original);
  }
};
QTEST_MAIN(DesktopProviderTest)
#include "test_desktop_provider.moc"
