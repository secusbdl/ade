#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "ui/AppStyle.h"
#include "ui/BrowserSessionStore.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/WorkbenchSplitter.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
class PageServer : public QTcpServer {
public:
  int requests = 0;
  QList<QPointer<QTcpSocket>> held;
  void reply(QTcpSocket *socket, const QByteArray &path) {
    const QByteArray page =
        "<!doctype html><title>Loaded " + path + "</title><p>HIDDEN_NATIVE_" +
        path + "</p><script>console.log('HIDDEN_LOG_READY')</script>";
    socket->write(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
        QByteArray::number(page.size()) + "\r\nConnection: close\r\n\r\n" +
        page);
    socket->disconnectFromHost();
  }
  void release() {
    const auto pending = held;
    held.clear();
    for (const auto &socket : pending)
      if (socket)
        reply(socket, socket->property("path").toByteArray());
  }
  PageServer() {
    listen(QHostAddress::LocalHost);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          const auto bytes = socket->readAll();
          if (socket->property("sent").toBool())
            return;
          socket->setProperty("sent", true);
          ++requests;
          const auto path = bytes.split(' ').value(1);
          socket->setProperty("path", path);
          if (path.startsWith("/abort")) {
            socket->abort();
            return;
          }
          if (path.startsWith("/hold")) {
            held.append(socket);
            return;
          }
          reply(socket, path);
        });
      }
    });
  }
  QUrl url(const QString &suffix = "first") const {
    return QUrl(
        QString("http://127.0.0.1:%1/%2").arg(serverPort()).arg(suffix));
  }
};
QJsonObject arguments(const QString &tab) {
  return {{"tabId", tab},
          {"method", "Runtime.evaluate"},
          {"params", QJsonObject{{"expression", "document.body.textContent"},
                                 {"returnByValue", true}}}};
}
QList<QVariant> result(QSignalSpy &spy, const QString &id) {
  for (const auto &row : spy)
    if (row[0].toString() == id)
      return row;
  return {};
}
QJsonObject evaluate(const QString &tab, const QString &expression) {
  return {{"tabId", tab},
          {"method", "Runtime.evaluate"},
          {"params", QJsonObject{{"expression", expression}, {"returnByValue", true}}}};
}
int numericValue(const QString &reply) {
  static const QRegularExpression value(R"("value"\s*:\s*(\d+))");
  const auto match = value.match(reply);
  return match.hasMatch() ? match.captured(1).toInt() : -1;
}
bool readyResult(QSignalSpy &spy, const QString &id, int ms = 15000) {
  QElapsedTimer timer;
  timer.start();
  while (result(spy, id).isEmpty() && timer.elapsed() < ms)
    QTest::qWait(10);
  return !result(spy, id).isEmpty();
}
void seed(const QString &data, PageServer &page, bool collapsed) {
  Conversation c;
  c.id = "fixture";
  c.title = "Hidden tab fixture";
  SessionStore(data).save({c});
  BrowserSessionState state{
      {{"saved-one", page.url(), "Saved first title"},
       {"saved-two", page.url("second"), "Saved second title"}},
      "saved-one"};
  BrowserSessionStore(data).save(state);
  QSettings settings(data + "/window.ini", QSettings::IniFormat);
  settings.setValue("contentCollapsed", collapsed);
  settings.sync();
}
} // namespace
class HiddenBrowserTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { initializeAppStyle(*qApp); }
  void neverShownTabInitializesForScopedRequest() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.resize(600, 400);
    QVERIFY(browser.setProfileDirectory(profile.path()));
    browser.navigate(page.url());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QVERIFY(!browser.isVisible());
    QVERIFY(!browser.isReady());
    QCOMPARE(page.requests, 0);
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("hidden-read", "fixture", "sendCDP",
                           arguments("hidden"));
    QVERIFY(readyResult(completed, "hidden-read"));
    const auto reply = result(completed, "hidden-read");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QVERIFY(reply[1].toString().contains("HIDDEN_NATIVE_/first"));
    QVERIFY(browser.isReady());
    QVERIFY(!browser.isVisible());
    QVERIFY(page.requests > 0);
  }
  void restoredMetadataIsAvailableWithoutLoading_data() {
    QTest::addColumn<bool>("collapsed");
    QTest::newRow("expanded") << false;
    QTest::newRow("collapsed") << true;
  }
  void restoredMetadataIsAvailableWithoutLoading() {
    QFETCH(bool, collapsed);
    QTemporaryDir data;
    PageServer page;
    seed(data.path(), page, collapsed);
    MainWindow window(data.path());
    auto *bridge = window.findChild<BrowserToolBridge *>();
    QVERIFY(bridge);
    QCOMPARE(page.requests, 0);
    QSignalSpy completed(bridge, &BrowserToolBridge::completed);
    bridge->executeForAgent("tabs", "fixture", "getBrowserTabs", {});
    QVERIFY(readyResult(completed, "tabs"));
    const auto document = QJsonDocument::fromJson(
                              result(completed, "tabs")[1].toString().toUtf8())
                              .object();
    const auto tabs = document.value("tabs").toArray();
    QCOMPARE(tabs.size(), 2);
    QCOMPARE(tabs[0].toObject().value("url").toString(), page.url().toString());
    QCOMPARE(tabs[0].toObject().value("title").toString(),
             QString("Saved first title"));
    QVERIFY(!tabs[0].toObject().value("ready").toBool());
    QCOMPARE(page.requests, 0);
    QVERIFY(window.close());
  }
  void restoredBackgroundAndCollapsedTabsAreUsable_data() {
    QTest::addColumn<bool>("collapsed");
    QTest::addColumn<int>("index");
    for (bool collapse : {false, true})
      for (int index : {0, 1})
        QTest::newRow(qPrintable(QString("%1-%2")
                                     .arg(collapse ? "collapsed" : "expanded")
                                     .arg(index)))
            << collapse << index;
  }
  void restoredBackgroundAndCollapsedTabsAreUsable() {
    QFETCH(bool, collapsed);
    QFETCH(int, index);
    QTemporaryDir data;
    PageServer page;
    seed(data.path(), page, collapsed);
    MainWindow window(data.path());
    auto *tabs = window.findChild<QTabWidget *>("contentTabs");
    auto *bridge = window.findChild<BrowserToolBridge *>();
    QVERIFY(tabs && bridge);
    QCOMPARE(tabs->count(), 2);
    const auto before = tabs->currentWidget();
    const auto id = tabs->widget(index)->property("browserTabId").toString();
    QVERIFY(!id.isEmpty());
    QSignalSpy completed(bridge, &BrowserToolBridge::completed);
    bridge->executeForAgent("restored-read", "fixture", "sendCDP",
                            arguments(id));
    QVERIFY(readyResult(completed, "restored-read"));
    const auto reply = result(completed, "restored-read");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QVERIFY(reply[1].toString().contains(index ? "HIDDEN_NATIVE_/second"
                                               : "HIDDEN_NATIVE_/first"));
    QCOMPARE(tabs->currentWidget(), before);
    QCOMPARE(window.findChild<WorkbenchSplitter *>()->contentCollapsed(),
             collapsed);
    QVERIFY(!window.isVisible());
    QVERIFY(window.close());
  }
  void foreignRequestCannotStartHiddenTab() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProperty("adeOwnerAgentId", "owner");
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "owned");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("foreign", "other", "sendCDP", arguments("owned"));
    QVERIFY(readyResult(completed, "foreign"));
    QVERIFY(result(completed, "foreign")[2].toBool());
    QVERIFY(!browser.isReady());
    QCOMPARE(page.requests, 0);
  }
  void hiddenConsoleRequestWaitsForInitialDocument() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "console");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("logs", "fixture", "readConsoleLogs",
                           {{"id", "console"}, {"delayMs", 100}});
    QVERIFY(readyResult(completed, "logs"));
    const auto reply = result(completed, "logs");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QVERIFY(reply[1].toString().contains("HIDDEN_LOG_READY"));
    QVERIFY(!browser.isVisible());
  }
  void readyVisibleTabDispatchesDuringFirstLoad() {
    // Upstream sendCDP never waits for navigation: a visible, ready tab whose
    // first page is still loading answers at once, not at the request deadline.
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.resize(600, 400);
    QVERIFY(browser.setProfileDirectory(profile.path()));
    browser.navigate(page.url("hold-visible"));
    browser.show();
    QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 15000);
    QVERIFY(!browser.hasCompletedNavigation());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "visible");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    QElapsedTimer timer;
    timer.start();
    bridge.executeForAgent("visible-eval", "fixture", "sendCDP",
                           evaluate("visible", "40+2"));
    QVERIFY(readyResult(completed, "visible-eval", 8000));
    const auto reply = result(completed, "visible-eval");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QCOMPARE(numericValue(reply[1].toString()), 42);
    QVERIFY(timer.elapsed() < 8000);
    QVERIFY(!page.held.isEmpty()); // Answered while the first page was still held.
    page.release();
  }
  void aboutBlankHiddenTabAcceptsCdp() {
    QTemporaryDir profile;
    BrowserWidget browser;
    browser.resize(600, 400);
    QVERIFY(browser.setProfileDirectory(profile.path()));
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "blank");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("blank-eval", "fixture", "sendCDP",
                           evaluate("blank", "location.href"));
    QVERIFY(readyResult(completed, "blank-eval"));
    const auto reply = result(completed, "blank-eval");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QVERIFY2(reply[1].toString().contains("about:blank"), qPrintable(reply[1].toString()));
    QVERIFY(!browser.isVisible());
  }
  void resumedStartupRequestExecutesOnce() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.resize(600, 400);
    QVERIFY(browser.setProfileDirectory(profile.path()));
    browser.navigate(page.url("hold-once"));
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "once");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("count", "fixture", "sendCDP",
                           evaluate("once", "window.__adeCount=(window.__adeCount||0)+1"));
    QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 15000);
    QTest::qWait(300);
    QVERIFY(result(completed, "count").isEmpty()); // Waits for the initial document.
    page.release();
    QVERIFY(readyResult(completed, "count"));
    QVERIFY2(!result(completed, "count")[2].toBool(),
             qPrintable(result(completed, "count")[1].toString()));
    QTest::qWait(300);
    bridge.executeForAgent("read", "fixture", "sendCDP",
                           evaluate("once", "window.__adeCount"));
    QVERIFY(readyResult(completed, "read"));
    const auto reply = result(completed, "read");
    QVERIFY2(!reply[2].toBool(), qPrintable(reply[1].toString()));
    QCOMPARE(numericValue(reply[1].toString()), 1);
  }
  void hiddenTabUsesContentAreaViewport() {
    QTemporaryDir data;
    PageServer page;
    seed(data.path(), page, false);
    MainWindow window(data.path());
    window.resize(1200, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *tabs = window.findChild<QTabWidget *>("contentTabs");
    auto *bridge = window.findChild<BrowserToolBridge *>();
    QVERIFY(tabs && bridge);
    QCOMPARE(tabs->count(), 2);
    const int shown = tabs->currentIndex();
    const int hidden = shown == 0 ? 1 : 0;
    const auto shownId = tabs->widget(shown)->property("browserTabId").toString();
    const auto hiddenId = tabs->widget(hidden)->property("browserTabId").toString();
    QSignalSpy completed(bridge, &BrowserToolBridge::completed);
    bridge->executeForAgent("shown-width", "fixture", "sendCDP",
                            evaluate(shownId, "window.innerWidth"));
    QVERIFY(readyResult(completed, "shown-width"));
    bridge->executeForAgent("hidden-width", "fixture", "sendCDP",
                            evaluate(hiddenId, "window.innerWidth"));
    QVERIFY(readyResult(completed, "hidden-width"));
    const int shownWidth = numericValue(result(completed, "shown-width")[1].toString());
    const int hiddenWidth = numericValue(result(completed, "hidden-width")[1].toString());
    QVERIFY2(shownWidth > 300, qPrintable(QString::number(shownWidth)));
    QVERIFY2(hiddenWidth >= shownWidth * 8 / 10,
             qPrintable(QString("hidden %1 shown %2").arg(hiddenWidth).arg(shownWidth)));
    QCOMPARE(tabs->currentIndex(), shown);
    QVERIFY(!tabs->widget(hidden)->isVisible());
    QVERIFY(window.close());
  }
  void malformedRequestDoesNotInitialize() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("bad", "fixture", "sendCDP",
                           {{"tabId", "hidden"}, {"method", "invalid method"}});
    QVERIFY(readyResult(completed, "bad"));
    QVERIFY(result(completed, "bad")[2].toBool());
    QVERIFY(!browser.isReady());
    QCOMPARE(page.requests, 0);
  }
  void cancelDuringInitialLoadNeverExecutesQueuedCdp() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url("hold"));
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    auto args = arguments("hidden");
    args["params"] = QJsonObject{{"expression", "window.unwantedMutation=true"},
                                 {"returnByValue", true}};
    bridge.executeForAgent("cancelled", "fixture", "sendCDP", args);
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 10000);
    QVERIFY(result(completed, "cancelled").isEmpty());
    bridge.cancel("cancelled");
    QCOMPARE(completed.size(), 1);
    QVERIFY(result(completed, "cancelled")[2].toBool());
    page.release();
    QTRY_VERIFY(browser.hasCompletedNavigation());
    args["params"] =
        QJsonObject{{"expression", "Boolean(window.unwantedMutation)"},
                    {"returnByValue", true}};
    bridge.executeForAgent("check", "fixture", "sendCDP", args);
    QVERIFY(readyResult(completed, "check"));
    const auto resultObject =
        QJsonDocument::fromJson(
            result(completed, "check")[1].toString().toUtf8())
            .object();
    QVERIFY(!resultObject.value("result").toObject().value("value").toBool());
    QCOMPARE(completed.size(), 2);
  }
  void deadlineDuringInitialLoadSettlesOnce() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url("hold"));
    BrowserToolBridge bridge;
    bridge.setRequestTimeoutMs(1500);
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("deadline", "fixture", "sendCDP",
                           arguments("hidden"));
    QVERIFY(readyResult(completed, "deadline", 5000));
    QVERIFY(result(completed, "deadline")[2].toBool());
    QVERIFY(result(completed, "deadline")[1].toString().contains("timed out"));
    page.release();
    QTRY_VERIFY(browser.hasCompletedNavigation());
    QCOMPARE(completed.size(), 1);
  }
  void ownerRecheckedAfterInitialization() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProperty("adeOwnerAgentId", "first");
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url("hold"));
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("owned", "first", "sendCDP", arguments("hidden"));
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 10000);
    browser.setProperty("adeOwnerAgentId", "other");
    page.release();
    QVERIFY(readyResult(completed, "owned"));
    QVERIFY(result(completed, "owned")[2].toBool());
    QVERIFY(result(completed, "owned")[1].toString().contains("another agent"));
    QCOMPARE(completed.size(), 1);
  }
  void reusedRequestIdRetainsOriginalCorrelation() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget oldBrowser, replacement;
    oldBrowser.setProfileDirectory(profile.filePath("old"));
    replacement.setProfileDirectory(profile.filePath("replacement"));
    oldBrowser.navigate(page.url("hold"));
    replacement.navigate(page.url("replacement"));
    BrowserToolBridge bridge;
    bridge.registerTab(&oldBrowser, "old");
    bridge.registerTab(&replacement, "new");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("same", "fixture", "sendCDP", arguments("old"));
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 10000);
    bridge.cancel("same");
    QCOMPARE(completed.size(), 1);
    bridge.executeForAgent("same", "fixture", "sendCDP", arguments("new"));
    page.release();
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 15000);
    QVERIFY(!completed[1][2].toBool());
    QVERIFY(completed[1][1].toString().contains("HIDDEN_NATIVE_/replacement"));
  }
  void initialFailureCanRetryAfterProfileRepair() {
    QTemporaryDir profile;
    PageServer page;
    const auto blocked = profile.filePath("blocked");
    QFile file(blocked);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("fixture");
    file.close();
    BrowserWidget browser;
    browser.setProfileDirectory(blocked);
    browser.navigate(page.url());
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("failed", "fixture", "sendCDP", arguments("hidden"));
    QVERIFY(readyResult(completed, "failed"));
    QVERIFY(result(completed, "failed")[2].toBool());
    QVERIFY(result(completed, "failed")[1].toString().contains(
        "initialization failed"));
    QVERIFY(!browser.isReady());
    QCOMPARE(page.requests, 0);
    QVERIFY(browser.setProfileDirectory(profile.filePath("repaired")));
    bridge.executeForAgent("repaired", "fixture", "sendCDP",
                           arguments("hidden"));
    QVERIFY(readyResult(completed, "repaired"));
    QVERIFY(!result(completed, "repaired")[2].toBool());
    QVERIFY(
        result(completed, "repaired")[1].toString().contains("HIDDEN_NATIVE_"));
  }
  void completionMayDeleteBridgeDuringSynchronousInitFailure() {
    QTemporaryDir profile;
    QFile file(profile.filePath("blocked"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    BrowserWidget browser;
    browser.setProfileDirectory(file.fileName());
    browser.navigate(QUrl("about:blank"));
    QPointer<BrowserToolBridge> bridge = new BrowserToolBridge;
    bridge->registerTab(&browser, "hidden");
    int calls = 0;
    connect(bridge, &BrowserToolBridge::completed, this,
            [&](const QString &, const QString &, bool error) {
              QVERIFY(error);
              ++calls;
              delete bridge.data();
            });
    bridge->executeForAgent("failed", "fixture", "sendCDP",
                            arguments("hidden"));
    QTRY_VERIFY(!bridge);
    QCOMPARE(calls, 1);
  }
  void initialNavigationFailureSettlesBeforeDeadline() {
    QTemporaryDir profile;
    PageServer page;
    BrowserWidget browser;
    browser.setProfileDirectory(profile.path());
    browser.navigate(page.url("abort"));
    BrowserToolBridge bridge;
    bridge.registerTab(&browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("failed-load", "fixture", "sendCDP",
                           arguments("hidden"));
    QVERIFY(readyResult(completed, "failed-load"));
    const auto reply = result(completed, "failed-load");
    QVERIFY(reply[2].toBool());
    QVERIFY2(reply[1].toString().contains("failed to load"),
             qPrintable(reply[1].toString()));
    QCOMPARE(completed.size(), 1);
    QVERIFY(browser.isReady());
    QVERIFY(!browser.isVisible());
  }
  void initialMetadataObserverMayDeleteWindow() {
    QTemporaryDir data;
    Conversation chat;
    chat.id = "fixture";
    QVERIFY(SessionStore(data.path()).save({chat}));
    QPointer<MainWindow> window = new MainWindow(data.path());
    auto *bridge = window->findChild<BrowserToolBridge *>();
    auto *button = window->findChild<QToolButton *>("newBrowserTabButton");
    QVERIFY(bridge);
    QVERIFY(button);
    int observed = 0;
    connect(bridge, &BrowserToolBridge::tabsChanged, this, [&, bridge] {
      const auto snapshot = bridge->snapshotForAgent("fixture");
      if (!window || snapshot.tabs.isEmpty() ||
          snapshot.tabs.first().url != "about:blank")
        return;
      ++observed;
      delete window.data();
    });
    button->click();
    QVERIFY(window.isNull());
    QCOMPARE(observed, 1);
  }
  void closingInitializingTabSettlesRequest() {
    QTemporaryDir profile;
    PageServer page;
    QPointer<BrowserWidget> browser = new BrowserWidget;
    browser->setProfileDirectory(profile.path());
    browser->navigate(page.url("hold"));
    BrowserToolBridge bridge;
    bridge.registerTab(browser, "hidden");
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    bridge.executeForAgent("closed", "fixture", "sendCDP", arguments("hidden"));
    QTRY_VERIFY_WITH_TIMEOUT(!page.held.isEmpty(), 10000);
    delete browser.data();
    QVERIFY(readyResult(completed, "closed"));
    QVERIFY(result(completed, "closed")[2].toBool());
    page.release();
    QCoreApplication::processEvents();
    QCOMPARE(completed.size(), 1);
  }
};
QTEST_MAIN(HiddenBrowserTests)
#include "test_hidden_browser.moc"
