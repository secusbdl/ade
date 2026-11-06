#include "apps/MiniAppPage.h"
#include "apps/MiniAppHost.h"
#include "browser/browserwidget.h"
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDir>
#include <memory>
using namespace ade;
class MiniAppTests final:public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> temp;
    static bool write(const QString& path,const QByteArray& data){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(data)==data.size();}
private slots:
    void init(){temp=std::make_unique<QTemporaryDir>();QVERIFY(temp->isValid());}
    void cleanup(){const QString path=temp->path();temp->setAutoRemove(false);temp.reset();QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<5000&&!QDir(path).removeRecursively())QTest::qWait(50);QVERIFY(!QFile::exists(path));}
    void iframeContractRoundTripAndQueue() {
        QVERIFY(QDir().mkpath(temp->filePath("app")));
        QVERIFY(write(temp->filePath("app/index.html"),"<!doctype html><html><head><link rel='stylesheet' href='style.css'></head><body><div id='value'>initial</div><script>window.addEventListener('message',e=>{document.getElementById('value').textContent=JSON.stringify(e.data);parent.postMessage({ack:e.data},'*');});</script></body></html>"));
        QVERIFY(write(temp->filePath("app/style.css"),"body{color:rgb(1,2,3)}"));
        MiniAppPage page(QUrl("app://plugins/fixture/main"),temp->filePath("app"),temp->filePath("profile"));QVERIFY2(page.configured(),qPrintable(page.configurationError()));
        QSignalSpy failed(&page,&MiniAppPage::failed),received(&page,&MiniAppPage::messageReceived);
        QVERIFY(page.postMessage(QJsonObject{{"beforeReady",true}}));page.resize(640,400);page.show();
        QTRY_VERIFY_WITH_TIMEOUT(page.isReady()||!failed.isEmpty(),15000);QVERIFY2(page.isReady(),failed.isEmpty()?"No ready signal":qPrintable(failed.first().at(0).toString()));
        QTRY_COMPARE_WITH_TIMEOUT(received.count(),1,4000);QVERIFY(received.first().at(0).toJsonValue().toObject()["ack"].toObject()["beforeReady"].toBool());
        QVERIFY(page.postMessage(QJsonObject{{"selection",QJsonArray{"1:2","3:4"}}}));QTRY_COMPARE_WITH_TIMEOUT(received.count(),2,3000);
        QCOMPARE(received.last().at(0).toJsonValue().toObject()["ack"].toObject()["selection"].toArray().size(),2);
    }

    void hostOwnershipRefreshAndClosedTargets() {
        const auto plugin=temp->filePath("plugin");QVERIFY(QDir().mkpath(plugin+"/apps/main"));
        QVERIFY(write(plugin+"/apps/main/index.html","<script>addEventListener('message',e=>parent.postMessage({ack:e.data},'*'));</script>"));
        MiniAppHost host("agent-a",temp->path());host.setPluginRoots({{"fixture",plugin}});
        QSignalSpy complete(&host,&MiniAppHost::completed),messages(&host,&MiniAppHost::messageReceived),opened(&host,&MiniAppHost::pageOpened),closed(&host,&MiniAppHost::pageCloseRequested);
        connect(&host,&MiniAppHost::pageOpened,&host,[&host](const QString& instance,MiniAppPage* page){page->resize(500,300);page->show();host.bindTab(instance,"app-tab-1");});
        host.execute("open","open",{{"appId","main"},{"pluginId","fixture"}});QCOMPARE(complete.count(),1);QCOMPARE(complete.last().at(1).toJsonValue().toObject()["tabId"].toString(),"app-tab-1");QVERIFY(host.activePage());
        QTRY_VERIFY_WITH_TIMEOUT(host.activePage()->isReady(),15000);
        host.execute("send","send",{{"appId","main"},{"pluginId","fixture"},{"data",QJsonObject{{"agentId","forged"},{"payload",1}}}});
        QTRY_COMPARE_WITH_TIMEOUT(messages.count(),1,3000);QCOMPARE(messages.first().at(0).toString(),"main");QCOMPARE(messages.first().at(1).toString(),"fixture");
        const auto instance=host.activeInstanceId();const auto* page=host.activePage();host.execute("refresh","open",{{"appId","main"},{"pluginId","fixture"}});QCOMPARE(opened.count(),1);QCOMPARE(host.activeInstanceId(),instance);QCOMPARE(host.activePage(),page);
        host.execute("wrong","send",{{"appId","main"},{"pluginId","other"},{"data",1}});QVERIFY(!complete.last().at(2).toString().isEmpty());
        host.execute("close","close",{});QCOMPARE(closed.count(),1);QVERIFY(!host.activePage());
        host.execute("closed","send",{{"appId","main"},{"pluginId","fixture"},{"data",2}});QVERIFY(!complete.last().at(2).toString().isEmpty());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }

    void twoAgentProfilesAndResourceOriginIsolation() {
        const auto app=temp->filePath("app");QVERIFY(QDir().mkpath(app));
        QVERIFY(write(app+"/index.html",R"HTML(<script>addEventListener('message',async e=>{let result;if(e.data.action==='set'){localStorage.setItem('value',e.data.value);result='stored';}else if(e.data.action==='get')result=localStorage.getItem('value');else if(e.data.action==='fetch'){try{const r=await fetch(e.data.url);result=r.status;}catch(error){result='denied';}}else if(e.data.action==='spoof'){try{chrome.webview.postMessage({channel:'ade-mini-app',kind:'message',data:{spoof:true}});}catch{}return;}parent.postMessage({result},'*');});</script>)HTML"));
        MiniAppPage a(QUrl("app://plugins/fixture/main"),app,temp->filePath("profile-a")),b(QUrl("app://plugins/fixture/main"),app,temp->filePath("profile-b"));
        QSignalSpy first(&a,&MiniAppPage::messageReceived),second(&b,&MiniAppPage::messageReceived);a.show();b.show();
        QTRY_VERIFY_WITH_TIMEOUT(a.isReady()&&b.isReady(),15000);
        QVERIFY(a.postMessage(QJsonObject{{"action","set"},{"value","agent-a-private"}}));QTRY_COMPARE_WITH_TIMEOUT(first.count(),1,3000);
        QVERIFY(b.postMessage(QJsonObject{{"action","get"}}));QTRY_COMPARE_WITH_TIMEOUT(second.count(),1,3000);QVERIFY(second.first().at(0).toJsonValue().toObject()["result"].isNull());
        QVERIFY(a.postMessage(QJsonObject{{"action","fetch"},{"url","app://plugins/other/main/index.html"}}));QTRY_COMPARE_WITH_TIMEOUT(first.count(),2,3000);QCOMPARE(first.last().at(0).toJsonValue().toObject()["result"].toInt(),403);
        QVERIFY(a.postMessage(QJsonObject{{"action","fetch"},{"url","file:///C:/Windows/win.ini"}}));QTRY_COMPARE_WITH_TIMEOUT(first.count(),3,3000);QCOMPARE(first.last().at(0).toJsonValue().toObject()["result"].toString(),"denied");
        QVERIFY(a.postMessage(QJsonObject{{"action","spoof"}}));QTest::qWait(100);QCOMPARE(first.count(),3);
        const auto previous=a.browser()->currentUrl();a.browser()->navigate(QUrl("about:blank"));QTest::qWait(100);QCOMPARE(a.browser()->currentUrl(),previous);
    }

    void pendingOpenCancellationAndReentrantDestruction() {
        const auto plugin=temp->filePath("plugin");QVERIFY(QDir().mkpath(plugin+"/apps/main"));QVERIFY(write(plugin+"/apps/main/index.html","<title>test</title>"));
        MiniAppHost host("agent-a",temp->path());host.setPluginRoots({{"fixture",plugin}});QSignalSpy completed(&host,&MiniAppHost::completed);
        host.execute("pending","open",{{"appId","main"},{"pluginId","fixture"}});QCOMPARE(completed.count(),0);QPointer<MiniAppPage> page=host.activePage();host.cancel("pending");QCOMPARE(completed.count(),1);QVERIFY(!completed.first().at(2).toString().isEmpty());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!page);
        auto* doomed=new MiniAppHost("other",temp->path());doomed->setPluginRoots({{"fixture",plugin}});QPointer<MiniAppHost> guard(doomed);connect(doomed,&MiniAppHost::pageOpened,this,[doomed]{delete doomed;});
        doomed->execute("delete","open",{{"appId","main"},{"pluginId","fixture"}});QVERIFY(!guard);
    }
};
QTEST_MAIN(MiniAppTests)
#include "test_mini_apps.moc"
