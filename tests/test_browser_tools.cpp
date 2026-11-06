#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <functional>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using ade::BrowserWidget;
using ade::BrowserToolBridge;

class BrowserToolsTest final : public QObject {
    Q_OBJECT
    QTcpServer server;
    QTemporaryDir profiles;
    quint64 sequence = 0;
    struct Result { bool received = false; bool error = true; QString output; };
    QUrl url(const QString& path = QStringLiteral("/one")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
    }
    void prepare(BrowserWidget& browser, const QString& profile = QStringLiteral("shared")) {
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(800, 600);
        browser.setProfileDirectory(profiles.path() + QLatin1Char('/') + profile);
    }
    static bool waitFor(const std::function<bool()>& condition, int timeout = 10000) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < timeout) QTest::qWait(10);
        return condition();
    }
    bool load(BrowserWidget& browser, const QUrl& address) {
        QSignalSpy loading(&browser, &BrowserWidget::loadingChanged);
        browser.navigate(address);
        browser.show();
        return waitFor([&] {
            for (const auto& row : loading) if (!row[0].toBool() && browser.currentUrl() == address) return true;
            return false;
        }, 15000);
    }
    Result request(BrowserToolBridge& bridge, const QString& name, const QJsonObject& args = {}) {
        const QString id = QStringLiteral("request-%1").arg(++sequence);
        QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
        bridge.execute(id, name, args);
        Result result;
        waitFor([&] {
            for (const auto& row : completed) if (row[0].toString() == id) {
                result = {true, row[2].toBool(), row[1].toString()};
                return true;
            }
            return false;
        });
        return result;
    }
    Result cdp(BrowserToolBridge& bridge, const QString& tabId, const QString& method, const QJsonObject& params = {}) {
        return request(bridge, QStringLiteral("sendCDP"), {{"tabId", tabId}, {"method", method}, {"params", params}});
    }
    bool focusForFullscreen(BrowserWidget& browser,BrowserToolBridge& bridge,const QString& tab) {
        // Fullscreen requires the real native foreground, not just CDP's
        // document-focus emulation. Other desktop tools may have taken focus.
        browser.raise();browser.activateWindow();browser.setFocus(Qt::OtherFocusReason);
#ifdef Q_OS_WIN
        const auto hwnd=reinterpret_cast<HWND>(browser.winId());
        const auto activate=[&]{
            const auto foregroundThread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
            const auto currentThread=GetCurrentThreadId();
            const bool attached=foregroundThread&&foregroundThread!=currentThread&&AttachThreadInput(currentThread,foregroundThread,TRUE);
            ShowWindow(hwnd,SW_SHOWNORMAL);BringWindowToTop(hwnd);SetForegroundWindow(hwnd);SetActiveWindow(hwnd);SetFocus(hwnd);
            if(attached)AttachThreadInput(currentThread,foregroundThread,FALSE);
        };
        activate();
#endif
        // This predicate runs a CDP request; do not use waitFor(), which checks
        // its predicate a second time after success and can observe a new state.
        QElapsedTimer deadline;deadline.start();
        while(deadline.elapsed()<3000){
#ifdef Q_OS_WIN
            if(GetForegroundWindow()!=hwnd)activate();
#endif
            const auto state=cdp(bridge,tab,"Runtime.evaluate",{{"expression","document.hasFocus() && document.visibilityState==='visible' && document.fullscreenEnabled"},{"returnByValue",true}});
            const bool focused=state.received&&!state.error&&object(state)["result"].toObject()["value"].toBool();
#ifdef Q_OS_WIN
            if(focused&&GetForegroundWindow()==hwnd)return true;
#else
            if(focused)return true;
#endif
            QTest::qWait(10);
        }
        return false;
    }
    static QJsonObject object(const Result& result) { return QJsonDocument::fromJson(result.output.toUtf8()).object(); }
    static QJsonObject consoleResult(const Result& result) {
        return QJsonDocument::fromJson(object(result).value("result").toString().toUtf8()).object();
    }

