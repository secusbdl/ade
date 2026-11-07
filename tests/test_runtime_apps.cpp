#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "apps/MiniAppPage.h"
#include <QtTest>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QBuffer>
#include <QImage>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
using namespace ade;
namespace {
struct Result {QString output;bool error=true;};
QString quote(const QString& value){auto json=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(json.mid(1,json.size()-2));}
Result run(AgentToolHost& host,const QString& script){
    QSignalSpy results(host.sandbox(),&SandboxHost::completed);const auto id=QUuid::createUuid().toString();
    host.sandbox()->execute(id,"executeSandboxJs",{{"explanation","Test native app"},{"script",script}});
    QElapsedTimer timer;timer.start();while(results.isEmpty()&&timer.elapsed()<15000)QTest::qWait(20);
    if(results.isEmpty())return {"Timed out waiting for app tool",true};
    return {results.last()[1].toString(),results.last()[2].toBool()};
}
bool observed(const QSignalSpy& spy,const QString& type){for(const auto& row:spy)if(row[2].toJsonValue().toObject()["type"]==type)return true;return false;}
bool writeFixtureFile(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
class AuthoringProvider final:public QObject {
public:
    QTcpServer server;QList<QPair<QString,QJsonObject>> calls;QList<QJsonObject> requests;
    AuthoringProvider(){
        server.listen(QHostAddress::LocalHost);
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                const auto data=socket->property("data").toByteArray()+socket->readAll();socket->setProperty("data",data);
                const auto end=data.indexOf("\r\n\r\n");if(end<0)return;int length=0;
                for(const auto& line:data.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(data.size()<end+4+length)return;
                socket->setProperty("handled",true);
                requests.append(QJsonDocument::fromJson(data.mid(end+4,length)).object());
                QJsonObject choice;
                if(!calls.isEmpty()){
                    const auto item=calls.takeFirst();const QJsonObject function{{"name",item.first},{"arguments",QString::fromUtf8(QJsonDocument(item.second).toJson(QJsonDocument::Compact))}};
                    const QJsonObject call{{"index",0},{"id",QStringLiteral("author-%1").arg(requests.size())},{"type","function"},{"function",function}};
                    choice={{"delta",QJsonObject{{"tool_calls",QJsonArray{call}}}},{"finish_reason","tool_calls"}};
                }else choice={{"delta",QJsonObject{{"content","Authored local fixture."}}},{"finish_reason","stop"}};
                const auto body="data: "+QJsonDocument(QJsonObject{{"choices",QJsonArray{choice}}}).toJson(QJsonDocument::Compact)+"\n\ndata: [DONE]\n\n";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
            });
        }});
    }
    ProviderConfig provider()const{return {"fixture","Loopback authoring",QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()),"fake-fixture-token","fixture","openai"};}
};
QString resultValue(const Result& result){return QJsonDocument::fromJson(result.output.toUtf8()).object()["result"].toString();}
bool installFixture(const QString& root){
    if(!QDir().mkpath(root+"/demo"))return false;
    QFile file(root+"/demo/index.html");
    const QByteArray html=R"(<!doctype html><body>Fixture<script>
      window.addEventListener('message', e => {
        if(e.data && e.data.type==='ping') parent.postMessage({type:'pong',value:e.data.value},'*');
        if(e.data && e.data.type==='render') {document.body.dataset.value=e.data.value; parent.postMessage({type:'rendered',value:e.data.value},'*');}
      });
    </script></body>)";
    return file.open(QIODevice::WriteOnly)&&file.write(html)==html.size();
}
void connectTabs(AgentToolHost& host,BrowserToolBridge& bridge){
    QObject::connect(host.apps(),&MiniAppHost::pageOpened,&host,[&host,&bridge](const QString& instance,MiniAppPage* page,const QString&,bool){
        page->setAttribute(Qt::WA_DontShowOnScreen);page->resize(700,500);
        const auto tab=bridge.registerTab(page->browser());host.apps()->bindTab(instance,tab);page->show();
    });
}
}
class RuntimeAppTests:public QObject {
    Q_OBJECT
private slots:
    void nativeToolsAuthorAppsWithoutWorkspace(){
        QTemporaryDir data;AgentEngine engine;AgentToolHost host(&engine,nullptr,"native-author",data.path());AuthoringProvider provider;
        provider.calls={
            {"mkdir",{{"path","apps/native"}}},
            {"write",{{"path","apps/native/index.html"},{"content","<body>Original</body>"}}},
            {"multiEdit",{{"path","apps/native/index.html"},{"edits",QJsonArray{QJsonObject{{"old_string","Original"},{"new_string","Updated"}}}}}},
            {"read",{{"path","apps/native/index.html"}}},
            {"ls",{{"path","apps/native"}}},
            {"glob",{{"mount_prefix","apps"},{"path","native"},{"pattern","**/*.html"}}},
            {"grepSearch",{{"mount_prefix","apps"},{"path","native"},{"query","Updated"}}}
        };
        engine.setProvider(provider.provider());engine.setAlwaysAllow(true);QSignalSpy done(&engine,&AgentEngine::finished),failed(&engine,&AgentEngine::failed),results(&engine,&AgentEngine::toolFinished);
        engine.submit("Author the controlled local mini-app fixture.");QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty()||!failed.isEmpty(),15000);
        QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first()[0].toString()));QCOMPARE(results.count(),7);
        for(const auto& result:results)QVERIFY2(!result[2].toBool(),qPrintable(result[1].toString()));
        QCOMPARE(results[3][1].toString(),QString("File opened and loaded into context."));
        QCOMPARE(results[4][1].toString(),QString("Directory opened and loaded into context."));
        QVERIFY(results[5][1].toString().contains("index.html"));QVERIFY(results[6][1].toString().contains("Updated"));
        QList<QJsonObject> savedResults;for(const auto& value:engine.messages())if(value.toObject()["role"]=="tool")savedResults.append(value.toObject());
        QCOMPARE(savedResults.size(),7);
        for(const int index:{3,4}){
            const auto metadata=savedResults[index]["metadata"].toObject();const auto reads=metadata["fileReads"].toArray();QCOMPARE(reads.size(),1);
            const auto entry=reads.first().toObject();QCOMPARE(entry["tool"].toString(),index==3?QString("read"):QString("ls"));
            QCOMPARE(entry["path"].toString(),index==3?QString("apps/native/index.html"):QString("apps/native"));
            QVERIFY(metadata["pathReferences"].toObject().contains(entry["path"].toString()));
            QVERIFY(!savedResults[index]["content"].toString().contains("<body>Updated</body>"));
        }
        QCOMPARE(provider.requests.size(),8);
        const auto userContext=[](const QJsonObject& request){QString text;for(const auto& message:request["messages"].toArray())if(message.toObject()["role"]=="user"){const QJsonValue content=message.toObject().value("content");if(content.isString())text+=content.toString();else for(const auto& part:content.toArray())text+=part.toObject()["text"].toString();}return text;};
        const auto fileContext=userContext(provider.requests[4]);QVERIFY(fileContext.contains("apps/native/index.html"));QVERIFY(fileContext.contains("1|<body>Updated</body>"));
        const auto directoryContext=userContext(provider.requests[5]);QVERIFY(directoryContext.contains("type:directory"));QVERIFY(directoryContext.contains("index.html"));
        QVERIFY(engine.workspace().isEmpty());QFile file(host.apps()->agentAppsDirectory()+"/native/index.html");QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("<body>Updated</body>"));
        QStringList names;for(const auto& value:provider.requests.first()["tools"].toArray())names.append(value.toObject()["function"].toObject()["name"].toString());
        QVERIFY(names.contains("write"));QVERIFY(names.contains("executeSandboxJs"));QVERIFY(!names.contains("runCommand"));
        const auto sandbox=run(host,"return await fsPromises.readFile('apps/native/index.html','utf8');");QVERIFY2(!sandbox.error,qPrintable(sandbox.output));QCOMPARE(resultValue(sandbox),"<body>Updated</body>");
        QString error;const auto captured=host.captureAttachments(QJsonArray{QJsonObject{{"path","apps/native/index.html"}}},&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto hash=QString::fromLatin1(QCryptographicHash::hash("<body>Updated</body>",QCryptographicHash::Sha256).toHex());QCOMPARE(captured["pathReferences"].toObject()["apps/native/index.html"].toString(),hash);
        QVERIFY(!host.apps()->setAgentAppsDirectory(data.filePath("unbound")));
    }

    void authoredSiblingAssetsLoadAndReloadInActualApp(){
        QTemporaryDir data;AgentEngine engine;BrowserToolBridge bridge;AgentToolHost host(&engine,&bridge,"asset-author",data.path());connectTabs(host,bridge);
        QImage image(1,1,QImage::Format_ARGB32);image.fill(Qt::red);QByteArray png;QBuffer buffer(&png);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));
        const QString html="<!doctype html><html><head><link rel='stylesheet' href='./style.css'></head><body><img id='pixel' src='./pixel.png'><script src='./app.js'></script></body></html>";
        const QString js="addEventListener('message',e=>{if(e.data.type==='probe')parent.postMessage({type:'asset-result',version:'one',color:getComputedStyle(document.body).color,image:document.getElementById('pixel').naturalWidth},'*');});";
        auto result=run(host,"await fsPromises.mkdir('apps/demo',{recursive:true});await fsPromises.writeFile('apps/demo/index.html',"+quote(html)+");await fsPromises.writeFile('apps/demo/style.css','body{color:rgb(12,34,56)}');await fsPromises.writeFile('apps/demo/app.js',"+quote(js)+");await fsPromises.writeFile('apps/demo/pixel.png',Buffer.from("+quote(QString::fromLatin1(png.toBase64()))+",'base64'));return await API.openApp('demo');");
        QVERIFY2(!result.error,qPrintable(result.output));QTRY_VERIFY_WITH_TIMEOUT(host.apps()->activePage()&&host.apps()->activePage()->isReady(),15000);
        const auto instance=host.apps()->activeInstanceId();const auto* page=host.apps()->activePage();QSignalSpy messages(host.apps(),&MiniAppHost::messageReceived),opened(host.apps(),&MiniAppHost::pageOpened);
        result=run(host,"await API.sendMessage('demo',{type:'probe'});return 1;");QVERIFY2(!result.error,qPrintable(result.output));QTRY_VERIFY_WITH_TIMEOUT(observed(messages,"asset-result"),5000);
        auto value=messages.last()[2].toJsonValue().toObject();QCOMPARE(value["version"].toString(),"one");QCOMPARE(value["color"].toString(),"rgb(12, 34, 56)");QCOMPARE(value["image"].toInt(),1);messages.clear();
        result=run(host,"await fsPromises.writeFile('apps/demo/app.js',"+quote(QString(js).replace("'one'","'two'"))+");return await API.openApp('demo');");QVERIFY2(!result.error,qPrintable(result.output));
        QCOMPARE(host.apps()->activeInstanceId(),instance);QCOMPARE(host.apps()->activePage(),page);QCOMPARE(opened.count(),0);
        QTRY_VERIFY_WITH_TIMEOUT(host.apps()->activePage()->isReady(),15000);result=run(host,"await API.sendMessage('demo',{type:'probe'});return 1;");QVERIFY2(!result.error,qPrintable(result.output));QTRY_VERIFY_WITH_TIMEOUT(observed(messages,"asset-result"),5000);
        QCOMPARE(messages.last()[2].toJsonValue().toObject()["version"].toString(),"two");
        result=run(host,"return (await fsPromises.readFile('apps/demo/pixel.png')).toString('base64');");QVERIFY(!result.error);QCOMPARE(resultValue(result),QString::fromLatin1(png.toBase64()));
        const QPointer<MiniAppPage> closing=host.apps()->activePage();QVERIFY(host.apps()->ownsPage(instance,closing));
        host.apps()->closeTab(instance);QVERIFY(closing);QVERIFY(!host.apps()->ownsPage(instance,closing));QVERIFY(!host.apps()->activePage());
    }

    void authoringRootsAreAgentScopedAndSurviveWorkspaceChanges(){
        QTemporaryDir data;AgentEngine firstEngine,secondEngine;AgentToolHost first(&firstEngine,nullptr,"first",data.path()),second(&secondEngine,nullptr,"second",data.path());
        QVERIFY(first.apps()->agentAppsDirectory()!=second.apps()->agentAppsDirectory());
        auto result=run(first,"await fsPromises.writeFile('apps/private.txt','first only');return 1;");QVERIFY2(!result.error,qPrintable(result.output));
        QVERIFY(run(second,"return await fsPromises.readFile('apps/private.txt','utf8');").error);
        QVERIFY(run(second,"return await fsPromises.readFile("+quote(first.apps()->agentAppsDirectory()+"/private.txt")+",'utf8');").error);
        const auto workspace=data.filePath("workspace");QVERIFY(QDir().mkpath(workspace));QVERIFY(first.setWorkspace(workspace));
        result=run(first,"return await fsPromises.readFile('apps/private.txt','utf8');");QVERIFY(!result.error);QCOMPARE(resultValue(result),"first only");
        QString error;const auto captured=first.captureAttachments(QJsonArray{QJsonObject{{"path","apps/private.txt"}}},&error);QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(!captured["pathReferences"].toObject().isEmpty());
        QVERIFY(first.setWorkspace({}));result=run(first,"return await fsPromises.readFile('apps/private.txt','utf8');");QVERIFY(!result.error);QCOMPARE(resultValue(result),"first only");
    }

    void cancelQueuedBinaryAppWritePreventsDiskMutation(){
        QTemporaryDir data;AgentEngine engine;AgentToolHost host(&engine,nullptr,"cancel-app-asset",data.path());bool queued=false;
        connect(host.sandbox(),&SandboxHost::fileRequest,&host,[&](const QString&,const QString& name){if(name=="writeBytes"){queued=true;host.sandbox()->cancel("binary-cancel");}});
        QSignalSpy completed(host.sandbox(),&SandboxHost::completed),changes(&host,&AgentToolHost::fileChanged);
        host.sandbox()->execute("binary-cancel","executeSandboxJs",{{"explanation","Cancel binary app asset"},{"script","await fsPromises.writeFile('apps/canceled.bin',Buffer.from([0,255,128]));"}});
        QTRY_VERIFY_WITH_TIMEOUT(queued,3000);QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,3000);QVERIFY(completed.first()[2].toBool());QTest::qWait(30);
        QVERIFY(!QFileInfo::exists(host.apps()->agentAppsDirectory()+"/canceled.bin"));QCOMPARE(changes.count(),0);
    }
    void cancelQueuedSandboxWritePreventsDiskMutation(){
        QTemporaryDir data;const auto workspace=data.filePath("workspace");QVERIFY(QDir().mkpath(workspace));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"cancel-write",data.path());host.setWorkspace(workspace);
        QSignalSpy results(host.sandbox(),&SandboxHost::completed),changes(&host,&AgentToolHost::fileChanged);
        bool queued=false;
        // The host's fileRequest connection has queued ToolExecutor work before
        // this later connection cancels. This is the actual zero-timer window.
        connect(host.sandbox(),&SandboxHost::fileRequest,&host,[&](const QString&,const QString& name){
            if(name=="write"){queued=true;host.sandbox()->cancel("cancel-queued-write");}
        });
        host.sandbox()->execute("cancel-queued-write","executeSandboxJs",{{"explanation","Cancel queued write"},{"script","await fsPromises.writeFile('ws1/canceled.txt','must not be written');return 1;"}});
        QTRY_VERIFY_WITH_TIMEOUT(queued,3000);QTRY_COMPARE_WITH_TIMEOUT(results.count(),1,3000);QVERIFY(results.first()[2].toBool());
        QTest::qWait(30);QVERIFY2(!QFileInfo::exists(workspace+"/canceled.txt"),"Canceled queued file work must not mutate disk");QCOMPARE(changes.count(),0);
    }
    void workspaceChangeResetsIdleRealmAndOldCallbacks(){
        QTemporaryDir data;const auto a=data.filePath("a"),b=data.filePath("b");QVERIFY(QDir().mkpath(a));QVERIFY(QDir().mkpath(b));
        QVERIFY(writeFixtureFile(a+"/identity.txt","workspace A"));QVERIFY(writeFixtureFile(b+"/identity.txt","workspace B"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"workspace-reset",data.path());host.setWorkspace(a);
        QString listener;connect(host.sandbox(),&SandboxHost::eventSubscriptionRequested,&host,[&](const QString& id){listener=id;});
        auto result=run(host,"globalThis.workspaceMarker='old realm';API.onMessage('old',()=>fsPromises.writeFile('ws1/stale.txt','old callback'));return 1;");QVERIFY2(!result.error,qPrintable(result.output));QVERIFY(!listener.isEmpty());
        QSignalSpy resets(host.sandbox(),&SandboxHost::contextReset);host.setWorkspace(b);
        QCOMPARE(resets.count(),1);host.sandbox()->deliverEvent(listener,1);
        result=run(host,"return {marker:typeof workspaceMarker,identity:await fsPromises.readFile('ws1/identity.txt','utf8')};");QVERIFY2(!result.error,qPrintable(result.output));
        const auto values=QJsonDocument::fromJson(resultValue(result).toUtf8()).object();QCOMPARE(values["marker"].toString(),"undefined");QCOMPARE(values["identity"].toString(),"workspace B");
        QTest::qWait(30);QVERIFY(!QFileInfo::exists(a+"/stale.txt"));QVERIFY(!QFileInfo::exists(b+"/stale.txt"));
    }
    void workspaceSwapDuringQueuedFileKeepsFileAndAttachmentRootsAligned(){
        QTemporaryDir data;const auto a=data.filePath("a"),b=data.filePath("b");QVERIFY(QDir().mkpath(a));QVERIFY(QDir().mkpath(b));
        QVERIFY(writeFixtureFile(a+"/identity.txt","workspace A"));QVERIFY(writeFixtureFile(b+"/identity.txt","workspace B"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"workspace-queued",data.path());host.setWorkspace(a);
        bool swapped=false;connect(host.sandbox(),&SandboxHost::fileRequest,&host,[&](const QString&,const QString& name){if(!swapped&&name=="write"){swapped=true;host.setWorkspace(b);}});
        const auto old=run(host,"await fsPromises.writeFile('ws1/old-work.txt','obsolete write');return 'done';");QVERIFY(swapped);
        QString error;const auto metadata=host.captureAttachments(QJsonArray{QJsonObject{{"path","ws1/identity.txt"}}},&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto expected=QString::fromLatin1(QCryptographicHash::hash("workspace B",QCryptographicHash::Sha256).toHex());
        QCOMPARE(metadata["pathReferences"].toObject()["ws1/identity.txt"].toString(),expected);
        const auto fresh=run(host,"const text=await fsPromises.readFile('w1/identity.txt','utf8');await fsPromises.writeFile('ws1/new-work.txt',text);return text;");
        QVERIFY2(!fresh.error,qPrintable(fresh.output));QCOMPARE(resultValue(fresh),"workspace B");
        QVERIFY(old.error);QVERIFY(!QFileInfo::exists(a+"/old-work.txt"));QVERIFY(!QFileInfo::exists(b+"/old-work.txt"));
        QVERIFY(!QFileInfo::exists(a+"/new-work.txt"));QFile newFile(b+"/new-work.txt");QVERIFY(newFile.open(QIODevice::ReadOnly));QCOMPARE(newFile.readAll(),QByteArray("workspace B"));
    }
    void sameCanonicalWorkspacePreservesPersistentState(){
        QTemporaryDir data;const auto workspace=data.filePath("workspace");QVERIFY(QDir().mkpath(workspace+"/alias"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"same-workspace",data.path());host.setWorkspace(workspace);
        auto result=run(host,"globalThis.persistentValue=42;return persistentValue;");QVERIFY2(!result.error,qPrintable(result.output));
        QSignalSpy resets(host.sandbox(),&SandboxHost::contextReset);host.setWorkspace(workspace+"/alias/..");host.setWorkspace(workspace);
        QCOMPARE(resets.count(),0);result=run(host,"return persistentValue;");QVERIFY2(!result.error,qPrintable(result.output));QCOMPARE(resultValue(result),"42");
    }
    void invalidWorkspaceDoesNotSilentlyReplaceAcceptedRoot(){
        QTemporaryDir data;const auto workspace=data.filePath("workspace");QVERIFY(QDir().mkpath(workspace));QVERIFY(writeFixtureFile(workspace+"/identity.txt","accepted root"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"invalid-workspace",data.path());host.setWorkspace(workspace);
        QVERIFY(!run(host,"globalThis.saved=7;return saved;").error);QSignalSpy resets(host.sandbox(),&SandboxHost::contextReset);
        host.setWorkspace(data.filePath("does-not-exist"));QCOMPARE(resets.count(),0);
        const auto result=run(host,"return {saved,identity:await fsPromises.readFile('ws1/identity.txt','utf8')};");QVERIFY2(!result.error,qPrintable(result.output));
        const auto value=QJsonDocument::fromJson(resultValue(result).toUtf8()).object();QCOMPARE(value["saved"].toInt(),7);QCOMPARE(value["identity"].toString(),"accepted root");
    }
    void contextResetCanDeleteHostFromActualAppDeactivation(){
        QTemporaryDir data;
        BrowserToolBridge bridge;AgentEngine engine;
        auto* host=new AgentToolHost(&engine,&bridge,"delete-on-app-close",data.path());
        QPointer<AgentToolHost> guard(host);QVERIFY(installFixture(host->apps()->agentAppsDirectory()));connectTabs(*host,bridge);
        const auto opened=run(*host,"return await API.openApp('demo');");QVERIFY2(!opened.error,qPrintable(opened.output));
        QTRY_VERIFY_WITH_TIMEOUT(host->apps()->activePage()&&host->apps()->activePage()->isReady(),15000);
        const QPointer<MiniAppPage> page=host->apps()->activePage();bool deactivated=false;
        connect(host->apps(),&MiniAppHost::activeAppChanged,this,[&]{
            deactivated=true;delete host;
        });
        // Deletion happens inside AgentToolHost's contextReset cleanup, rather
        // than after reset returns. Both nested hosts must stop touching members.
        host->sandbox()->resetAgent("delete-on-app-close");
        QVERIFY(deactivated);QVERIFY(guard.isNull());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(page.isNull());
    }
    void synchronousBrowserCancellationCanDeleteItsHost(){
        QTemporaryDir data;BrowserToolBridge bridge;AgentEngine engine;
        auto* host=new AgentToolHost(&engine,&bridge,"delete-on-cancel",data.path());QPointer<AgentToolHost> guard(host);
        QString rpc;int completions=0;
        connect(&bridge,&BrowserToolBridge::completed,this,[&](const QString& id,const QString&,bool error){
            if(id!=rpc)return;
            QVERIFY(error);++completions;delete host;
        });
        connect(host->sandbox(),&SandboxHost::browserRequest,this,[&](const QString& id){
            rpc=id;
            // The actual worker request was registered by the host; cancellation
            // occurs before the bridge's deferred tab lookup/dispatch.
            emit engine.externalToolCancellationRequested("cancel-delete");
        });
        host->sandbox()->execute("cancel-delete","executeSandboxJs",{{"explanation","Cancel pending browser"},{"script","return await API.sendCDP('pending-tab','Runtime.evaluate',{expression:'42'});"}});
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),5000);QVERIFY(!rpc.isEmpty());QCOMPARE(completions,1);
        QTest::qWait(30);QCOMPARE(completions,1);
    }
    void idleAppCallbackCompletesThroughActualHost(){
        QTemporaryDir data;
        BrowserToolBridge bridge;AgentEngine engine;AgentToolHost host(&engine,&bridge,"app-owner",data.path());
        QVERIFY(installFixture(host.apps()->agentAppsDirectory()));connectTabs(host,bridge);
        auto result=run(host,"globalThis.seen=[];globalThis.off=API.onMessage('demo',async msg=>{seen.push(msg);if(msg.type==='pong')await API.sendMessage('demo',{type:'render',value:msg.value});});return await API.openApp('demo');");
        QVERIFY2(!result.error,qPrintable(result.output));QTRY_VERIFY_WITH_TIMEOUT(host.apps()->activePage()&&host.apps()->activePage()->isReady(),15000);
        QSignalSpy received(host.apps(),&MiniAppHost::messageReceived);QString error;
        QVERIFY(host.apps()->activePage()->postMessage(QJsonObject{{"type","ping"},{"value","BACKGROUND_HOST_58319"}},&error));
        QTRY_VERIFY_WITH_TIMEOUT(observed(received,"rendered"),15000);
        result=run(host,"return seen;");QVERIFY2(!result.error,qPrintable(result.output));QVERIFY(result.output.contains("BACKGROUND_HOST_58319"));
        result=run(host,"off();return seen.length;");QVERIFY(!result.error);const auto before=result.output;
        QVERIFY(host.apps()->activePage()->postMessage(QJsonObject{{"type","ping"},{"value","UNSUBSCRIBED"}},&error));QTest::qWait(200);
        result=run(host,"return seen.length;");QCOMPARE(result.output,before);
        host.sandbox()->resetAgent(host.agentId());QVERIFY(!host.apps()->activePage());
    }
    void otherAgentCannotAccessAppThroughDirectOrSandboxBrowserTools(){
        QTemporaryDir data;
        BrowserToolBridge bridge;AgentEngine ownerEngine,otherEngine;
        AgentToolHost owner(&ownerEngine,&bridge,"owner",data.path()),other(&otherEngine,&bridge,"other",data.path());
        QVERIFY(installFixture(owner.apps()->agentAppsDirectory()));QVERIFY(installFixture(other.apps()->agentAppsDirectory()));connectTabs(owner,bridge);connectTabs(other,bridge);
        auto result=run(owner,"return await API.openApp('demo');");QVERIFY2(!result.error,qPrintable(result.output));
        QTRY_VERIFY_WITH_TIMEOUT(owner.apps()->activePage()&&owner.apps()->activePage()->isReady(),15000);const auto tab=owner.apps()->activeTabId();
        QSignalSpy requests(owner.apps()->activePage()->browser(),&BrowserWidget::devToolsRequestCompleted);
        result=run(other,"return await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:'document.body.innerHTML',returnByValue:true});");
        QVERIFY(result.error);QVERIFY(!result.output.contains("<body>"));QCOMPARE(requests.size(),0);
        QSignalSpy completed(&bridge,&BrowserToolBridge::completed);
        emit otherEngine.externalToolRequested("host-direct-list","getBrowserTabs",{});QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,3000);
        QVERIFY(!completed.first()[1].toString().contains(tab));completed.clear();
        emit otherEngine.externalToolRequested("host-direct-cdp","sendCDP",{{"tabId",tab},{"method","Runtime.evaluate"},{"params",QJsonObject{{"expression","42"}}}});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,3000);QVERIFY(completed.first()[2].toBool());QCOMPARE(requests.size(),0);
        result=run(other,"return await API.openApp('demo');");QVERIFY2(!result.error,qPrintable(result.output));
        QTRY_VERIFY_WITH_TIMEOUT(other.apps()->activePage()&&other.apps()->activePage()->isReady(),15000);
        const QPointer<MiniAppPage> survivor=other.apps()->activePage();owner.sandbox()->resetAgent("owner");QVERIFY(!owner.apps()->activePage());QVERIFY(survivor);QCOMPARE(other.apps()->activePage(),survivor.data());
    }
};
QTEST_MAIN(RuntimeAppTests)
#include "test_runtime_apps.moc"
