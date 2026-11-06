#include "apps/MiniAppHost.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "sandbox/SandboxHost.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <memory>
using namespace ade;
namespace {
QString literal(const QString& value){const auto bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(bytes.mid(1,bytes.size()-2));}
struct Result{bool arrived=false,error=true;QString raw,value;};
}
class SandboxAppTests final:public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> temp;
    std::unique_ptr<QTcpServer> server;
    int sequence=0;
    Result run(SandboxHost& host,const QString& script){
        const QString id=QString("script-%1").arg(++sequence);QSignalSpy completed(&host,&SandboxHost::completed);
        host.execute(id,"executeSandboxJs",{{"explanation","Test native app"},{"script",script}});
        QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<10000){for(const auto& row:completed)if(row[0]==id){const auto raw=row[1].toString();return {true,row[2].toBool(),raw,QJsonDocument::fromJson(raw.toUtf8()).object()["result"].toString()};}completed.wait(30);}return {false,true,"Timed out",{}};
    }
    QJsonObject cdp(BrowserToolBridge& bridge,const QString& tab,const QString& method,const QJsonObject& params={}){
        const QString id=QString("cdp-%1").arg(++sequence);QSignalSpy done(&bridge,&BrowserToolBridge::completed);bridge.executeForAgent(id,"fixture-agent","sendCDP",{{"tabId",tab},{"method",method},{"params",params}});
        QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<5000){for(const auto& row:done)if(row[0]==id)return QJsonDocument::fromJson(row[1].toString().toUtf8()).object();done.wait(30);}return {};
    }
    QJsonObject sessionCdp(BrowserToolBridge& bridge, BrowserWidget* browser, const QString& tab,
                           const QString& session, const QString& method, const QJsonObject& params={}) {
        const int messageId=++sequence;
        QSignalSpy events(browser,&BrowserWidget::devToolsEventReceived);
        const auto message=QJsonDocument(QJsonObject{{"id",messageId},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact);
        const auto sent=cdp(bridge,tab,"Target.sendMessageToTarget",{{"sessionId",session},{"message",QString::fromUtf8(message)}});
        if(sent.contains("error")) return sent;
        QElapsedTimer elapsed;elapsed.start();
        while(elapsed.elapsed()<5000){
            for(const auto& row:events){
                if(row[0].toString()!="Target.receivedMessageFromTarget") continue;
                const auto envelope=QJsonDocument::fromJson(row[1].toString().toUtf8()).object();
                if(envelope["sessionId"].toString()!=session) continue;
                const auto response=QJsonDocument::fromJson(envelope["message"].toString().toUtf8()).object();
                if(response["id"].toInt()!=messageId) continue;
                return response.contains("error")?response:response["result"].toObject();
            }
            events.wait(30);
        }
        return {{"error","Timed out waiting for iframe CDP response"}};
    }
private slots:
    void init(){
        temp=std::make_unique<QTemporaryDir>();QVERIFY(temp->isValid());server=std::make_unique<QTcpServer>();QVERIFY(server->listen(QHostAddress::LocalHost));
        connect(server.get(),&QTcpServer::newConnection,this,[this]{while(server->hasPendingConnections()){auto* socket=server->nextPendingConnection();connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QTcpSocket::readyRead,socket,[socket]{const auto request=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",request);if(!request.contains("\r\n\r\n")||socket->property("done").toBool())return;socket->setProperty("done",true);const QByteArray body="<!doctype html><title>Figma selection fixture</title><script>globalThis.figma={currentPage:{selection:[]}};</script>";socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();});}});
    }
    void cleanup(){server.reset();const QString path=temp->path();temp->setAutoRemove(false);temp.reset();QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<5000&&!QDir(path).removeRecursively())QTest::qWait(50);QVERIFY(!QFile::exists(path));}

    void realCdpSelectionUpdatesFigmaAppAfterSetupReturns(){
        BrowserToolBridge bridge;BrowserWidget fixture;fixture.setProfileDirectory(temp->filePath("browser"));fixture.resize(800,500);
        const QString tab=bridge.registerTab(&fixture,"t_fixture");QCOMPARE(tab,"t_fixture");
        QSignalSpy loaded(&fixture,&BrowserWidget::loadingChanged);fixture.navigate(QUrl(QString("http://127.0.0.1:%1/").arg(server->serverPort())));fixture.show();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.isReady()&&loaded.count()>=2,15000);
        SandboxHost host;host.setAgentId("fixture-agent");host.setExecutionLimits(2000,4000,6000);
        MiniAppHost apps("fixture-agent",temp->path());const auto package=QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath("../assets/plugins/figma");QVERIFY(QFile::exists(package+"/apps/figma-app/index.html"));apps.setPluginRoots({{"figma",package}});
        QHash<QString,QJsonObject> listeners;
        connect(&host,&SandboxHost::browserRequest,&bridge,[&bridge](const QString& id,const QString& tab,const QString& method,const QJsonObject& params){bridge.executeForAgent(id,"fixture-agent","sendCDP",{{"tabId",tab},{"method",method},{"params",params}});});
        connect(&bridge,&BrowserToolBridge::completed,&host,&SandboxHost::completeBrowserRequest);
        connect(&host,&SandboxHost::appRequest,&apps,[&apps](const QString& id,const QString& agent,const QString& operation,const QJsonObject& args){QCOMPARE(agent,"fixture-agent");apps.execute(id,operation,args);});
        connect(&apps,&MiniAppHost::completed,&host,&SandboxHost::completeAppRequest);
        connect(&host,&SandboxHost::eventSubscriptionRequested,&host,[&](const QString& id,const QString& agent,const QString& kind,const QJsonObject& args){QCOMPARE(agent,"fixture-agent");auto entry=args;entry["kind"]=kind;listeners[id]=entry;if(kind=="cdp")bridge.subscribeEvent(id,args["tabId"].toString(),args["event"].toString());});
        connect(&host,&SandboxHost::eventUnsubscriptionRequested,&bridge,[&](const QString& id){listeners.remove(id);bridge.unsubscribeEvent(id);});
        connect(&bridge,&BrowserToolBridge::eventReceived,&host,[&](const QString& id,const QJsonObject& value){if(listeners.contains(id))host.deliverEvent(id,value);});
        connect(&bridge,&BrowserToolBridge::subscriptionEnded,&host,&SandboxHost::endEventSubscription);
        connect(&apps,&MiniAppHost::messageReceived,&host,[&](const QString& app,const QString& plugin,const QJsonValue& value){const auto copy=listeners;for(auto it=copy.cbegin();it!=copy.cend();++it)if(it.value()["kind"]=="appMessage"&&it.value()["appId"]==app&&it.value()["pluginId"]==plugin)host.deliverEvent(it.key(),value);});
        connect(&host,&SandboxHost::contextReset,&apps,[&](const QString&){apps.reset();});
        connect(&host,&SandboxHost::brokerCancellationRequested,&bridge,&BrowserToolBridge::cancel);
        connect(&host,&SandboxHost::brokerCancellationRequested,&apps,&MiniAppHost::cancel);
        connect(&apps,&MiniAppHost::pageOpened,&apps,[&](const QString& instance,MiniAppPage* page){
            auto* browser=page->browser();const auto id=bridge.registerTab(browser);page->resize(720,400);page->show();apps.bindTab(instance,id);
        });
        QString script=QString::fromUtf8(R"JS(
globalThis.figmaAppMessages=[];
API.onMessage('figma-app',message=>figmaAppMessages.push(message),{pluginId:'figma'});
await API.openApp('figma-app',{pluginId:'figma',title:'Figma Selection'});
const tabId=TAB;
await API.sendCDP(tabId,'Runtime.addBinding',{name:'sendToAgent'});
globalThis.figmaSelection=[];
globalThis._unsubFigmaBinding=API.onCDPEvent(tabId,'Runtime.bindingCalled',event=>{
 if(event.name==='sendToAgent'){try{const payload=JSON.parse(event.payload);globalThis.figmaSelection=payload.nodes||[];API.sendMessage('figma-app',{type:'selectionChanged',nodes:payload.nodes||[],timestamp:payload.timestamp},{pluginId:'figma'});}catch{}}
});
await API.sendCDP(tabId,'Runtime.evaluate',{expression:`(()=>{if(window.__figmaSelectionWatcher)return;window.__figmaSelectionWatcher=true;window.__lastSelKey='';setInterval(()=>{try{if(typeof figma==='undefined'||!figma.currentPage)return;const sel=figma.currentPage.selection,key=sel.map(n=>n.id).join(',');if(key===window.__lastSelKey)return;window.__lastSelKey=key;window.sendToAgent(JSON.stringify({timestamp:Date.now(),nodes:sel.map(n=>({id:n.id,name:n.name,type:n.type,width:n.width,height:n.height}))}));}catch{}},200);})();`});
return 'monitoring';
)JS");script.replace("TAB",literal(tab));
        const auto setup=run(host,script);QVERIFY2(setup.arrived&&!setup.error,qPrintable(setup.raw));QCOMPARE(setup.value,"monitoring");QVERIFY(apps.activePage());
        QTRY_VERIFY_WITH_TIMEOUT(apps.activePage()->isReady(),15000);auto* appBrowser=apps.activePage()->browser();
        const auto targets=cdp(bridge,apps.activeTabId(),"Target.getTargets")["targetInfos"].toArray();
        QString appTarget;
        for(const auto& entry:targets){const auto target=entry.toObject();if(target["type"]=="iframe"&&target["url"]=="app://plugins/figma/figma-app/index.html")appTarget=target["targetId"].toString();}
        QVERIFY2(!appTarget.isEmpty(),"Packaged app iframe must be present as its own CDP target");
        appBrowser->subscribeDevToolsEvent("Target.receivedMessageFromTarget");
        const auto attached=cdp(bridge,apps.activeTabId(),"Target.attachToTarget",{{"targetId",appTarget},{"flatten",false}});
        const auto appSession=attached["sessionId"].toString();QVERIFY2(!appSession.isEmpty(),qPrintable(QString::fromUtf8(QJsonDocument(attached).toJson())));
        QSignalSpy messages(&apps,&MiniAppHost::messageReceived);
        const auto mutate=cdp(bridge,tab,"Runtime.evaluate",{{"expression","figma.currentPage.selection=[{id:'1:23',name:'Header Frame',type:'FRAME',width:1440,height:900}];"}});QVERIFY(!mutate.contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(messages.count()>0,4000);
        const auto state=run(host,"return {selection:figmaSelection,messages:figmaAppMessages};");QVERIFY2(!state.error,qPrintable(state.raw));const auto value=QJsonDocument::fromJson(state.value.toUtf8()).object();QCOMPARE(value["selection"].toArray().first().toObject()["name"].toString(),"Header Frame");QVERIFY(!value["messages"].toArray().isEmpty());
        const auto badge=sessionCdp(bridge,appBrowser,apps.activeTabId(),appSession,"Runtime.evaluate",{{"expression","document.querySelector('.badge-name').textContent"},{"returnByValue",true}});QCOMPARE(badge["result"].toObject()["value"].toString(),"Header Frame");
        const auto custom=run(host,"await API.sendMessage('figma-app',{type:'custom',data:'<img src=x onerror=\"globalThis.injected=true\">'},{pluginId:'figma'});return 1;");QVERIFY2(!custom.error,qPrintable(custom.raw));QTest::qWait(80);
        const auto safe=sessionCdp(bridge,appBrowser,apps.activeTabId(),appSession,"Runtime.evaluate",{{"expression","[document.querySelector('.custom-data').textContent,document.querySelectorAll('.custom-data img').length,typeof injected]"},{"returnByValue",true}});
        const auto safety=safe["result"].toObject()["value"].toArray();QCOMPARE(safety.size(),3);QVERIFY(safety[0].toString().contains("<img"));QCOMPARE(safety[1].toInt(),0);QCOMPARE(safety[2].toString(),"undefined");
        const auto close=run(host,"_unsubFigmaBinding();await API.closeApp();return 'closed';");QVERIFY2(!close.error,qPrintable(close.raw));QVERIFY(!apps.activePage());
        cdp(bridge,tab,"Runtime.evaluate",{{"expression","figma.currentPage.selection=[{id:'other',name:'Ignored'}];"}});QTest::qWait(300);QCOMPARE(run(host,"return figmaSelection[0].name;").value,"Header Frame");
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }

    void foreignMiniAppTabsAreHiddenAndCannotReceiveAgentCdp(){
        BrowserToolBridge bridge;BrowserWidget first,second,shared;first.setProperty("adeOwnerAgentId","alpha");second.setProperty("adeOwnerAgentId","beta");
        const auto a=bridge.registerTab(&first,"a"),b=bridge.registerTab(&second,"b"),s=bridge.registerTab(&shared,"shared");bridge.setActiveTab(b);
        QVERIFY(bridge.canAccessTab(a,"alpha"));QVERIFY(!bridge.canAccessTab(b,"alpha"));QVERIFY(bridge.canAccessTab(s,"alpha"));QSignalSpy done(&bridge,&BrowserToolBridge::completed);
        bridge.executeForAgent("list","alpha","getBrowserTabs",{});QTRY_COMPARE(done.count(),1);const auto snapshot=QJsonDocument::fromJson(done.first()[1].toString().toUtf8()).object();QCOMPARE(snapshot["tabs"].toArray().size(),2);QVERIFY(snapshot["activeTabId"].toString().isEmpty());
        bridge.executeForAgent("forbidden","alpha","sendCDP",{{"tabId",b},{"method","Runtime.evaluate"},{"params",QJsonObject{{"expression","1"}}}});QTRY_COMPARE(done.count(),2);QVERIFY(done.last()[2].toBool());QVERIFY(done.last()[1].toString().contains("another agent"));
    }
};
QTEST_MAIN(SandboxAppTests)
#include "test_sandbox_apps.moc"
