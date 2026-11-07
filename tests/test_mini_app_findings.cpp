#include "apps/MiniAppHost.h"
#include "browser/browserwidget.h"
#include "sandbox/SandboxHost.h"
#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "browser/BrowserToolBridge.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
using namespace ade;
namespace {
bool write(const QString& path,const QByteArray& bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))return false;
    QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
}
bool capture(MiniAppPage* page,const QString& name) {
    const auto directory=qEnvironmentVariable("ADE_MINIAPP_RENDER_DIR");
    if(directory.isEmpty())return true;
    QSignalSpy screenshots(page->browser(),&BrowserWidget::screenshotReady);
    page->browser()->requestScreenshot();
    QElapsedTimer elapsed;elapsed.start();while(screenshots.isEmpty()&&elapsed.elapsed()<4000)QTest::qWait(20);
    return !screenshots.isEmpty()&&write(QDir(directory).filePath(name+".png"),screenshots.last()[0].toByteArray());
}
struct Result { bool arrived=false,error=true;QString value; };
Result run(SandboxHost& sandbox,const QString& script) {
    static int sequence=0;const auto id=QStringLiteral("fixture-%1").arg(++sequence);
    QSignalSpy done(&sandbox,&SandboxHost::completed);
    sandbox.execute(id,"executeSandboxJs",{{"explanation","Test mini-app routing"},{"script",script}});
    QElapsedTimer elapsed;elapsed.start();
    while(elapsed.elapsed()<10000){
        for(const auto& row:done)if(row[0].toString()==id)
            return {true,row[2].toBool(),QJsonDocument::fromJson(row[1].toString().toUtf8()).object().value("result").toString()};
        QTest::qWait(10);
    }
    return {};
}
struct Runtime {
    SandboxHost sandbox;
    MiniAppHost apps;
    QHash<QString,QJsonObject> subscriptions;
    QList<QPointer<MiniAppPage>> pages;
    Runtime(const QString& data):apps("fixture-agent",data) {
        sandbox.setAgentId("fixture-agent");
        QObject::connect(&sandbox,&SandboxHost::appRequest,&apps,[this](const QString& id,const QString& owner,const QString& op,const QJsonObject& args){
            if(owner=="fixture-agent")apps.execute(id,op,args);
        });
        QObject::connect(&apps,&MiniAppHost::completed,&sandbox,&SandboxHost::completeAppRequest);
        QObject::connect(&sandbox,&SandboxHost::eventSubscriptionRequested,&sandbox,[this](const QString& id,const QString& owner,const QString& kind,const QJsonObject& args){
            if(owner=="fixture-agent"&&kind=="appMessage")subscriptions[id]=args;
        });
        QObject::connect(&sandbox,&SandboxHost::eventUnsubscriptionRequested,&sandbox,[this](const QString& id){subscriptions.remove(id);});
        QObject::connect(&apps,&MiniAppHost::messageReceived,&sandbox,[this](const QString& app,const QString& plugin,const QJsonValue& value){
            const auto copy=subscriptions;
            for(auto it=copy.cbegin();it!=copy.cend();++it)
                if(it.value().value("appId")==app&&it.value().value("pluginId").toString()==plugin)sandbox.deliverEvent(it.key(),value);
        });
        QObject::connect(&apps,&MiniAppHost::pageOpened,&apps,[this](const QString& id,MiniAppPage* page){
            pages.append(page);page->setAttribute(Qt::WA_DontShowOnScreen);page->resize(640,360);
            apps.bindTab(id,QStringLiteral("app-%1").arg(pages.size()));page->show();
        });
    }
};
}
class MiniAppFindingsTests final:public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> temp;
private slots:
    void init(){temp=std::make_unique<QTemporaryDir>();QVERIFY(temp->isValid());}
    void cleanup(){
        auto path=temp->path();temp->setAutoRemove(false);temp.reset();
        QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<5000&&!QDir(path).removeRecursively())QTest::qWait(50);
        QVERIFY(!QFile::exists(path));
    }
    void inactiveOpenAppStillReachesItsQuickJsListener() {
        Runtime runtime(temp->path());
        const QByteArray html="<body>Fixture<script>addEventListener('message',e=>{if(e.data.action==='click')parent.postMessage({clicked:'a',value:e.data.value},'*');});</script>";
        QVERIFY(write(runtime.apps.agentAppsDirectory()+"/a/index.html",html));
        QVERIFY(write(runtime.apps.agentAppsDirectory()+"/b/index.html","<body>Second app</body>"));
        auto setup=run(runtime.sandbox,"globalThis.appEvents=[];API.onMessage('a',message=>appEvents.push(message));await API.openApp('a');await API.openApp('b');return 'ready';");
        QVERIFY(setup.arrived&&!setup.error);QCOMPARE(setup.value,"ready");QCOMPARE(runtime.pages.size(),2);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.pages[0]->isReady()&&runtime.pages[1]->isReady(),15000);
        QCOMPARE(runtime.apps.activePage(),runtime.pages[1].data());
        QSignalSpy completed(&runtime.apps,&MiniAppHost::completed);
        runtime.apps.execute("inactive-send","send",{{"appId","a"},{"data",1}});
        QCOMPARE(completed.size(),1);QVERIFY(!completed[0][2].toString().isEmpty());
        // Drive the existing inactive iframe; this is not an API.sendMessage permission grant.
        QSignalSpy received(&runtime.apps,&MiniAppHost::messageReceived);
        QVERIFY(runtime.pages[0]->postMessage(QJsonObject{{"action","click"},{"value",17}}));
        QTRY_COMPARE_WITH_TIMEOUT(received.size(),1,2000);
        QCOMPARE(received[0][0].toString(),"a");QCOMPARE(received[0][1].toString(),QString());
        QTRY_COMPARE_WITH_TIMEOUT(run(runtime.sandbox,"return appEvents.length;").value,"1",2000);
        const auto result=run(runtime.sandbox,"return appEvents[0];");QVERIFY(!result.error);
        QCOMPARE(QJsonDocument::fromJson(result.value.toUtf8()).object(),(QJsonObject{{"clicked","a"},{"value",17}}));
    }
    void parsingMessageArrivesBeforeLoadAndOnReload() {
        const auto app=temp->filePath("app");
        QVERIFY(write(app+"/index.html","<body><script>parent.postMessage({phase:'parsing'},'*');const until=performance.now()+180;while(performance.now()<until){};</script></body>"));
        MiniAppPage page(QUrl("app://agents/fixture-agent/a"),app,temp->filePath("profile"));
        QVERIFY(page.configured());int beforeReady=0;
        connect(&page,&MiniAppPage::messageReceived,&page,[&](const QJsonValue& value){if(value.toObject().value("phase")=="parsing"&&!page.isReady())++beforeReady;});
        QSignalSpy messages(&page,&MiniAppPage::messageReceived);
        page.setAttribute(Qt::WA_DontShowOnScreen);page.resize(640,360);page.show();
        QTRY_VERIFY_WITH_TIMEOUT(page.isReady(),15000);
        QCOMPARE(messages.size(),1);QCOMPARE(beforeReady,1);
        page.reloadApp();QTRY_VERIFY_WITH_TIMEOUT(page.isReady(),15000);
        QCOMPARE(messages.size(),2);QCOMPARE(beforeReady,2);
    }
    void contextResetPreservesBoundPageAndRevokesListener_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("idle-reset")<<QString("reset");
        QTest::newRow("stop-running-script")<<QString("cancel");
        QTest::newRow("workspace-change")<<QString("workspace");
    }
    void contextResetPreservesBoundPageAndRevokesListener() {
        QFETCH(QString,operation);
        AgentEngine engine;BrowserToolBridge bridge;
        AgentToolHost host(&engine,&bridge,"reset-owner",temp->path());
        QList<QPointer<MiniAppPage>> pages;QString originalInstance;
        connect(host.apps(),&MiniAppHost::pageOpened,&host,[&](const QString& id,MiniAppPage* page){
            if(originalInstance.isEmpty())originalInstance=id;
            pages.append(page);page->setAttribute(Qt::WA_DontShowOnScreen);page->resize(640,360);
            host.apps()->bindTab(id,bridge.registerTab(page->browser()));page->show();
        });
        QVERIFY(write(host.apps()->agentAppsDirectory()+"/demo/index.html","<body style='font-family:system-ui;background:#f2f5fa;color:#182635;padding:24px'><h1>Persistent app</h1><p>Owned preview fixture</p><script>addEventListener('message',e=>{if(e.data.ping)parent.postMessage({pong:e.data.ping},'*');});</script>"));
        auto result=run(*host.sandbox(),"globalThis.oldState=[];API.onMessage('demo',m=>oldState.push(m));await API.openApp('demo');return 'opened';");
        QVERIFY(result.arrived&&!result.error);QCOMPARE(pages.size(),1);
        const QPointer<MiniAppPage> original=pages[0];
        QTRY_VERIFY_WITH_TIMEOUT(original->isReady(),15000);
        QVERIFY(original->isVisible());
        if(operation=="reset")QVERIFY(capture(original,"bound-before-context-reset"));
        QSignalSpy closed(host.apps(),&MiniAppHost::pageCloseRequested);
        QSignalSpy scriptDone(host.sandbox(),&SandboxHost::completed);
        if(operation=="reset")host.sandbox()->resetAgent(host.agentId());
        else if(operation=="cancel"){
            QSignalSpy output(host.sandbox(),&SandboxHost::consoleMessage);
            host.sandbox()->execute("held","executeSandboxJs",{{"explanation","Wait until canceled"},{"script","console.log('held-started');await new Promise(()=>{});"}});
            QTRY_VERIFY_WITH_TIMEOUT(!output.isEmpty(),3000);host.sandbox()->cancel("held");
        }else{
            QVERIFY(QDir().mkpath(temp->filePath("workspace")));QString error;
            QVERIFY2(host.setWorkspace(temp->filePath("workspace"),&error),qPrintable(error));
        }
        QCOMPARE(closed.size(),0);QVERIFY(original);QVERIFY(original->isVisible());
        QVERIFY(host.apps()->ownsPage(originalInstance,original));QVERIFY(!host.apps()->activePage());
        if(operation=="reset")QVERIFY(capture(original,"bound-after-context-reset"));
        if(operation=="cancel"){
            const auto settled=[&]{for(const auto& row:scriptDone)if(row[0].toString()=="held")return true;return false;};
            QTRY_VERIFY_WITH_TIMEOUT(settled(),3000);
        }
        result=run(*host.sandbox(),"return typeof oldState;");QVERIFY(!result.error);QCOMPARE(result.value,"undefined");
        result=run(*host.sandbox(),"globalThis.newState=[];API.onMessage('demo',m=>newState.push(m));return 'subscribed';");QVERIFY(!result.error);
        QVERIFY(original->postMessage(QJsonObject{{"ping","new-context"}}));
        QTRY_COMPARE_WITH_TIMEOUT(run(*host.sandbox(),"return newState.length;").value,"1",3000);
        result=run(*host.sandbox(),"return await API.sendMessage('demo',{ping:'denied'});");QVERIFY(result.error);
        result=run(*host.sandbox(),"return await API.openApp('demo');");QVERIFY(!result.error);QCOMPARE(pages.size(),2);
        QVERIFY(original);QVERIFY(host.apps()->activePage()!=original);
    }
    void parsingMessageReachesActualQuickJsListener() {
        Runtime runtime(temp->path());
        QVERIFY(write(runtime.apps.agentAppsDirectory()+"/early/index.html","<script>parent.postMessage({phase:'parsing'},'*');const until=performance.now()+180;while(performance.now()<until){};</script>"));
        auto result=run(runtime.sandbox,"globalThis.early=[];API.onMessage('early',m=>early.push(m));await API.openApp('early');return 'opened';");
        QVERIFY(result.arrived&&!result.error);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.apps.activePage()->isReady(),15000);
        QTRY_COMPARE_WITH_TIMEOUT(run(runtime.sandbox,"return early.length;").value,"1",3000);
    }
    void inboundIdentityNonceAndSizeRemainRequired() {
        const auto app=temp->filePath("app");QVERIFY(write(app+"/index.html","<body>Fixture</body>"));
        MiniAppPage page(QUrl("app://agents/owner/app"),app,temp->filePath("profile"));
        QSignalSpy messages(&page,&MiniAppPage::messageReceived);
        page.setAttribute(Qt::WA_DontShowOnScreen);page.resize(640,360);page.show();
        QTRY_VERIFY_WITH_TIMEOUT(page.isReady(),15000);
        const auto source=page.browser()->currentUrl();
        const auto parts=source.path().split('/',Qt::SkipEmptyParts);QVERIFY(!parts.isEmpty());
        const auto nonce=parts.first();
        QJsonObject envelope{{"channel","ade-mini-app"},{"nonce",nonce},{"kind","message"},{"data",QJsonObject{{"value",1}}}};
        emit page.browser()->webMessageReceived(QUrl("app://agents/other/app/index.html"),envelope);
        auto wrong=envelope;wrong["nonce"]="wrong";emit page.browser()->webMessageReceived(source,wrong);
        wrong=envelope;wrong["channel"]="wrong";emit page.browser()->webMessageReceived(source,wrong);
        wrong=envelope;wrong["data"]=QString(128*1024,'x');emit page.browser()->webMessageReceived(source,wrong);
        QCOMPARE(messages.size(),0);
        emit page.browser()->webMessageReceived(source,envelope);QCOMPARE(messages.size(),1);
        page.reloadApp();emit page.browser()->webMessageReceived(source,envelope);QCOMPARE(messages.size(),1);
    }
    void resetDiscardsQueuedMessagesAndSettlesUnboundOpen() {
        MiniAppHost host("owner",temp->path());
        QVERIFY(write(host.agentAppsDirectory()+"/demo/index.html","<script>addEventListener('message',e=>parent.postMessage(e.data,'*'));</script>"));
        QPointer<MiniAppPage> page;QString instance;
        connect(&host,&MiniAppHost::pageOpened,&host,[&](const QString& id,MiniAppPage* p){page=p;instance=id;host.bindTab(id,"bound");});
        host.execute("open","open",{{"appId","demo"}});QVERIFY(page);QVERIFY(!page->isReady());
        QSignalSpy received(&host,&MiniAppHost::messageReceived);
        QVERIFY(page->postMessage(QJsonObject{{"oldContext",true}}));host.resetContext();
        page->setAttribute(Qt::WA_DontShowOnScreen);page->resize(640,360);page->show();
        QTRY_VERIFY_WITH_TIMEOUT(page->isReady(),15000);
        QVERIFY(page->postMessage(QJsonObject{{"currentContext",true}}));
        QTRY_COMPARE_WITH_TIMEOUT(received.size(),1,3000);
        QCOMPARE(received[0][2].toJsonValue().toObject(),(QJsonObject{{"currentContext",true}}));
        host.closeTab(instance);QVERIFY(!host.ownsPage(instance,page));
        if(page)emit page->messageReceived(QJsonObject{{"closed",true}});
        QCOMPARE(received.size(),1);
        MiniAppHost unbound("unbound",temp->path());
        QVERIFY(write(unbound.agentAppsDirectory()+"/demo/index.html","<body>Unbound</body>"));
        QSignalSpy completed(&unbound,&MiniAppHost::completed);
        unbound.execute("pending","open",{{"appId","demo"}});QPointer<MiniAppPage> orphan=unbound.activePage();QVERIFY(orphan);
        unbound.resetContext();QCOMPARE(completed.size(),1);QCOMPARE(completed[0][0].toString(),"pending");QVERIFY(!completed[0][2].toString().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!orphan);
    }
    void resetReentryPreservesReplacementAndAllowsOwnerDeletion() {
        auto* host=new MiniAppHost("owner",temp->path());QPointer<MiniAppHost> alive(host);
        QVERIFY(write(host->agentAppsDirectory()+"/demo/index.html","<body>Fixture</body>"));
        host->execute("old","open",{{"appId","demo"}});
        QPointer<MiniAppPage> replacement;
        connect(host,&MiniAppHost::completed,this,[&](const QString& id){if(id=="old"){
            host->execute("new","open",{{"appId","demo"}});replacement=host->activePage();host->bindTab(host->activeInstanceId(),"replacement");
        }});
        host->resetContext();QVERIFY(replacement);QCOMPARE(host->activePage(),replacement.data());
        connect(host,&MiniAppHost::activeAppChanged,this,[&]{delete host;});
        host->resetContext();QVERIFY(!alive);QVERIFY(!replacement);
    }
    void pluginRevocationStillClosesInactivePages() {
        const auto plugin=temp->filePath("plugin");
        QVERIFY(write(plugin+"/apps/a/index.html","<body>A</body>"));
        QVERIFY(write(plugin+"/apps/b/index.html","<body>B</body>"));
        MiniAppHost host("owner",temp->path());host.setPluginRoots({{"fixture",plugin}});
        QList<QPointer<MiniAppPage>> pages;
        connect(&host,&MiniAppHost::pageOpened,&host,[&](const QString& id,MiniAppPage* p){pages.append(p);host.bindTab(id,QString::number(pages.size()));});
        host.execute("a","open",{{"appId","a"},{"pluginId","fixture"}});
        host.execute("b","open",{{"appId","b"},{"pluginId","fixture"}});
        QCOMPARE(pages.size(),2);host.resetContext();QVERIFY(pages[0]);QVERIFY(pages[1]);
        QSignalSpy messages(&host,&MiniAppHost::messageReceived),closed(&host,&MiniAppHost::pageCloseRequested);
        host.setPluginRoots({});QCOMPARE(closed.size(),2);
        for(const auto& page:pages)if(page)emit page->messageReceived(QJsonObject{{"revoked",true}});
        QCOMPARE(messages.size(),0);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(!pages[0]);QVERIFY(!pages[1]);
    }
    void permanentFileContextRemovalStillClosesBoundPage() {
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"removed-owner",temp->path());
        QVERIFY(write(host.apps()->agentAppsDirectory()+"/demo/index.html","<body>Owned app</body>"));
        QPointer<MiniAppPage> page;
        connect(host.apps(),&MiniAppHost::pageOpened,&host,[&](const QString& id,MiniAppPage* p){page=p;host.apps()->bindTab(id,"bound");});
        auto result=run(*host.sandbox(),"return await API.openApp('demo');");QVERIFY(result.arrived&&!result.error);QVERIFY(page);
        QSignalSpy closed(host.apps(),&MiniAppHost::pageCloseRequested);QString error;
        QVERIFY2(host.removeFileContext(&error),qPrintable(error));QCOMPARE(closed.size(),1);
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!page);
    }
};
QTEST_MAIN(MiniAppFindingsTests)
#include "test_mini_app_findings.moc"
