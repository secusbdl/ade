#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
bool write(const QString &path, const QByteArray &data) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
         f.write(data) == data.size();
}
QByteArray read(const QString &path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
void show(MainWindow &window) {
  window.setAttribute(Qt::WA_DontShowOnScreen);
  window.resize(1000, 700);
  window.show();
  QCoreApplication::processEvents();
}
class HeldProvider : public QTcpServer {
public:
  int requests = 0;
  QList<QPointer<QTcpSocket>> sockets;
  HeldProvider() {
    listen(QHostAddress::LocalHost);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        sockets.append(socket);
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          if (socket->property("handled").toBool()) {
            socket->readAll();
            return;
          }
          auto data =
              socket->property("data").toByteArray() + socket->readAll();
          socket->setProperty("data", data);
          const auto split = data.indexOf("\r\n\r\n");
          if (split < 0)
            return;
          int length = 0;
          for (const auto &line : data.left(split).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (data.size() < split + 4 + length)
            return;
          socket->setProperty("handled", true);
          ++requests;
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: "
              "text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\n");
          const QByteArray chunk =
              "data: {\"choices\":[{\"delta\":{\"content\":\"Still "
              "running\"}}]}\n\n";
          socket->write(QByteArray::number(chunk.size(), 16) + "\r\n" + chunk +
                        "\r\n");
        });
      }
    });
  }
  ProviderConfig config() const {
    return {"fixture",
            "Local held fixture",
            QString("http://127.0.0.1:%1/v1").arg(serverPort()),
            "synthetic-fixture-key",
            "fixture-model",
            "openai"};
  }
  void finish() {
    for (const auto &socket : sockets)
      if (socket) {
        const QByteArray chunk = "data: "
                                 "{\"choices\":[{\"delta\":{},\"finish_"
                                 "reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        socket->write(QByteArray::number(chunk.size(), 16) + "\r\n" + chunk +
                      "\r\n0\r\n\r\n");
        socket->disconnectFromHost();
      }
  }
};
Conversation chat(const QString &id, const QString &draft = {}) {
  Conversation c;
  c.id = id;
  c.title = id;
  c.draft = draft;
  return c;
}
void click(MainWindow &window, const QString &name) {
  if (auto *button = window.findChild<QPushButton *>(name))
    button->click();
}
} // namespace
class StartupRecoveryTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { initializeAppStyle(*qApp); }
  void corruptEmptyProfileCanCloseWithoutChangingBytes_data() {
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("truncated") << QByteArray("{broken");
    QTest::newRow("schema")
        << QByteArray("{\"schemaVersion\":999,\"sessions\":[]}");
  }
  void corruptEmptyProfileCanCloseWithoutChangingBytes() {
    QFETCH(QByteArray, bytes);
    QTemporaryDir data;
    const auto path = data.filePath("sessions.json");
    QVERIFY(write(path, bytes));
    MainWindow window(data.path());
    show(window);
    QVERIFY(window.currentSessionId().isEmpty());
    QVERIFY2(window.close(),
             "No unsaved chat or draft should trap a corrupt-profile window");
    QCOMPARE(read(path), bytes);
  }
  void noChatComposerIsReadOnlyWithRecoveryExplanation() {
    QTemporaryDir data;
    const auto path = data.filePath("sessions.json");
    const QByteArray bytes = "{broken";
    QVERIFY(write(path, bytes));
    MainWindow window(data.path());
    show(window);
    auto *input = window.findChild<Composer *>("composerInput");
    QVERIFY(input);
    QVERIFY(input->isReadOnly());
    QVERIFY(!input->placeholderText().isEmpty());
    QTest::keyClicks(input, "must not be accepted");
    QVERIFY(input->toPlainText().isEmpty());
    QCOMPARE(read(path), bytes);
  }
  void failedCloseKeepsBusyEngineAndDraftUntilDecision() {
    QTemporaryDir data;
    HeldProvider server;
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),
                                {server.config()}));
    MainWindow window(data.path());
    show(window);
    window.submitPrompt("Keep this task running");
    QTRY_COMPARE_WITH_TIMEOUT(server.requests, 1, 3000);
    const auto engines = window.findChildren<AgentEngine *>();
    QCOMPARE(engines.size(), 1);
    auto *engine = engines.first();
    QTRY_VERIFY(engine->isBusy());
    auto *input = window.findChild<Composer *>("composerInput");
    input->setPlainText("Unsaved follow-up");
    const QByteArray external = "{external-change";
    QVERIFY(write(data.filePath("sessions.json"), external));
    QVERIFY(!window.close());
    QVERIFY2(engine->isBusy(),
             "A refused close must not cancel the running task");
    auto *dialog =
        window.findChild<QMessageBox *>("sessionCloseRecoveryDialog");
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(input->toPlainText(), QString("Unsaved follow-up"));
    QCOMPARE(read(data.filePath("sessions.json")), external);
  }
  void validProfileCloseStillSavesDraft() {
    QTemporaryDir data;
    MainWindow window(data.path());
    show(window);
    auto *input = window.findChild<Composer *>("composerInput");
    input->setPlainText("Durable ordinary draft");
    QVERIFY(window.close());
    QString error;
    const auto sessions = SessionStore(data.path()).load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(sessions.first().draft, QString("Durable ordinary draft"));
  }
  void keepWorkingIsDefaultAndDiscardIsExplicit() {
    QTemporaryDir data;
    HeldProvider server;
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),
                                {server.config()}));
    MainWindow window(data.path());
    show(window);
    window.submitPrompt("Working turn");
    QTRY_COMPARE(server.requests, 1);
    auto *engine = window.findChild<AgentEngine *>();
    QVERIFY(engine && engine->isBusy());
    auto *input = window.findChild<Composer *>("composerInput");
    input->setPlainText("Recoverable draft");
    const QByteArray external = "{external-conflict";
    QVERIFY(write(data.filePath("sessions.json"), external));
    window.findChild<QToolButton *>("settingsButton")->click();
    QVERIFY(!window.close());
    auto *dialog =
        window.findChild<QMessageBox *>("sessionCloseRecoveryDialog");
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(dialog->defaultButton(),
             dialog->findChild<QPushButton *>("recoveryKeepWorking"));
    QVERIFY(window.findChild<QStatusBar *>("sessionRecoveryBar")->isVisible());
    click(window, "recoveryKeepWorking");
    QTRY_VERIFY(!dialog->isVisible());
    QVERIFY(window.isVisible());
    QVERIFY(engine->isBusy());
    QCOMPARE(input->toPlainText(), QString("Recoverable draft"));
    QCOMPARE(read(data.filePath("sessions.json")), external);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!window.close());
    click(window, "recoveryCloseWithoutSaving");
    QTRY_VERIFY(!window.isVisible());
    QVERIFY(!engine->isBusy());
    QCOMPARE(read(data.filePath("sessions.json")), external);
    QTest::qWait(550);
    QCOMPARE(read(data.filePath("sessions.json")), external);
  }
  void retrySaveAfterLockReleasePersistsDraft() {
    QTemporaryDir data;
    MainWindow window(data.path());
    show(window);
    window.findChild<Composer *>("composerInput")
        ->setPlainText("Retry saved this draft");
    QLockFile lock(data.filePath("sessions.lock"));
    QVERIFY(lock.tryLock(0));
    QVERIFY(!window.close());
    QVERIFY(window.findChild<QMessageBox *>("sessionCloseRecoveryDialog"));
    lock.unlock();
    click(window, "recoveryRetrySave");
    QTRY_VERIFY(!window.isVisible());
    QString error;
    const auto loaded = SessionStore(data.path()).load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.first().draft, QString("Retry saved this draft"));
  }
  void closeDiscardDoesNotWriteAfterLockFailure() {
    QTemporaryDir data;
    MainWindow window(data.path());
    show(window);
    const auto original = read(data.filePath("sessions.json"));
    window.findChild<Composer *>("composerInput")
        ->setPlainText("Explicitly discarded draft");
    QLockFile lock(data.filePath("sessions.lock"));
    QVERIFY(lock.tryLock(0));
    QVERIFY(!window.close());
    lock.unlock();
    click(window, "recoveryCloseWithoutSaving");
    QTRY_VERIFY(!window.isVisible());
    QTest::qWait(550);
    QCOMPARE(read(data.filePath("sessions.json")), original);
  }
  void repairedStartupCanReloadAndCreateWithoutOverwritingOriginal() {
    QTemporaryDir data;
    const auto path = data.filePath("sessions.json");
    QVERIFY(write(path, "{broken"));
    MainWindow window(data.path());
    show(window);
    const QByteArray fixed =
        "{\"schemaVersion\":1,\"sessions\":[],\"pendingCleanup\":[]}";
    QVERIFY(write(path, fixed));
    click(window, "reloadConversationsButton");
    QCOMPARE(read(path), fixed);
    QVERIFY(window.currentSessionId().isEmpty());
    QVERIFY(window.findChild<Composer *>("composerInput")->isReadOnly());
    click(window, "newAgentButton");
    QVERIFY(!window.currentSessionId().isEmpty());
    QVERIFY(!window.findChild<Composer *>("composerInput")->isReadOnly());
    Composer original;
    QCOMPARE(window.findChild<Composer *>("composerInput")->placeholderText(),
             original.placeholderText());
  }
  void reloadRequiresConfirmationAndKeepsInvalidCandidate() {
    QTemporaryDir data;
    QVERIFY(SessionStore(data.path()).save({chat("original", "saved draft")}));
    MainWindow window(data.path());
    show(window);
    auto *input = window.findChild<Composer *>("composerInput");
    input->setPlainText("unsaved local");
    const QByteArray bad = "{bad-again";
    QVERIFY(write(data.filePath("sessions.json"), bad));
    QVERIFY(!window.close());
    click(window, "recoveryKeepWorking");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    click(window, "reloadConversationsButton");
    auto *dialog = window.findChild<QMessageBox *>("sessionReloadConfirmation");
    QVERIFY(dialog);
    QCOMPARE(input->toPlainText(), QString("unsaved local"));
    click(window, "recoveryConfirmReload");
    QTRY_VERIFY(!window.findChild<QMessageBox *>("sessionReloadConfirmation"));
    QCOMPARE(window.currentSessionId(), QString("original"));
    QCOMPARE(input->toPlainText(), QString("unsaved local"));
    QCOMPARE(read(data.filePath("sessions.json")), bad);
  }
  void validConflictReloadRetiresOldOwnersAndKeepsDiskBytes() {
    QTemporaryDir data;
    HeldProvider server;
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),
                                {server.config()}));
    auto initial = chat("original-owner");
    initial.providerId = "fixture";
    initial.model = "fixture-model";
    initial.titleLocked = true;
    QVERIFY(SessionStore(data.path()).save({initial}));
    MainWindow window(data.path());
    show(window);
    window.submitPrompt("Finish before reload");
    QTRY_COMPARE(server.requests, 1);
    QPointer<AgentEngine> old = window.findChild<AgentEngine *>();
    QVERIFY(old);
    server.finish();
    QTRY_VERIFY(!old->isBusy());
    SessionStore external(data.path());
    QVERIFY(!external.load().isEmpty());
    QVERIFY(external.save({chat("disk-owner", "disk draft")}));
    const auto disk = read(data.filePath("sessions.json"));
    window.findChild<Composer *>("composerInput")
        ->setPlainText("discard on confirmed reload");
    QVERIFY(!window.close());
    click(window, "recoveryKeepWorking");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    bool retired = false;
    connect(old, &QObject::destroyed, this, [&] {
      retired = true;
      window.submitPrompt("obsolete owner must not restart");
    });
    click(window, "reloadConversationsButton");
    click(window, "recoveryConfirmReload");
    QTRY_COMPARE(window.currentSessionId(), QString("disk-owner"));
    QVERIFY(retired);
    QVERIFY(!old);
    QCOMPARE(window.findChildren<AgentEngine *>().size(), 0);
    QCOMPARE(server.requests, 1);
    QCOMPARE(window.findChild<Composer *>("composerInput")->toPlainText(),
             QString("disk draft"));
    QCOMPARE(read(data.filePath("sessions.json")), disk);
  }
  void reloadWhileRunningDoesNotChangeOwnerOrCancel() {
    QTemporaryDir data;
    HeldProvider server;
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),
                                {server.config()}));
    MainWindow window(data.path());
    show(window);
    window.submitPrompt("Stay running");
    QTRY_COMPARE(server.requests, 1);
    auto *engine = window.findChild<AgentEngine *>();
    const auto owner = window.currentSessionId();
    const QByteArray bad = "{conflict";
    QVERIFY(write(data.filePath("sessions.json"), bad));
    QVERIFY(!window.close());
    click(window, "recoveryKeepWorking");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    click(window, "reloadConversationsButton");
    QVERIFY(engine->isBusy());
    QCOMPARE(window.currentSessionId(), owner);
    QVERIFY(!window.findChild<QMessageBox *>("sessionReloadConfirmation"));
    QCOMPARE(read(data.filePath("sessions.json")), bad);
  }
  void existingOrphanDraftSurvivesNewAgentAfterReload() {
    QTemporaryDir data;
    const auto path = data.filePath("sessions.json");
    QVERIFY(write(path, "{broken"));
    MainWindow window(data.path());
    show(window);
    QVERIFY(write(path, "{\"schemaVersion\":1,\"sessions\":[]}"));
    click(window, "reloadConversationsButton");
    // Programmatic draft delivery can still happen while the editor is
    // readonly.
    auto *input = window.findChild<Composer *>("composerInput");
    input->setPlainText("Retain this previously supplied draft");
    click(window, "newAgentButton");
    QCOMPARE(input->toPlainText(),
             QString("Retain this previously supplied draft"));
    QCOMPARE(SessionStore(data.path()).load().first().draft,
             input->toPlainText());
  }
  void dialogRejectionDuringRaiseDoesNotDereferenceClearedOwner() {
    class RejectOnRaise final : public QObject {
    public:
      bool invoked = false;

    protected:
      bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::ZOrderChange) {
          invoked = true;
          static_cast<QDialog *>(object)->reject();
        }
        return false;
      }
    } observer;
    QTemporaryDir data;
    MainWindow window(data.path());
    show(window);
    window.findChild<Composer *>("composerInput")->setPlainText("draft");
    QVERIFY(write(data.filePath("sessions.json"), "{conflict"));
    QVERIFY(!window.close());
    auto *dialog =
        window.findChild<QMessageBox *>("sessionCloseRecoveryDialog");
    QVERIFY(dialog);
    dialog->installEventFilter(&observer);
    QVERIFY(!window.close());
    QVERIFY(observer.invoked);
    QVERIFY(window.isVisible());
  }
  void reloadDoesNotAdoptRetargetedDataJunction() {
#ifdef Q_OS_WIN
    QTemporaryDir data;
    const auto first = data.filePath("first"), second = data.filePath("second"),
               alias = data.filePath("alias");
    QVERIFY(QDir().mkpath(first));
    QVERIFY(QDir().mkpath(second));
    QVERIFY(SessionStore(first).save({chat("first")}));
    QVERIFY(SessionStore(second).save({chat("second")}));
    const auto junction = [&](const QString &target) {
      QProcess process;
      process.setCreateProcessArgumentsModifier(
          [](QProcess::CreateProcessArguments *a) { a->flags |= 0x08000000; });
      process.start("cmd.exe",
                    {"/c", "mklink", "/J", QDir::toNativeSeparators(alias),
                     QDir::toNativeSeparators(target)});
      return process.waitForFinished(5000) && process.exitCode() == 0;
    };
    QVERIFY(junction(first));
    SessionStore original(alias);
    QCOMPARE(original.load().first().id, QString("first"));
    QVERIFY(QDir().rmdir(alias));
    QVERIFY(junction(second));
    SessionStore replacement(alias);
    QCOMPARE(replacement.load().first().id, QString("second"));
    QString error;
    QVERIFY(!original.canAdoptReload(replacement, &error));
    QVERIFY(error.contains("moved"));
    QVERIFY(QDir().rmdir(alias));
#endif
  }
  void failureDuringCancellationRetainsPartialHistoryForDecision() {
    QTemporaryDir data;
    HeldProvider server;
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),
                                {server.config()}));
    MainWindow window(data.path());
    show(window);
    window.submitPrompt("Running task");
    QTRY_COMPARE(server.requests, 1);
    auto *engine = window.findChild<AgentEngine *>();
    QVERIFY(engine);
    QTRY_VERIFY(QJsonDocument(engine->displayMessages())
                    .toJson()
                    .contains("Still running"));
    window.findChild<Composer *>("composerInput")
        ->setPlainText("Pending draft");
    const QByteArray conflict = "{changed-during-cancel";
    connect(engine, &AgentEngine::busyChanged, this, [&](bool busy) {
      if (!busy)
        QVERIFY(write(data.filePath("sessions.json"), conflict));
    });
    QVERIFY(!window.close());
    QVERIFY(!engine->isBusy());
    QVERIFY(window.isVisible());
    QVERIFY(window.findChild<QMessageBox *>("sessionCloseRecoveryDialog"));
    QVERIFY(
        QJsonDocument(engine->messages()).toJson().contains("Still running"));
    QCOMPARE(window.findChild<Composer *>("composerInput")->toPlainText(),
             QString("Pending draft"));
    QCOMPARE(read(data.filePath("sessions.json")), conflict);
  }
  void recoveryRemainsVisibleInSettings_data() {
    QTest::addColumn<QString>("theme");
    for (const auto &theme : {QString("light"), QString("dark")})
      QTest::newRow(qPrintable(theme)) << theme;
  }
  void recoveryRemainsVisibleInSettings() {
    QFETCH(QString, theme);
    QTemporaryDir data;
    QVERIFY(write(data.filePath("sessions.json"), "{broken"));
    MainWindow window(data.path());
    show(window);
    auto *settings = window.findChild<SettingsPage *>();
    QVERIFY(settings->setPreference("personalization.appearance", theme));
    window.resize(350, 600);
    window.findChild<QToolButton *>("settingsButton")->click();
    QCoreApplication::processEvents();
    auto *bar = window.findChild<QStatusBar *>("sessionRecoveryBar");
    auto *reload = window.findChild<QPushButton *>("reloadConversationsButton");
    auto *label = window.findChild<QLabel *>("sessionRecoveryMessage");
    QVERIFY(settings->isVisible());
    QVERIFY(bar && bar->isVisible());
    QVERIFY(reload && reload->isVisible());
    QVERIFY(label && !label->text().isEmpty());
    QVERIFY(window.rect().contains(
        QRect(reload->mapTo(&window, QPoint()), reload->size())));
    QCOMPARE(window.width(), 350);
    const auto renders = qEnvironmentVariable("ADE_RECOVERY_RENDER_DIR");
    if (!renders.isEmpty()) {
      QVERIFY(QDir().mkpath(renders));
      QVERIFY(window.grab().save(
          QDir(renders).filePath("storage-error-settings-" + theme + ".png")));
    }
  }
};
QTEST_MAIN(StartupRecoveryTests)
#include "test_startup_recovery.moc"
