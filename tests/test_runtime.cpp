#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace ade;

namespace {
QByteArray event(const QJsonObject& value) { return "data: " + QJsonDocument(value).toJson(QJsonDocument::Compact) + "\n\n"; }
QByteArray tool(const QString& script) {
    const QJsonObject args{{"explanation", "Test native integration"}, {"script", script}};
    const QJsonObject function{{"name","executeSandboxJs"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}};
    const QJsonObject call{{"index",0},{"id","call_runtime"},{"type","function"},{"function",function}};
    const QJsonObject choice{{"delta",QJsonObject{{"tool_calls",QJsonArray{call}}}},{"finish_reason","tool_calls"}};
    return event(QJsonObject{{"choices",QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
QByteArray finalText() {
    const QJsonObject choice{{"delta",QJsonObject{{"content","Finished fixture work."}}},{"finish_reason","stop"}};
    return event(QJsonObject{{"choices",QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
QString quote(const QString& value) {
    const auto array=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1,array.size()-2));
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }

class RuntimeFixture final : public QObject {
public:
    QTcpServer server;
    QHash<QString,QList<QByteArray>> responses;
    QHash<QString,QList<QJsonObject>> modelRequests;
    QList<QByteArray> pageHeaders;
    QList<QByteArray> networkHeaders;
    RuntimeFixture() {
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto* socket=server.nextPendingConnection()) {
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                    if(socket->property("handled").toBool()){socket->readAll();return;}
                    const QByteArray bytes=socket->property("request").toByteArray()+socket->readAll();
                    socket->setProperty("request",bytes);
                    const auto headerEnd=bytes.indexOf("\r\n\r\n");if(headerEnd<0)return;
                    const auto headers=bytes.left(headerEnd);
                    int length=0;
                    for(const auto& line:headers.split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                    if(bytes.size()<headerEnd+4+length)return;
                    socket->setProperty("handled",true);
                    const QByteArray target=headers.split(' ').value(1);
                    QByteArray content,type;
                    if(target.contains("/chat/completions")) {
                        const QString route=QString::fromUtf8(target.split('/').value(1));
                        modelRequests[route].append(QJsonDocument::fromJson(bytes.mid(headerEnd+4,length)).object());
                        content=responses[route].isEmpty()?finalText():responses[route].takeFirst();
                        type="text/event-stream";
                    } else if(target=="/credential-fixture") {
                        networkHeaders.append(headers);content="{\"value\":\"fixture-network-ok\"}";type="application/json";
                    } else if(target=="/network-slow") {
                        networkHeaders.append(headers);return;
                    } else {
                        pageHeaders.append(headers);
                        content="<!doctype html><html><head><title>Native runtime fixture</title></head>"
                            "<body style='background:#112233;color:white'><h1 id='fixture'>native integration</h1>"
                            "<script>console.log('runtime fixture ready')</script></body></html>";
                        type="text/html; charset=utf-8";
                    }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "+type+"\r\nConnection: close\r\nContent-Length: "+QByteArray::number(content.size())+"\r\n\r\n"+content);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl page()const{return QUrl(QStringLiteral("http://127.0.0.1:%1/page").arg(server.serverPort()));}
    ProviderConfig provider(const QString& route)const {
        return {route,"Native fixture",QStringLiteral("http://127.0.0.1:%1/%2/v1").arg(server.serverPort()).arg(route),"runtime-fixture-secret","fixture-model","openai"};
    }
};
}

class RuntimeTests final : public QObject {
    Q_OBJECT
    std::unique_ptr<RuntimeFixture> fixture;
    std::unique_ptr<QTemporaryDir> temporary;
    void prepare(BrowserWidget& browser) {
        browser.setAttribute(Qt::WA_DontShowOnScreen);
        browser.resize(800,600);
        browser.setProfileDirectory(temporary->filePath("browser"));
    }
    bool load(BrowserWidget& browser) {
        QSignalSpy navigation(&browser,&BrowserWidget::loadingChanged);
        browser.navigate(fixture->page());browser.show();
        QElapsedTimer elapsed;elapsed.start();
        while(elapsed.elapsed()<15000) {
            for(const auto& row:navigation)if(!row.at(0).toBool()&&browser.currentUrl()==fixture->page())return true;
            QTest::qWait(20);
        }
        return false;
    }
    void configure(AgentEngine& engine,AgentToolHost& host,const QString& route) {
        const QString workspace=temporary->filePath("workspace-"+route);
        QDir().mkpath(workspace);engine.setWorkspace(workspace);host.setWorkspace(workspace);
        engine.setProvider(fixture->provider(route));engine.setAlwaysAllow(true);
    }
private slots:
    void init() {
        fixture=std::make_unique<RuntimeFixture>();temporary=std::make_unique<QTemporaryDir>();
        QVERIFY(fixture->server.isListening());QVERIFY(temporary->isValid());
    }
    void cleanup(){fixture.reset();temporary.reset();}

    void contextReadIsScopedAndReachesProvider(){
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"context",temporary->path());configure(engine,host,"context");
        const QString skills=engine.workspace()+"/.agents/skills/fixture";QVERIFY(QDir().mkpath(skills));
        QFile instruction(engine.workspace()+"/AGENTS.md");QVERIFY(instruction.open(QIODevice::WriteOnly));instruction.write("Follow CONTEXT_ROOT_71.");instruction.close();
        QFile skill(skills+"/SKILL.md");QVERIFY(skill.open(QIODevice::WriteOnly));skill.write("---\nname: fixture\ndescription: Fixture skill\n---\nCONTEXT_SKILL_BODY_82");skill.close();
        ContextOptions options;options.workspace=engine.workspace();const auto snapshot=host.configureContext(options);QCOMPARE(snapshot.skills.size(),1);
        const QJsonObject args{{"path",snapshot.skills.first().contentPath}};
        const QJsonObject call{{"index",0},{"id","call_context"},{"type","function"},{"function",QJsonObject{{"name","readContext"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}};
        const auto response=::event(QJsonObject{{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"tool_calls",QJsonArray{call}}}},{"finish_reason","tool_calls"}}}}})+"data: [DONE]\n\n";
        fixture->responses["context"]={response,finalText()};QSignalSpy completed(&engine,&AgentEngine::finished),results(&engine,&AgentEngine::toolFinished);
        engine.submit("Use the fixture skill.");QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,10000);QCOMPARE(results.size(),1);
        QVERIFY(!results.first().at(2).toBool());QVERIFY(results.first().at(1).toString().contains("CONTEXT_SKILL_BODY_82"));
        QCOMPARE(fixture->modelRequests["context"].size(),2);
        QVERIFY(QJsonDocument(fixture->modelRequests["context"].first()).toJson().contains("CONTEXT_ROOT_71"));
        QVERIFY(QJsonDocument(fixture->modelRequests["context"].last()).toJson().contains("CONTEXT_SKILL_BODY_82"));
    }

    void nativeNetworkBrokerKeepsCredentialOutOfWorkerOutput(){
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"network",temporary->path());configure(engine,host,"network");
        const QString origin=QString("http://127.0.0.1:%1").arg(fixture->server.serverPort());QString error;
        QVERIFY(host.network()->registerCredentialType({"fixture-token",{"token"},{origin},{}},&error));
        const QString secret="runtime-credential-not-real-9123";
        QVERIFY(host.network()->setCredential("fixture-token",{{"token",secret}},&error));host.network()->setAllowedCredentialTypes(host.agentId(),{"fixture-token"});
        const QString script="const cred=await API.getCredential('fixture-token');const response=await fetch("+quote(origin+"/credential-fixture")+",{headers:{Authorization:'Bearer '+cred.token}});return {status:response.status,body:await response.json(),opaque:cred.token.startsWith('{{CRED:')};";
        fixture->responses["network"]={tool(script),finalText()};QSignalSpy finished(&engine,&AgentEngine::finished),results(&engine,&AgentEngine::toolFinished);
        engine.submit("Use the isolated network fixture.");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);QCOMPARE(results.size(),1);
        QVERIFY2(!results.first().at(2).toBool(),qPrintable(results.first().at(1).toString()));
        QVERIFY(results.first().at(1).toString().contains("fixture-network-ok"));QCOMPARE(fixture->networkHeaders.size(),1);
        QVERIFY(fixture->networkHeaders.first().contains(secret.toUtf8()));
        QVERIFY(!QJsonDocument(engine.messages()).toJson().contains(secret.toUtf8()));
        AgentEngine other;AgentToolHost isolated(&other,nullptr,"other-network",temporary->path());
        QVERIFY(isolated.network()->credential(isolated.agentId(),"fixture-token").isNull());
    }

    void cancelPendingSandboxFetchSettlesTheBroker(){
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"network-cancel",temporary->path());configure(engine,host,"network-cancel");
        const QString url=QString("http://127.0.0.1:%1/network-slow").arg(fixture->server.serverPort());
        fixture->responses["network-cancel"]={tool("await fetch("+quote(url)+");return 'unexpected';")};
        QSignalSpy requests(host.sandbox(),&SandboxHost::networkRequest),completed(host.network(),&NetworkBroker::completed),finished(&engine,&AgentEngine::finished);
        engine.submit("Wait for the controlled response.");QTRY_COMPARE_WITH_TIMEOUT(requests.size(),1,10000);
        QTRY_COMPARE_WITH_TIMEOUT(fixture->networkHeaders.size(),1,5000);engine.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),3000);QCOMPARE(completed.size(),1);QVERIFY(!completed.first().at(2).toString().isEmpty());
        QTest::qWait(50);QCOMPARE(completed.size(),1);
    }

    void providerToSandboxToBrowserToFilesAndAttachment() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser);
        const QString tab=bridge.registerTab(&browser);QVERIFY(load(browser));
        AgentEngine engine;AgentToolHost host(&engine,&bridge,"main",temporary->path());configure(engine,host,"main");
        auto imageProvider=fixture->provider("main");imageProvider.model="gpt-5.5";engine.setProvider(imageProvider);
        const QString script="const response=await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:\"document.querySelector('#fixture').textContent\",returnByValue:true});"
            "await fsPromises.writeFile('ws1/result.txt',response.result.value+'\\r\\n');"
            "const text=await fsPromises.readFile('w1/result.txt','utf8');"
            "const image=await API.sendCDP("+quote(tab)+",'Page.captureScreenshot',{format:'png',clip:{x:0,y:0,width:200,height:120,scale:1}});"
            "const attachment=await API.createAttachment('preview.png',image.data);API.output('copied');return {text,attachment};";
        fixture->responses["main"]={tool(script),finalText()};
        QSignalSpy done(&engine,&AgentEngine::finished),failed(&engine,&AgentEngine::failed),results(&engine,&AgentEngine::toolFinished);
        QSignalSpy changed(&host,&AgentToolHost::fileChanged),attachments(&host,&AgentToolHost::attachmentCreated),cdp(host.sandbox(),&SandboxHost::browserRequest);
        engine.submit("Run the controlled integration fixture.");
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),15000);
        QCOMPARE(failed.count(),0);QCOMPARE(done.count(),1);QCOMPARE(results.count(),1);
        QVERIFY2(!results.first().at(2).toBool(),qPrintable(results.first().at(1).toString()));
        QCOMPARE(read(engine.workspace()+"/result.txt"),QByteArray("native integration\r\n"));
        QCOMPARE(changed.count(),1);QCOMPARE(attachments.count(),1);
        QVERIFY(!QImage(attachments.first().at(1).toString()).isNull());
        QCOMPARE(cdp.count(),2);
        for(const auto& call:cdp)QVERIFY(!QJsonDocument(call.at(3).toJsonObject()).toJson().contains("runtime-fixture-secret"));
        for(const auto& headers:fixture->pageHeaders)QVERIFY(!headers.contains("runtime-fixture-secret"));
        QCOMPARE(fixture->modelRequests["main"].size(),2);
        const auto definitions=fixture->modelRequests["main"].first().value("tools").toArray();
        bool advertised=false;for(const auto& definition:definitions)advertised|=definition.toObject().value("function").toObject().value("name")=="executeSandboxJs";
        QVERIFY(advertised);
        const auto messages=fixture->modelRequests["main"].last().value("messages").toArray();
        bool resultDelivered=false;for(const auto& value:messages){const auto message=value.toObject();if(message.value("role")=="tool")resultDelivered|=message.value("content").toString().contains("native integration");}
        QVERIFY(resultDelivered);
        const auto history=engine.messages();QJsonObject toolMetadata;
        for(const auto& message:history)if(message.toObject()["role"]=="tool")toolMetadata=message.toObject()["metadata"].toObject();
        QCOMPARE(toolMetadata["attachments"].toArray().size(),1);
        QCOMPARE(toolMetadata["pathReferences"].toObject().size(),1);
        QVERIFY(!QJsonDocument(history).toJson().contains("data:image/"));
        bool screenshotDelivered=false;
        for(const auto& value:messages)for(const auto& part:value.toObject()["content"].toArray())
            screenshotDelivered|=part.toObject()["type"]=="image_url";
        QVERIFY(screenshotDelivered);
    }

    void userAttachmentIsPreparedAndPersistedAsReference() {
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"user-files",temporary->path());configure(engine,host,"user-files");
        AttachmentStore store(temporary->path());QString error;
        const auto ref=store.write("user-files","user-context.txt","USER_ATTACHMENT_MARKER_7926",&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        const auto metadata=host.captureAttachments({ref.toJson()},&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        fixture->responses["user-files"]={finalText()};engine.submitWithAttachments("",metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject());
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),5000);QVERIFY(engine.lastTurnSucceeded());
        QCOMPARE(fixture->modelRequests["user-files"].size(),1);
        QVERIFY(QJsonDocument(fixture->modelRequests["user-files"].first()).toJson().contains("USER_ATTACHMENT_MARKER_7926"));
        const auto saved=engine.messages().first().toObject();QCOMPARE(saved["content"].toString(),QString());
        auto savedContext=saved["metadata"].toObject();const auto display=savedContext.take("display").toObject();QCOMPARE(savedContext,metadata);
        QCOMPARE(display["version"].toInt(),1);QVERIFY(!display["id"].toString().isEmpty());
        const QJsonArray parts{QJsonObject{{"id",display["id"].toString()+"/0"},{"type","text"},{"text",""},{"state","done"}}};QCOMPARE(display["parts"].toArray(),parts);
        const auto projected=AgentEngine::displayHistory(QJsonArray{saved});QCOMPARE(projected.size(),1);QCOMPARE(projected[0].toObject()["id"],display["id"]);QCOMPARE(projected[0].toObject()["attachments"].toArray(),metadata["attachments"].toArray());
        const auto request=QJsonDocument(fixture->modelRequests["user-files"].first()).toJson();QVERIFY(!request.contains("\"metadata\""));QVERIFY(!request.contains("\"display\""));QVERIFY(!request.contains(display["id"].toString().toUtf8()));
        QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("USER_ATTACHMENT_MARKER_7926"));
    }

    void realCdpFailureReturnsToolErrorAndModelContinues() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser);
        const QString tab=bridge.registerTab(&browser);QVERIFY(load(browser));
        AgentEngine engine;AgentToolHost host(&engine,&bridge,"failure",temporary->path());configure(engine,host,"failure");
        fixture->responses["failure"]={tool("return await API.sendCDP("+quote(tab)+",'MissingDomain.missingMethod');"),finalText()};
        QSignalSpy done(&engine,&AgentEngine::finished),results(&engine,&AgentEngine::toolFinished),failed(&engine,&AgentEngine::failed);
        engine.submit("Run a controlled failing browser method.");
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),10000);
        QCOMPARE(done.count(),1);QCOMPARE(failed.count(),0);QCOMPARE(results.count(),1);QVERIFY(results.first().at(2).toBool());
        QCOMPARE(fixture->modelRequests["failure"].size(),2);
        QVERIFY(results.first().at(1).toString().contains("Sandbox JavaScript failed"));
    }

    void twoHostsShareBrowserWithCancellationIsolation() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser);
        const QString tab=bridge.registerTab(&browser);QVERIFY(load(browser));
        AgentEngine canceled,continuing;
        AgentToolHost hostA(&canceled,&bridge,"agent-a",temporary->path()),hostB(&continuing,&bridge,"agent-b",temporary->path());
        configure(canceled,hostA,"a");configure(continuing,hostB,"b");
        fixture->responses["a"]={tool("globalThis.poison=1;await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:\"new Promise(r=>setTimeout(()=>r('late'),450))\",awaitPromise:true,returnByValue:true});await fsPromises.writeFile('late.txt','wrong');"),finalText()};
        fixture->responses["b"]={tool("globalThis.counter=41;const v=await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:\"document.title\",returnByValue:true});await fsPromises.writeFile('ok.txt',v.result.value);return ++globalThis.counter;"),finalText(),tool("return ++globalThis.counter;"),finalText()};
        QSignalSpy requests(hostA.sandbox(),&SandboxHost::browserRequest),aResults(&canceled,&AgentEngine::toolFinished),bResults(&continuing,&AgentEngine::toolFinished);
        QSignalSpy brokerCancellations(hostA.sandbox(),&SandboxHost::brokerCancellationRequested);
        canceled.submit("Start a cancellable tool.");
        QTRY_COMPARE_WITH_TIMEOUT(requests.count(),1,10000);
        continuing.submit("Run the other agent independently.");
        canceled.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!continuing.isBusy(),10000);
        QVERIFY(!canceled.isBusy());QCOMPARE(bResults.count(),1);QVERIFY(!bResults.first().at(2).toBool());
        QCOMPARE(read(continuing.workspace()+"/ok.txt"),QByteArray("Native runtime fixture"));
        QTRY_COMPARE_WITH_TIMEOUT(brokerCancellations.count(),1,3000);
        QTest::qWait(550);QVERIFY(!QFile::exists(canceled.workspace()+"/late.txt"));
        QCOMPARE(aResults.count(),1);QVERIFY(aResults.first().at(2).toBool());
        continuing.submit("Read the same agent's persistent global.");
        QTRY_VERIFY_WITH_TIMEOUT(!continuing.isBusy(),10000);
        QCOMPARE(bResults.count(),2);QVERIFY(!bResults.last().at(2).toBool());
        const auto output=QJsonDocument::fromJson(bResults.last().at(1).toString().toUtf8()).object();
        QCOMPARE(output.value("result").toString(),"43");
    }

    void destroyingOneHostCancelsOnlyItsBrokerRequests() {
        BrowserToolBridge bridge;BrowserWidget browser;prepare(browser);
        const QString tab=bridge.registerTab(&browser);QVERIFY(load(browser));
        auto engine=std::make_unique<AgentEngine>();
        auto host=std::make_unique<AgentToolHost>(engine.get(),&bridge,"destroy",temporary->path());configure(*engine,*host,"destroy");
        fixture->responses["destroy"]={tool("return await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:'new Promise(()=>{})',awaitPromise:true});")};
        QSignalSpy requests(host->sandbox(),&SandboxHost::browserRequest),browserResults(&bridge,&BrowserToolBridge::completed);
        engine->submit("Wait inside the browser.");QTRY_COMPARE_WITH_TIMEOUT(requests.count(),1,10000);
        host.reset();engine.reset();
        QTRY_COMPARE_WITH_TIMEOUT(browserResults.count(),1,3000);QVERIFY(browserResults.first().at(2).toBool());
        AgentEngine survivor;AgentToolHost next(&survivor,&bridge,"survivor",temporary->path());configure(survivor,next,"survivor");
        fixture->responses["survivor"]={tool("return (await API.sendCDP("+quote(tab)+",'Runtime.evaluate',{expression:'2+3',returnByValue:true})).result.value;"),finalText()};
        QSignalSpy results(&survivor,&AgentEngine::toolFinished);survivor.submit("Verify the browser is usable.");
        QTRY_VERIFY_WITH_TIMEOUT(!survivor.isBusy(),10000);QCOMPARE(results.count(),1);QVERIFY(!results.first().at(2).toBool());
        QCOMPARE(QJsonDocument::fromJson(results.first().at(1).toString().toUtf8()).object().value("result").toString(),"5");
    }
};
QTEST_MAIN(RuntimeTests)
#include "test_runtime.moc"
