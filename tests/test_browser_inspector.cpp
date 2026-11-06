#include "browser/BrowserInspector.h"
#include "browser/browserwidget.h"
#include "ui/BrowserViewport.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
using namespace ade;

class BrowserInspectorTest : public QObject {
  Q_OBJECT
  QTcpServer server;
  QTemporaryDir profiles;
  int serial = 0;
  bool wait(const std::function<bool()> &condition, int timeout = 12000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
      if (condition())
        return true;
      QTest::qWait(10);
    }
    return false;
  }
  QUrl url(const QString &path = "/") {
    return QUrl(
        QString("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
  }
  bool load(BrowserWidget &browser, const QString &path = "/") {
    browser.setAttribute(Qt::WA_DontShowOnScreen);
    browser.resize(900, 700);
    browser.setProfileDirectory(profiles.path() + "/" +
                                QString::number(++serial));
    QSignalSpy loaded(&browser, &BrowserWidget::loadingChanged);
    browser.navigate(url(path));
    browser.show();
    return wait([&] {
      for (const auto &row : loaded)
        if (!row[0].toBool() && browser.isReady())
          return true;
      return false;
    });
  }
  QJsonObject cdp(BrowserWidget &browser, const QString &method,
                  const QJsonObject &parameters = {}) {
    const QString id = "fixture-" + QString::number(++serial);
    QSignalSpy replies(&browser, &BrowserWidget::devToolsRequestCompleted);
    browser.callDevToolsRequest(
        id, method,
        QString::fromUtf8(
            QJsonDocument(parameters).toJson(QJsonDocument::Compact)));
    QJsonObject result;
    const bool received = wait([&] {
      for (const auto &row : replies)
        if (row[0].toString() == id) {
          result = row[2].toBool()
                       ? QJsonObject{{"fixtureError", row[1].toString()}}
                       : QJsonDocument::fromJson(row[1].toString().toUtf8())
                             .object();
          return true;
        }
      return false;
    });
    return received ? result : QJsonObject{{"fixtureError", "timeout"}};
  }
  int node(BrowserWidget &browser, const QString &selector) {
    const auto document = cdp(browser, "DOM.getDocument");
    const auto queried = cdp(browser, "DOM.querySelector",
                             {{"nodeId", document["root"].toObject()["nodeId"]},
                              {"selector", selector}});
    return cdp(browser, "DOM.describeNode",
               {{"nodeId", queried["nodeId"]}})["node"]
        .toObject()["backendNodeId"]
        .toInt();
  }
private slots:
  void cancellationInsideBusyObservers_data() {
    QTest::addColumn<bool>("onBusy");
    QTest::newRow("starting") << true;
    QTest::newRow("ready") << false;
  }
  void cancellationInsideBusyObservers() {
    QFETCH(bool, onBusy);
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy selection(&inspector, &BrowserInspector::selectingChanged);
    bool canceled = false;
    connect(&inspector, &BrowserInspector::busyChanged, &inspector,
            [&](bool busy) {
              if (!canceled && busy == onBusy) {
                canceled = true;
                inspector.setSelecting(false);
              }
            });
    inspector.setSelecting(true);
    QVERIFY(wait([&] { return canceled; }));
    QTest::qWait(200);
    QVERIFY(!inspector.isSelecting());
    QVERIFY(!inspector.isBusy());
    for (const auto &signal : selection)
      QVERIFY(!signal[0].toBool());
  }
  void replacementSelectionRetiresOldCompletionSignals() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    inspector.setSelecting(true);
    QVERIFY(wait([&] { return inspector.isSelecting(); }));
    const auto target = node(browser, "#target");
    QSignalSpy busy(&inspector, &BrowserInspector::busyChanged);
    bool restarted = false;
    connect(&inspector, &BrowserInspector::selectingChanged, &inspector,
            [&](bool selecting) {
              if (!selecting && !restarted) {
                restarted = true;
                inspector.setSelecting(true);
              }
            });
    inspector.inspectNode(target);
    inspector.setSelecting(false);
    QVERIFY(restarted);
    QVERIFY(inspector.isBusy());
    QVERIFY(!busy.isEmpty());
    QVERIFY(busy.last()[0].toBool());
    QVERIFY(wait([&] { return inspector.isSelecting(); }));
  }
  void initTestCase() {
    QVERIFY(profiles.isValid());
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          auto request =
              socket->property("request").toByteArray() + socket->readAll();
          socket->setProperty("request", request);
          if (!request.contains("\r\n\r\n"))
            return;
          const QByteArray body =
              R"HTML(<!doctype html><html><head><title>Selection fixture</title><meta name="viewport" content="width=device-width,initial-scale=1"></head>
<body style="margin:0"><main><button id="target" data-context="hello &amp; world" style="position:absolute;left:30px;top:40px;width:200px;height:90px;color:rgb(12,34,56)">Pick me</button><input id="password" type="password" value="test-only-sensitive-value"><div id="later">Later</div></main></body></html>)HTML";
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                        "text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
      }
    });
  }
  void presetsMatchPinnedSource() {
    const auto presets = BrowserInspector::devicePresets();
    QCOMPARE(presets.size(), 10);
    QCOMPARE(presets[1].toObject()["width"].toInt(), 402);
    QCOMPARE(presets[5].toObject()["deviceScaleFactor"].toDouble(), 2.625);
  }
  void capturesSelectedElementAndExcludesInputValue() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy selected(&inspector, &BrowserInspector::elementSelected),
        errors(&inspector, &BrowserInspector::failed);
    const auto id = node(browser, "#target");
    QVERIFY(id > 0);
    inspector.inspectNode(id);
    QVERIFY(wait([&] { return selected.count() == 1 || !errors.isEmpty(); }));
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
    const auto element = selected.first()[0].toJsonObject();
    QCOMPARE(element["tagName"].toString(), "button");
    QCOMPARE(element["innerText"].toString(), "Pick me");
    QCOMPARE(element["attributes"].toObject()["data-context"].toString(),
             "hello & world");
    QCOMPARE(element["url"].toString(), url().toString());
    QCOMPARE(element["boundingClientRect"].toObject()["width"].toInt(), 200);
    QVERIFY(element["computedStyles"].toObject()["color"].toString().contains(
        "12"));
    QVERIFY(!inspector.isBusy());
    inspector.inspectNode(node(browser, "#password"));
    QVERIFY(wait([&] { return selected.count() == 2 || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    QVERIFY(
        !selected.last()[0].toJsonObject()["attributes"].toObject().contains(
            "value"));
  }
  void overlaySelectionAndCancellation() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy selected(&inspector, &BrowserInspector::elementSelected),
        errors(&inspector, &BrowserInspector::failed);
    inspector.setSelecting(true);
    QVERIFY(wait([&] { return inspector.isSelecting() || !errors.isEmpty(); }));
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
    cdp(browser, "Input.dispatchMouseEvent",
        {{"type", "mouseMoved"}, {"x", 70}, {"y", 70}});
    cdp(browser, "Input.dispatchMouseEvent",
        {{"type", "mousePressed"},
         {"x", 70},
         {"y", 70},
         {"button", "left"},
         {"clickCount", 1}});
    cdp(browser, "Input.dispatchMouseEvent",
        {{"type", "mouseReleased"},
         {"x", 70},
         {"y", 70},
         {"button", "left"},
         {"clickCount", 1}});
    QVERIFY(wait([&] { return selected.count() == 1 || !errors.isEmpty(); }));
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
    QCOMPARE(selected.first()[0].toJsonObject()["tagName"].toString(),
             "button");
    QVERIFY(!inspector.isSelecting());
    inspector.setSelecting(true);
    inspector.setSelecting(false);
    QTest::qWait(100);
    QVERIFY(!inspector.isSelecting());
    QVERIFY(!inspector.isBusy());
  }
  void deviceViewportAndReset() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy changed(&inspector, &BrowserInspector::deviceChanged),
        errors(&inspector, &BrowserInspector::failed);
    inspector.setDevice(BrowserInspector::devicePresets()[1].toObject());
    QVERIFY(wait([&] { return changed.count() == 1 || !errors.isEmpty(); }));
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors.first()[0].toString()));
    auto state =
        cdp(browser, "Runtime.evaluate",
            {{"expression", "({width:innerWidth,ratio:devicePixelRatio,touch:"
                            "navigator.maxTouchPoints})"},
             {"returnByValue", true}})["result"]
            .toObject()["value"]
            .toObject();
    QCOMPARE(state["width"].toInt(), 402);
    QCOMPARE(state["ratio"].toInt(), 3);
    QVERIFY(state["touch"].toInt() > 0);
    inspector.setDevice({});
    QVERIFY(wait([&] { return changed.count() == 2 || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    state = cdp(browser, "Runtime.evaluate",
                {{"expression",
                  "({width:innerWidth,touch:navigator.maxTouchPoints})"},
                 {"returnByValue", true}})["result"]
                .toObject()["value"]
                .toObject();
    QVERIFY(state["width"].toInt() > 402);
    QCOMPARE(state["touch"].toInt(), 0);
    QVERIFY(inspector.device().isEmpty());
    inspector.setDevice({{"width", 1}, {"height", 2}});
    QCOMPARE(errors.count(), 1);
    QVERIFY(!inspector.isBusy());
  }
  void screenshotAndNavigationRetirement() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy screenshots(&inspector, &BrowserInspector::screenshotCaptured),
        selected(&inspector, &BrowserInspector::elementSelected),
        errors(&inspector, &BrowserInspector::failed);
    inspector.captureScreenshot();
    QVERIFY(
        wait([&] { return screenshots.count() == 1 || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    const auto image = QImage::fromData(screenshots.first()[0].toByteArray());
    QVERIFY(!image.isNull());
    QVERIFY(image.width() > 0);
    inspector.inspectNode(node(browser, "#target"));
    emit browser.loadingChanged(true);
    QTest::qWait(100);
    QCOMPARE(selected.count(), 0);
    QVERIFY(!inspector.isBusy());
  }
  void captureContextIsPinnedBeforeObserversAndSelection() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(load(browser));
    QSignalSpy screenshots(&inspector, &BrowserInspector::screenshotCaptured),
        selected(&inspector, &BrowserInspector::elementSelected),
        errors(&inspector, &BrowserInspector::failed);
    inspector.setProperty("captureAgentId", "original");
    connect(&inspector, &BrowserInspector::busyChanged, &inspector,
            [&](bool busy) {
              if (busy)
                inspector.setProperty("captureAgentId", "replacement");
            });
    inspector.captureScreenshot();
    QVERIFY(wait([&] { return !screenshots.isEmpty() || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    QCOMPARE(screenshots.first()[1].toString(), "original");
    inspector.setProperty("captureAgentId", "selection-owner");
    inspector.setSelecting(true);
    QVERIFY(wait([&] { return inspector.isSelecting() || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    inspector.setProperty("captureAgentId", "other-chat");
    inspector.inspectNode(node(browser, "#target"));
    QVERIFY(wait([&] { return !selected.isEmpty() || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    QCOMPARE(selected.first()[1].toString(), "selection-owner");
  }
  void fittedViewportKeepsLogicalDeviceSize() {
    auto *browser = new BrowserWidget;
    browser->setProfileDirectory(profiles.path() + "/fitted");
    auto *inspector = new BrowserInspector(browser, browser);
    BrowserViewport viewport(browser, inspector);
    viewport.setAttribute(Qt::WA_DontShowOnScreen);
    viewport.resize(600, 500);
    QSignalSpy loaded(browser, &BrowserWidget::loadingChanged);
    browser->navigate(url());
    viewport.show();
    QVERIFY(wait([&] {
      for (const auto &row : loaded)
        if (!row[0].toBool())
          return true;
      return false;
    }));
    QTest::qWait(50);
    QSignalSpy changed(inspector, &BrowserInspector::deviceChanged),
        errors(inspector, &BrowserInspector::failed);
    inspector->setDevice(BrowserInspector::devicePresets()[1].toObject());
    QVERIFY(wait([&] { return !changed.isEmpty() || !errors.isEmpty(); }));
    QVERIFY(errors.isEmpty());
    QTest::qWait(75);
    QVERIFY(browser->width() < 402);
    QVERIFY(browser->height() <= viewport.height() - 30);
    auto logical = cdp(*browser, "Runtime.evaluate",
                       {{"expression", "innerWidth"}, {"returnByValue", true}});
    QCOMPARE(logical["result"].toObject()["value"].toInt(), 402);
    const auto before = browser->size();
    viewport.resize(900, 1000);
    QVERIFY(wait([&] {
      return browser->height() > before.height() && !inspector->isBusy();
    }));
    QCOMPARE(browser->width(), 402);
    inspector->setDevice({});
    QVERIFY(wait(
        [&] { return inspector->device().isEmpty() && !inspector->isBusy(); }));
    QCOMPARE(browser->size(), viewport.size());
  }
  void observersMayDestroyInspector() {
    BrowserWidget browser;
    auto *inspector = new BrowserInspector(&browser);
    QPointer<BrowserInspector> guard(inspector);
    connect(inspector, &BrowserInspector::busyChanged, inspector,
            [inspector](bool busy) {
              if (busy)
                delete inspector;
            });
    inspector->setSelecting(true);
    QVERIFY(guard.isNull());
    inspector = new BrowserInspector(&browser);
    guard = inspector;
    connect(inspector, &BrowserInspector::failed, inspector,
            [inspector] { delete inspector; });
    inspector->setDevice({{"width", 0}});
    QVERIFY(guard.isNull());
  }
};
QTEST_MAIN(BrowserInspectorTest)
#include "test_browser_inspector.moc"
