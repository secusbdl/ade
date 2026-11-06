#include "browser/browserwidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using ade::BrowserWidget;

class BrowserTest final : public QObject {
    Q_OBJECT
private:
    QTcpServer server;
    QUrl address(const QString& path = QStringLiteral("/one")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
    }
    static void prepare(BrowserWidget& browser) {
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(800, 600);
    }
    static bool waitLoaded(BrowserWidget& browser, const QUrl& url) {
        QSignalSpy complete(&browser, &BrowserWidget::loadingChanged);
        browser.navigate(url);
        browser.show();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 15000) {
            for (const auto& row : complete)
                if (!row[0].toBool() && browser.currentUrl() == url) return true;
            QTest::qWait(20);
        }
        return false;
    }

private slots:
    void initTestCase() {
        QCoreApplication::setApplicationName(QStringLiteral("ADE-Browser-Tests-%1").arg(QCoreApplication::applicationPid()));
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    const QByteArray path = request.split(' ').value(1);
                    if (path.startsWith("/drop")) { socket->abort(); return; }
                    const QByteArray name = path.startsWith("/two") ? QByteArray("two") : QByteArray("one");
                    const QByteArray body = "<!doctype html><html><head><title>ADE " + name
                        + "</title></head><body style='margin:0;background:#17202a;color:white'><h1 id='value'>"
                        + name + "</h1><a href='/two' target='_blank' id='new-window'>Second page</a></body></html>";
                    const auto send = [socket, body] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
                            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    };
                    if (path.startsWith("/slow")) QTimer::singleShot(250, socket, send);
                    else send();
                });
            }
        });
    }

    void screenshotBeforeReadyIsReported() {
        BrowserWidget browser;
        QSignalSpy errors(&browser, &BrowserWidget::errorOccurred);
        QSignalSpy screenshots(&browser, &BrowserWidget::screenshotReady);
        browser.requestScreenshot();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(screenshots.size(), 0);
    }

    void queuedNavigationAndDomAccess() {
        BrowserWidget browser;
        prepare(browser);
        QSignalSpy ready(&browser, &BrowserWidget::readyChanged);
        QSignalSpy title(&browser, &BrowserWidget::titleChanged);
        QSignalSpy errors(&browser, &BrowserWidget::errorOccurred);
        browser.navigate(address());
        QVERIFY(!browser.isReady());
        QVERIFY(waitLoaded(browser, address()));
        QCOMPARE(ready.size(), 1);
        QVERIFY(browser.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(!title.isEmpty(), 3000);
        QCOMPARE(title.last().at(0).toString(), QStringLiteral("ADE one"));
        QSignalSpy result(&browser, &BrowserWidget::scriptResult);
        browser.executeScript(QStringLiteral("document.getElementById('value').textContent"));
        QTRY_COMPARE_WITH_TIMEOUT(result.size(), 1, 3000);
        QCOMPARE(result.first().at(0).toString(), QStringLiteral("\"one\""));
        QCOMPARE(errors.size(), 0);
    }

    void failedNavigationRecovers() {
        BrowserWidget browser;
        prepare(browser);
        QVERIFY(waitLoaded(browser, address()));
        QSignalSpy errors(&browser, &BrowserWidget::errorOccurred);
        QSignalSpy loading(&browser, &BrowserWidget::loadingChanged);
        browser.navigate(address(QStringLiteral("/drop")));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
        QVERIFY(loading.size() >= 2);
        QCOMPARE(loading.last().at(0).toBool(), false);
        QVERIFY(waitLoaded(browser, address(QStringLiteral("/two"))));
        QCOMPARE(browser.currentUrl(), address(QStringLiteral("/two")));
    }

    void resizeAndScreenshot() {
        BrowserWidget browser;
        prepare(browser);
        QVERIFY(waitLoaded(browser, address()));
        QSignalSpy scripts(&browser, &BrowserWidget::scriptResult);
        browser.executeScript(QStringLiteral("innerWidth"));
        QTRY_COMPARE_WITH_TIMEOUT(scripts.size(), 1, 3000);
        const int initialWidth = scripts.takeFirst().at(0).toString().toInt();
        browser.resize(1050, 720);
        QTest::qWait(100);
        browser.executeScript(QStringLiteral("innerWidth"));
        QTRY_COMPARE_WITH_TIMEOUT(scripts.size(), 1, 3000);
        const int resizedWidth = scripts.takeFirst().at(0).toString().toInt();
        QVERIFY2(resizedWidth > initialWidth + 150, "The WebView2 controller did not track the native widget resize");
        QSignalSpy screenshots(&browser, &BrowserWidget::screenshotReady);
        browser.requestScreenshot();
        QTRY_COMPARE_WITH_TIMEOUT(screenshots.size(), 1, 5000);
        const QImage png = QImage::fromData(screenshots.first().at(0).toByteArray(), "PNG");
        QVERIFY(!png.isNull());
        QVERIFY(png.width() >= 1050);
        QVERIFY(png.height() >= 720);
    }

    void twoViewsRemainIndependent() {
        BrowserWidget first, second;
        prepare(first);
        prepare(second);
        first.navigate(address());
        second.navigate(address(QStringLiteral("/two")));
        first.show();
        second.show();
        QTRY_VERIFY_WITH_TIMEOUT(first.isReady() && second.isReady(), 15000);
        QSignalSpy firstResult(&first, &BrowserWidget::scriptResult);
        QSignalSpy secondResult(&second, &BrowserWidget::scriptResult);
        QVERIFY(waitLoaded(first, address()));
        QVERIFY(waitLoaded(second, address(QStringLiteral("/two"))));
        first.executeScript(QStringLiteral("document.getElementById('value').textContent"));
        second.executeScript(QStringLiteral("document.getElementById('value').textContent"));
        QTRY_COMPARE_WITH_TIMEOUT(firstResult.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(secondResult.size(), 1, 3000);
        QCOMPARE(firstResult.first().at(0).toString(), QStringLiteral("\"one\""));
        QCOMPARE(secondResult.first().at(0).toString(), QStringLiteral("\"two\""));
    }

    void devToolsErrorAndSuccess() {
        BrowserWidget browser;
        prepare(browser);
        QVERIFY(waitLoaded(browser, address()));
        QSignalSpy errors(&browser, &BrowserWidget::errorOccurred);
        QSignalSpy results(&browser, &BrowserWidget::devToolsResult);
        browser.callDevTools(QStringLiteral("ADE.nonexistentMethod"), QStringLiteral("{}"));
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        QCOMPARE(results.size(), 0);
        browser.callDevTools(QStringLiteral("Runtime.evaluate"), QStringLiteral("{\"expression\":\"6*7\",\"returnByValue\":true}"));
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 3000);
        const QJsonObject response = QJsonDocument::fromJson(results.first().at(0).toString().toUtf8()).object();
        QCOMPARE(response.value(QStringLiteral("result")).toObject().value(QStringLiteral("value")).toInt(), 42);
        browser.callDevTools(QStringLiteral("Runtime.evaluate"), QStringLiteral("[]"));
        QCOMPARE(errors.size(), 2);
    }

    void readySignalDoesNotOverwriteNavigation() {
        BrowserWidget browser;
        prepare(browser);
        connect(&browser, &BrowserWidget::readyChanged, &browser, [&] {
            browser.navigate(address(QStringLiteral("/two")));
        });
        browser.navigate(address());
        browser.show();
        QTRY_COMPARE_WITH_TIMEOUT(browser.currentUrl(), address(QStringLiteral("/two")), 15000);
        QTest::qWait(400);
        QCOMPARE(browser.currentUrl(), address(QStringLiteral("/two")));
    }

    void popupStaysInPreview() {
        BrowserWidget browser;
        prepare(browser);
        QVERIFY(waitLoaded(browser, address()));
        browser.executeScript(QStringLiteral("document.getElementById('new-window').click()"));
        QTRY_COMPARE_WITH_TIMEOUT(browser.currentUrl(), address(QStringLiteral("/two")), 5000);
    }

    void destroyDuringAsyncCreation() {
        for (int i = 0; i < 8; ++i) {
            auto* browser = new BrowserWidget;
            prepare(*browser);
            browser->navigate(address(QStringLiteral("/slow")));
            browser->show();
            if (i % 2) QCoreApplication::processEvents();
            delete browser;
        }
        QTest::qWait(400);
        BrowserWidget survivor;
        prepare(survivor);
        QVERIFY(waitLoaded(survivor, address()));
    }

    void destroyFromReadySignal() {
        auto* browser = new BrowserWidget;
        prepare(*browser);
        QPointer<BrowserWidget> pointer(browser);
        connect(browser, &BrowserWidget::readyChanged, this, [browser] { delete browser; });
        browser->navigate(address());
        browser->show();
        QTRY_VERIFY_WITH_TIMEOUT(pointer.isNull(), 15000);
        QTest::qWait(100);
    }

    void destroyFromNavigationSignal() {
        auto* browser = new BrowserWidget;
        prepare(*browser);
        QPointer<BrowserWidget> pointer(browser);
        connect(browser, &BrowserWidget::loadingChanged, this, [browser](bool loading) { if (loading) delete browser; });
        browser->navigate(address());
        browser->show();
        QTRY_VERIFY_WITH_TIMEOUT(pointer.isNull(), 15000);
        QTest::qWait(100);
    }

    void destroyWithOutstandingOperations() {
        auto* browser = new BrowserWidget;
        prepare(*browser);
        QVERIFY(waitLoaded(*browser, address()));
        browser->executeScript(QStringLiteral("(() => { const until=Date.now()+250; while(Date.now()<until) {} return 1; })()"));
        browser->requestScreenshot();
        browser->callDevTools(QStringLiteral("Runtime.evaluate"), QStringLiteral("{\"expression\":\"7\"}"));
        QPointer<BrowserWidget> pointer(browser);
        delete browser;
        QVERIFY(pointer.isNull());
        QTest::qWait(600);
        BrowserWidget survivor;
        prepare(survivor);
        QVERIFY(waitLoaded(survivor, address()));
    }
};

QTEST_MAIN(BrowserTest)
#include "test_browser.moc"