private slots:
    void initTestCase() {
        QVERIFY(profiles.isValid());
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    const auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    const auto target=request.split(' ').value(1);
                    if(target.startsWith("/download")) {
                        const bool slow=target.startsWith("/download-slow");
                        const QByteArray payload(slow?512*1024:1024,slow?'s':'d');
                        qsizetype offset=0;
                        for(const auto& line:request.split('\n'))if(line.toLower().startsWith("range: bytes="))offset=line.mid(13).split('-').first().trimmed().toLongLong();
                        offset=qBound<qsizetype>(0,offset,payload.size());
                        QByteArray header=offset?QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ")+QByteArray::number(offset)+'-'+QByteArray::number(payload.size()-1)+'/'+QByteArray::number(payload.size())+"\r\n":QByteArray("HTTP/1.1 200 OK\r\n");
                        header+="Content-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=fixture.bin\r\nAccept-Ranges: bytes\r\nConnection: close\r\nContent-Length: "+QByteArray::number(payload.size()-offset)+"\r\n\r\n";
                        socket->write(header);
                        if(!slow){socket->write(payload.mid(offset));socket->disconnectFromHost();return;}
                        socket->setProperty("offset",offset);
                        auto* chunks=new QTimer(socket);chunks->setInterval(25);
                        connect(chunks,&QTimer::timeout,socket,[socket,chunks,payload]{
                            if(socket->state()!=QAbstractSocket::ConnectedState){chunks->stop();return;}
                            const auto offset=socket->property("offset").toLongLong();
                            const auto chunk=payload.mid(offset,16*1024);socket->write(chunk);socket->setProperty("offset",offset+chunk.size());
                            if(offset+chunk.size()>=payload.size()){chunks->stop();socket->disconnectFromHost();}
                        });chunks->start();return;
                    }
                    const QByteArray name = request.split(' ').value(1).startsWith("/two") ? "two" : "one";
                    const QByteArray body = "<!doctype html><html><head><title>ADE " + name
                        + "</title></head><body><h1 id='value'>" + name
                        + "</h1><script>console.log('initial " + name + "')</script></body></html>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    void definitionsAndStableTabs() {
        BrowserToolBridge bridge;
        BrowserWidget first, second;
        const QString firstId = bridge.registerTab(&first);
        const QString secondId = bridge.registerTab(&second);
        QVERIFY(!firstId.isEmpty() && !secondId.isEmpty() && firstId != secondId);
        QCOMPARE(bridge.registerTab(&first), firstId);
        QVERIFY(bridge.setActiveTab(secondId));
        QVERIFY(!bridge.setActiveTab(QStringLiteral("unknown")));
        const auto snapshot = request(bridge, QStringLiteral("getBrowserTabs"));
        QVERIFY(snapshot.received && !snapshot.error);
        QCOMPARE(object(snapshot).value("activeTabId").toString(), secondId);
        QCOMPARE(object(snapshot).value("tabs").toArray().size(), 2);
        bridge.unregisterTab(firstId);
        QVERIFY(bridge.registerTab(&first, firstId).isEmpty());
        QVERIFY(bridge.registerTab(&first) != firstId);
        QCOMPARE(bridge.definitions().size(), 3);
        QVERIFY(bridge.handles(QStringLiteral("sendCDP")));
        QVERIFY(!bridge.handles(QStringLiteral("executeSandboxJs")));
    }

    void domNavigationHistoryAndScreenshot() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const QString tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        auto evaluated = cdp(bridge, tab, "Runtime.evaluate", {{"expression", "document.querySelector('#value').textContent"}, {"returnByValue", true}});
        QVERIFY(evaluated.received && !evaluated.error);
        QCOMPARE(object(evaluated).value("result").toObject().value("value").toString(), QStringLiteral("one"));
        auto dom = cdp(bridge, tab, "DOM.getDocument", {{"depth", 1}});
        QVERIFY(dom.received && !dom.error);
        QVERIFY(object(dom).value("root").toObject().value("nodeId").toInt() > 0);
        QSignalSpy loading(&browser, &BrowserWidget::loadingChanged);
        auto navigation = cdp(bridge, tab, "Page.navigate", {{"url", url("/two").toString()}});
        QVERIFY(navigation.received && !navigation.error);
        QVERIFY(waitFor([&] { return browser.currentUrl() == url("/two") && !loading.isEmpty() && !loading.last()[0].toBool(); }));
        auto history = cdp(bridge, tab, "Page.getNavigationHistory");
        const auto entries = object(history).value("entries").toArray();
        QVERIFY(entries.size() >= 2);
        int firstEntry = -1;
        for (const auto& entry : entries) if (entry.toObject().value("url").toString() == url().toString()) firstEntry = entry.toObject().value("id").toInt(-1);
        QVERIFY(firstEntry >= 0);
        auto back = cdp(bridge, tab, "Page.navigateToHistoryEntry", {{"entryId", firstEntry}});
        QVERIFY(back.received && !back.error);
        QVERIFY(waitFor([&] { return browser.currentUrl() == url(); }));
        auto screenshot = cdp(bridge, tab, "Page.captureScreenshot", {{"format", "png"}});
        QVERIFY(screenshot.received && !screenshot.error);
        QVERIFY(!QImage::fromData(QByteArray::fromBase64(object(screenshot).value("data").toString().toLatin1()), "PNG").isNull());
    }

    void outOfOrderResponsesAreCorrelated() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const auto tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
        bridge.execute("slow", "sendCDP", {{"tabId", tab}, {"method", "Runtime.evaluate"}, {"params", QJsonObject{
            {"expression", "new Promise(resolve=>setTimeout(()=>resolve('slow'),180))"}, {"awaitPromise", true}, {"returnByValue", true}}}});
        bridge.execute("fast", "sendCDP", {{"tabId", tab}, {"method", "Runtime.evaluate"}, {"params", QJsonObject{
            {"expression", "'fast'"}, {"returnByValue", true}}}});
        browser.callDevTools("Runtime.evaluate", "{\"expression\":\"'unrelated-ui-call'\"}");
        QVERIFY(waitFor([&] { return completed.size() == 2; }));
        QCOMPARE(completed.first()[0].toString(), QStringLiteral("fast"));
        QCOMPARE(completed.last()[0].toString(), QStringLiteral("slow"));
        for (const auto& row : completed) {
            QVERIFY(!row[2].toBool());
            QCOMPARE(QJsonDocument::fromJson(row[1].toString().toUtf8()).object().value("result").toObject().value("value").toString(), row[0].toString());
        }
    }

    void consoleCaptureFiltersDelayAndNavigation() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const auto tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        auto initial = request(bridge, "readConsoleLogs", {{"id", tab}, {"filter", "initial one"}, {"delayMs", 50}});
        QVERIFY(initial.received && !initial.error);
        QVERIFY(consoleResult(initial).value("logsReturned").toInt() >= 1);
        auto inject = cdp(bridge, tab, "Runtime.evaluate", {{"expression",
            "console.log('match-one');console.warn('MATCH-two');console.error('other-three');setTimeout(()=>console.log('match-late'),60);'done'"}});
        QVERIFY(inject.received && !inject.error);
        auto logs = request(bridge, "readConsoleLogs", {{"id", tab}, {"filter", "MATCH"}, {"levels", QJsonArray{"log"}}, {"limit", 1}, {"delayMs", 150}});
        QVERIFY(logs.received && !logs.error);
        QCOMPARE(consoleResult(logs).value("logsReturned").toInt(), 1);
        QCOMPARE(consoleResult(logs).value("logs").toArray().first().toObject().value("message").toString(), QStringLiteral("match-late"));
        QVERIFY(load(browser, url("/two")));
        auto cleared = request(bridge, "readConsoleLogs", {{"id", tab}, {"filter", "match"}});
        QCOMPARE(consoleResult(cleared).value("logsReturned").toInt(), 0);
    }

    void timeoutIgnoresLateCompletion() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const auto tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        bridge.setRequestTimeoutMs(80);
        QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
        bridge.execute("timeout", "sendCDP", {{"tabId", tab}, {"method", "Runtime.evaluate"}, {"params", QJsonObject{
            {"expression", "new Promise(resolve=>setTimeout(()=>resolve('late'),300))"}, {"awaitPromise", true}}}});
        QVERIFY(waitFor([&] { return completed.size() == 1; }));
        QVERIFY(completed.first()[2].toBool());
        QVERIFY(completed.first()[1].toString().contains("timed out"));
        bridge.setRequestTimeoutMs(5000);
        bridge.execute("timeout", "getBrowserTabs", {});
        QVERIFY(waitFor([&] { return completed.size() == 2; }));
        QVERIFY(!completed.last()[2].toBool());
        QTest::qWait(400);
        QCOMPARE(completed.size(), 2);
    }

    void cancellationAndDuplicateIdsSettleOnce() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const auto tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
        bridge.execute("cancel", "readConsoleLogs", {{"id", tab}, {"delayMs", 200}});
        QTest::qWait(20);
        bridge.cancel("cancel");
        QCOMPARE(completed.size(), 1);
        QVERIFY(completed.first()[2].toBool());
        bridge.execute("cancel", "getBrowserTabs", {});
        QVERIFY(waitFor([&] { return completed.size() == 2; }));
        QVERIFY(!completed.last()[2].toBool());
        bridge.execute("duplicate", "readConsoleLogs", {{"id", tab}, {"delayMs", 200}});
        bridge.execute("duplicate", "getBrowserTabs", {});
        QCOMPARE(completed.size(), 3);
        QVERIFY(completed.last()[2].toBool());
        QTest::qWait(300);
        QCOMPARE(completed.size(), 3);
    }

    void closingTabSettlesPendingRead() {
        BrowserToolBridge bridge;
        auto* browser = new BrowserWidget;
        prepare(*browser);
        const auto tab = bridge.registerTab(browser);
        QVERIFY(load(*browser, url()));
        QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
        bridge.execute("closing", "readConsoleLogs", {{"id", tab}, {"delayMs", 500}});
        QTest::qWait(20);
        delete browser;
        QCOMPARE(completed.size(), 1);
        QVERIFY(completed.first()[2].toBool());
        QTest::qWait(550);
        QCOMPARE(completed.size(), 1);
        const auto result = request(bridge, "readConsoleLogs", {{"id", tab}});
        QVERIFY(result.received && result.error);
    }

    void invalidArgumentsAndProtocolErrors() {
        BrowserToolBridge bridge;
        BrowserWidget browser;
        prepare(browser);
        const auto tab = bridge.registerTab(&browser);
        QVERIFY(load(browser, url()));
        for (const QJsonObject& args : {QJsonObject{{"id", tab}, {"limit", 1.5}}, QJsonObject{{"id", tab}, {"delayMs", 5001}},
            QJsonObject{{"id", tab}, {"levels", QJsonArray{"invented"}}}}) {
            const auto result = request(bridge, "readConsoleLogs", args);
            QVERIFY(result.received && result.error);
        }
        const auto missing = request(bridge, "sendCDP", {{"tabId", tab}});
        QVERIFY(missing.received && missing.error);
        const auto params = request(bridge, "sendCDP", {{"tabId", tab}, {"method", "Runtime.evaluate"}, {"params", QJsonArray{}}});
        QVERIFY(params.received && params.error);
        const auto protocol = cdp(bridge, tab, "ADE.nonexistentMethod");
        QVERIFY(protocol.received && protocol.error);
        const auto sandbox = request(bridge, "executeSandboxJs", {{"script", "return 1"}});
        QVERIFY(sandbox.received && sandbox.error);
    }

    void profilesDoNotSharePageStorage() {
        BrowserToolBridge bridge;
        BrowserWidget first, second;
        prepare(first, "isolated-one");
        prepare(second, "isolated-two");
        const auto firstTab = bridge.registerTab(&first);
        const auto secondTab = bridge.registerTab(&second);
        QVERIFY(load(first, url()));
        QVERIFY(load(second, url()));
        QVERIFY(!first.setProfileDirectory(profiles.path() + "/changed-too-late"));
        auto write = cdp(bridge, firstTab, "Runtime.evaluate", {{"expression", "localStorage.setItem('isolation','first');localStorage.getItem('isolation')"}, {"returnByValue", true}});
        QCOMPARE(object(write).value("result").toObject().value("value").toString(), QStringLiteral("first"));
        auto read = cdp(bridge, secondTab, "Runtime.evaluate", {{"expression", "localStorage.getItem('isolation')"}, {"returnByValue", true}});
        QVERIFY(read.received && !read.error);
        QVERIFY(object(read).value("result").toObject().value("value").isNull());
    }

    void bridgeCanBeDestroyedWithPendingProtocolRequest() {
        BrowserWidget browser;
        prepare(browser);
        auto* bridge = new BrowserToolBridge;
        const auto tab = bridge->registerTab(&browser);
        QVERIFY(load(browser, url()));
        bridge->execute("pending", "sendCDP", {{"tabId", tab}, {"method", "Runtime.evaluate"}, {"params", QJsonObject{
            {"expression", "new Promise(resolve=>setTimeout(()=>resolve(1),100))"}, {"awaitPromise", true}}}});
        QTest::qWait(20);
        delete bridge;
        QTest::qWait(150);
        BrowserToolBridge survivor;
        const auto newTab = survivor.registerTab(&browser);
        const auto result = cdp(survivor, newTab, "Runtime.evaluate", {{"expression", "42"}, {"returnByValue", true}});
        QVERIFY(result.received && !result.error);
    }

    void permissionDefaultsAndExactOriginExceptions() {
        BrowserWidget browser;
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "fullscreen"), "allow");
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "media"), "ask");
        QVERIFY(browser.setPermissionDefaults({{"camera","block"},{"location",1},{"notifications",0}}));
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "media"), "block");
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "geolocation"), "allow");
        QVERIFY(browser.setPermissionExceptions({{"media",QJsonObject{{"https://EXAMPLE.com:443/path",1}}}}));
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com/other"), "camera"), "allow");
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com:444"), "camera"), "block");
        QCOMPARE(browser.permissionSetting(QUrl("http://example.com"), "camera"), "block");
        QCOMPARE(browser.permissionSetting(QUrl("file:///C:/x.html"), "media"), "block");
        QVERIFY(!browser.setPermissionDefaults({{"camera","invented"}}));
        QVERIFY(!browser.setPermissionExceptions({{"media",QJsonObject{{"https://*.example.com",1}}}}));
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "camera"), "allow");
        QVERIFY(browser.setPermissionExceptions({{"media",QJsonObject{{"https://example.com",QJsonObject{{"setting",2},{"lastModified",1234}}}}}}));
        QCOMPARE(browser.permissionSetting(QUrl("https://example.com"), "camera"), "block");
    }

    void notificationDefaultsOverrideAndAskAreEnforced() {
        BrowserToolBridge bridge; BrowserWidget browser;
        prepare(browser,"permission-defaults");const auto tab=bridge.registerTab(&browser);
        QVERIFY(browser.setPermissionDefaults({{"notifications","block"}}));QVERIFY(load(browser,url()));
        QSignalSpy prompts(&browser,&BrowserWidget::permissionRequested);
        const QJsonObject params{{"expression","Notification.requestPermission()"},{"awaitPromise",true},{"returnByValue",true},{"userGesture",true}};
        const auto blocked=cdp(bridge,tab,"Runtime.evaluate",params);
        QVERIFY2(blocked.received&&!blocked.error,qPrintable(blocked.output));
        QCOMPARE(object(blocked).value("result").toObject().value("value").toString(),"denied");QCOMPARE(prompts.count(),0);
        const QString origin=QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        QVERIFY(browser.setPermissionExceptions({{"notifications",QJsonObject{{origin,"allow"}}}}));
        QVERIFY(load(browser,url("/two")));
        const auto allowed=cdp(bridge,tab,"Runtime.evaluate",params);
        QCOMPARE(object(allowed).value("result").toObject().value("value").toString(),"granted");QCOMPARE(prompts.count(),0);
        QVERIFY(browser.setPermissionDefaults({{"notifications","ask"}}));QVERIFY(browser.setPermissionExceptions({}));
        QVERIFY(load(browser,url("/one")));
        connect(&browser,&BrowserWidget::permissionRequested,&browser,[&browser](const QString& id,const QUrl&,const QString&,bool){browser.resolvePermission(id,false);});
        const auto asked=cdp(bridge,tab,"Runtime.evaluate",params);
        QCOMPARE(object(asked).value("result").toObject().value("value").toString(),"denied");
        QCOMPARE(prompts.count(),1);QCOMPARE(prompts.first().at(1).toUrl(),QUrl(origin));QCOMPARE(prompts.first().at(2).toString(),"notifications");
    }

    void permissionTimeoutNavigationAndLateDecision() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser,"permission-timeout");const auto tab=bridge.registerTab(&browser);
        browser.setPermissionTimeoutMs(80);QVERIFY(load(browser,url()));
        QSignalSpy prompts(&browser,&BrowserWidget::permissionRequested),decisions(&browser,&BrowserWidget::permissionRequestFinished);
        const QJsonObject params{{"expression","Notification.requestPermission()"},{"awaitPromise",true},{"returnByValue",true},{"userGesture",true}};
        const auto timed=cdp(bridge,tab,"Runtime.evaluate",params);
        QCOMPARE(object(timed).value("result").toObject().value("value").toString(),"denied");QCOMPARE(prompts.count(),1);QCOMPARE(decisions.count(),1);
        const QString expired=prompts.first().at(0).toString();browser.resolvePermission(expired,true);QCOMPARE(decisions.count(),1);
        browser.setPermissionTimeoutMs(3000);QVERIFY(load(browser,url("/two")));
        bridge.execute("navigation-permission","sendCDP",{{"tabId",tab},{"method","Runtime.evaluate"},{"params",params}});
        QVERIFY(waitFor([&]{return prompts.count()==2;}));
        const QString stale=prompts.last().at(0).toString();QVERIFY(load(browser,url("/one")));
        QCOMPARE(decisions.count(),2);QVERIFY(!decisions.last().at(1).toBool());
        browser.resolvePermission(stale,true);QCOMPARE(decisions.count(),2);
    }

    void changingPolicyResolvesOutstandingPermission() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser,"permission-update");const auto tab=bridge.registerTab(&browser);QVERIFY(load(browser,url()));
        QSignalSpy prompts(&browser,&BrowserWidget::permissionRequested),results(&bridge,&BrowserToolBridge::completed);
        bridge.execute("policy","sendCDP",{{"tabId",tab},{"method","Runtime.evaluate"},{"params",QJsonObject{{"expression","Notification.requestPermission()"},{"awaitPromise",true},{"returnByValue",true},{"userGesture",true}}}});
        QVERIFY(waitFor([&]{return prompts.count()==1;}));
        QVERIFY(browser.setPermissionDefaults({{"notifications","block"}}));
        QVERIFY(waitFor([&]{return results.count()==1;}));
        const auto result=QJsonDocument::fromJson(results.first().at(1).toString().toUtf8()).object();
        QCOMPARE(result.value("result").toObject().value("value").toString(),"denied");
    }

    void browserCanBeDeletedDuringPermissionPrompt() {
        BrowserToolBridge bridge;auto* browser=new BrowserWidget;prepare(*browser,"permission-destroy");const auto tab=bridge.registerTab(browser);QVERIFY(load(*browser,url()));
        QPointer<BrowserWidget> guard(browser);QSignalSpy results(&bridge,&BrowserToolBridge::completed);
        connect(browser,&BrowserWidget::permissionRequested,this,[browser]{delete browser;});
        bridge.execute("destroy-permission","sendCDP",{{"tabId",tab},{"method","Runtime.evaluate"},{"params",QJsonObject{{"expression","Notification.requestPermission()"},{"awaitPromise",true},{"returnByValue",true},{"userGesture",true}}}});
        QVERIFY(waitFor([&]{return guard.isNull();}));QCOMPARE(results.count(),1);QVERIFY(results.first().at(2).toBool());
    }

    void fullscreenPolicyGatesNativeWindowRequest() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser,"fullscreen-policy");const auto tab=bridge.registerTab(&browser);
        browser.setAttribute(Qt::WA_DontShowOnScreen,false);
#ifdef Q_OS_WIN
        const auto previousForeground=GetForegroundWindow();const auto fixtureWindow=reinterpret_cast<HWND>(browser.winId());
        const auto restoreForeground=qScopeGuard([previousForeground,fixtureWindow]{if(IsWindow(previousForeground)&&GetForegroundWindow()==fixtureWindow)SetForegroundWindow(previousForeground);});
#endif
        QVERIFY(browser.setPermissionDefaults({{"fullscreen","block"}}));QVERIFY(load(browser,url()));
        QSignalSpy prompts(&browser,&BrowserWidget::permissionRequested),fullscreen(&browser,&BrowserWidget::fullscreenChanged);
        const QJsonObject enter{{"expression","document.documentElement.requestFullscreen().then(()=>true)"},{"awaitPromise",true},{"returnByValue",true},{"userGesture",true}};
        QVERIFY2(focusForFullscreen(browser,bridge,tab),"Fullscreen fixture must own native foreground and document focus before requesting fullscreen.");
        auto result=cdp(bridge,tab,"Runtime.evaluate",enter);QVERIFY2(result.received&&!result.error,qPrintable(result.output));
        QVERIFY2(!object(result).contains("exceptionDetails"),qPrintable(result.output));
        QTest::qWait(100);
        auto state=cdp(bridge,tab,"Runtime.evaluate",{{"expression","document.fullscreenElement===null"},{"returnByValue",true}});
        QVERIFY(object(state).value("result").toObject().value("value").toBool());QCOMPARE(fullscreen.count(),0);QCOMPARE(prompts.count(),0);
        QVERIFY(browser.setPermissionDefaults({{"fullscreen","ask"}}));
        connect(&browser,&BrowserWidget::permissionRequested,&browser,[&browser](const QString& id,const QUrl&,const QString& key,bool){if(key=="fullscreen")browser.resolvePermission(id,false);});
        QVERIFY2(focusForFullscreen(browser,bridge,tab),"Fullscreen fixture lost focus before Ask/Deny.");
        result=cdp(bridge,tab,"Runtime.evaluate",enter);QVERIFY2(!object(result).contains("exceptionDetails"),qPrintable(result.output));
        QVERIFY(waitFor([&]{return prompts.count()==1;}));QTest::qWait(100);QCOMPARE(fullscreen.count(),0);
        QVERIFY(browser.setPermissionDefaults({{"fullscreen","allow"}}));
        QVERIFY2(focusForFullscreen(browser,bridge,tab),"Fullscreen fixture lost focus before Allow.");
        result=cdp(bridge,tab,"Runtime.evaluate",enter);QVERIFY2(!object(result).contains("exceptionDetails"),qPrintable(result.output));
        QVERIFY(waitFor([&]{return fullscreen.count()==1;}));QVERIFY(fullscreen.first().at(0).toBool());
        result=cdp(bridge,tab,"Runtime.evaluate",{{"expression","document.exitFullscreen()"},{"awaitPromise",true}});
        QVERIFY(result.received&&!result.error);QVERIFY(waitFor([&]{return fullscreen.count()==2;}));QVERIFY(!fullscreen.last().at(0).toBool());
    }

    void clearSelectedProfileDataPreservesUnselectedTypes() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser,"clear-profile");const auto tab=bridge.registerTab(&browser);QVERIFY(load(browser,url()));
        const auto seeded=cdp(bridge,tab,"Runtime.evaluate",{{"expression","(async()=>{document.cookie='ade_cookie=kept; path=/';localStorage.setItem('ade_storage','stored');await caches.open('ade_cache').then(c=>c.put('/cached',new Response('cached')));return true;})()"},{"awaitPromise",true},{"returnByValue",true}});
        QVERIFY2(seeded.received&&!seeded.error&&!object(seeded).contains("exceptionDetails"),qPrintable(seeded.output));
        QSignalSpy cleared(&browser,&BrowserWidget::browsingDataCleared);
        browser.clearBrowsingData("storage",{{"storage",true}});QVERIFY(waitFor([&]{return cleared.count()==1;}));QVERIFY2(cleared.first().at(1).toBool(),qPrintable(cleared.first().at(2).toString()));
        auto result=cdp(bridge,tab,"Runtime.evaluate",{{"expression","[localStorage.getItem('ade_storage'),document.cookie]"},{"returnByValue",true}});
        auto values=object(result).value("result").toObject().value("value").toArray();QCOMPARE(values.size(),2);QVERIFY(values[0].isNull());QVERIFY(values[1].toString().contains("ade_cookie=kept"));
        result=cdp(bridge,tab,"Runtime.evaluate",{{"expression","caches.has('ade_cache')"},{"awaitPromise",true},{"returnByValue",true}});QVERIFY(object(result).value("result").toObject().value("value").toBool());
        browser.clearBrowsingData("cookies-cache",{{"cookies",true},{"cacheStorage",true},{"cache",true}});QVERIFY(waitFor([&]{return cleared.count()==2;}));QVERIFY(cleared.last().at(1).toBool());
        result=cdp(bridge,tab,"Runtime.evaluate",{{"expression","(async()=>[document.cookie,await caches.has('ade_cache')])()"},{"awaitPromise",true},{"returnByValue",true}});
        values=object(result).value("result").toObject().value("value").toArray();QCOMPARE(values[0].toString(),QString());QVERIFY(!values[1].toBool());
        browser.clearBrowsingData("invalid",{{"timeRange",QJsonObject{{"start",1}}}});QCOMPARE(cleared.count(),3);QVERIFY(!cleared.last().at(1).toBool());
    }

    void downloadSelectionPreservesExistingFilesAndSavesBytes() {
        BrowserWidget browser;prepare(browser,"download-save");QVERIFY(load(browser,url()));QTemporaryDir output;QVERIFY(output.isValid());
        QFile existing(output.filePath("existing.bin"));QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("preserve");existing.close();
        QSignalSpy requests(&browser,&BrowserWidget::downloadRequested),finished(&browser,&BrowserWidget::downloadFinished),started(&browser,&BrowserWidget::downloadStarted);
        bool existingRejected=false,accepted=false;
        connect(&browser,&BrowserWidget::downloadRequested,&browser,[&](const QString& id,const QUrl&,const QString&,const QString&,qint64){
            existingRejected=!browser.acceptDownload(id,existing.fileName());
            accepted=browser.acceptDownload(id,output.filePath("saved.bin"));
        });
        browser.navigate(url("/download"));QVERIFY(waitFor([&]{return finished.count()==1;}));
        QVERIFY(existingRejected&&accepted);QCOMPARE(requests.count(),1);QCOMPARE(started.count(),1);QVERIFY2(finished.first().at(2).toBool(),qPrintable(finished.first().at(3).toString()));
        QCOMPARE(requests.first().at(2).toString(),"fixture.bin");QCOMPARE(requests.first().at(4).toLongLong(),1024);
        QFile saved(output.filePath("saved.bin"));QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),QByteArray(1024,'d'));
        QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("preserve"));
        browser.cancelDownload(requests.first().at(0).toString());QCOMPARE(finished.count(),1);
    }

    void downloadPauseResumeAndCancel() {
        BrowserWidget browser;prepare(browser,"download-control");QVERIFY(load(browser,url()));QTemporaryDir output;QVERIFY(output.isValid());
        QSignalSpy finished(&browser,&BrowserWidget::downloadFinished),states(&browser,&BrowserWidget::downloadStateChanged);
        QString id;bool paused=false,resumed=false;
        connect(&browser,&BrowserWidget::downloadRequested,&browser,[&](const QString& value,const QUrl&,const QString&,const QString&,qint64){id=value;browser.acceptDownload(id,output.filePath("slow.bin"));});
        connect(&browser,&BrowserWidget::downloadProgress,&browser,[&](const QString& value,qint64 received,qint64){
            if(!paused&&received>0){paused=true;QVERIFY(browser.pauseDownload(value));}
        });
        browser.navigate(url("/download-slow"));
        QVERIFY(waitFor([&]{for(const auto& row:states)if(row.at(1).toString()=="paused")return true;return false;}));
        QCOMPARE(finished.count(),0);QTest::qWait(80);resumed=browser.resumeDownload(id);QVERIFY(resumed);
        QVERIFY(waitFor([&]{return finished.count()==1;}));QVERIFY2(finished.first().at(2).toBool(),qPrintable(finished.first().at(3).toString()));
        QFile saved(output.filePath("slow.bin"));QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),QByteArray(512*1024,'s'));saved.close();
        // A second transfer is canceled after native bytes reach the file.
        disconnect(&browser,&BrowserWidget::downloadRequested,&browser,nullptr);
        disconnect(&browser,&BrowserWidget::downloadProgress,&browser,nullptr);
        connect(&browser,&BrowserWidget::downloadRequested,&browser,[&](const QString& value,const QUrl&,const QString&,const QString&,qint64){id=value;browser.acceptDownload(id,output.filePath("canceled.bin"));});
        connect(&browser,&BrowserWidget::downloadProgress,&browser,[&](const QString& value,qint64 received,qint64){if(received>0)browser.cancelDownload(value);});
        browser.navigate(url("/download-slow?second"));QVERIFY(waitFor([&]{return finished.count()==2;}));QVERIFY(!finished.last().at(2).toBool());
    }

    void downloadDecisionTimeoutAndDestruction() {
        BrowserWidget browser;prepare(browser,"download-timeout");browser.setDownloadDecisionTimeoutMs(80);QVERIFY(load(browser,url()));
        QSignalSpy requested(&browser,&BrowserWidget::downloadRequested),finished(&browser,&BrowserWidget::downloadFinished);
        browser.navigate(url("/download"));QVERIFY(waitFor([&]{return finished.count()==1;}));QCOMPARE(requested.count(),1);QVERIFY(!finished.first().at(2).toBool());
        QVERIFY(finished.first().at(3).toString().contains("timed out"));
        QVERIFY(!browser.acceptDownload(requested.first().at(0).toString(),profiles.filePath("late.bin")));
        auto* doomed=new BrowserWidget;prepare(*doomed,"download-destroy");QVERIFY(load(*doomed,url()));QPointer<BrowserWidget> guard(doomed);
        connect(doomed,&BrowserWidget::downloadRequested,this,[doomed]{delete doomed;});doomed->navigate(url("/download-slow"));QVERIFY(waitFor([&]{return guard.isNull();}));
    }

    void downloadCanBeDestroyedWhileDestinationIsAccepted() {
        auto* browser=new BrowserWidget;prepare(*browser,"download-accept-destroy");QVERIFY(load(*browser,url()));
        QTemporaryDir output;QVERIFY(output.isValid());QPointer<BrowserWidget> guard(browser);
        connect(browser,&BrowserWidget::downloadRequested,browser,[browser,&output](const QString& id,const QUrl&,const QString&,const QString&,qint64){browser->acceptDownload(id,output.filePath("partial.bin"));});
        connect(browser,&BrowserWidget::downloadStarted,this,[browser]{delete browser;});
        browser->navigate(url("/download-slow"));QVERIFY(waitFor([&]{return guard.isNull();}));
    }

    void dataClearRejectsUnreadyAndDuplicateRequests() {
        BrowserWidget browser;prepare(browser,"clear-lifecycle");QSignalSpy finished(&browser,&BrowserWidget::browsingDataCleared);
        browser.clearBrowsingData("not-ready",{{"cookies",true}});QCOMPARE(finished.count(),1);QVERIFY(!finished.last().at(1).toBool());
        QVERIFY(load(browser,url()));
        browser.clearBrowsingData("duplicate",{{"cache",true}});browser.clearBrowsingData("duplicate",{{"cookies",true}});
        QCOMPARE(finished.count(),2);QVERIFY(!finished.last().at(1).toBool());QVERIFY(finished.last().at(2).toString().contains("Duplicate"));
        QTest::qWait(150);QCOMPARE(finished.count(),2);
        auto* doomed=new BrowserWidget;prepare(*doomed,"clear-destroy");QVERIFY(load(*doomed,url()));QPointer<BrowserWidget> guard(doomed);
        connect(doomed,&BrowserWidget::browsingDataCleared,this,[doomed]{delete doomed;});
        doomed->clearBrowsingData("delete",{{"cookies",true},{"cache",true}});QVERIFY(waitFor([&]{return guard.isNull();}));
    }
};

QTEST_MAIN(BrowserToolsTest)
#include "test_browser_tools.moc"
