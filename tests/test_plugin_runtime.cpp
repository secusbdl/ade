#include "support/PluginTransport.h"
#include "plugins/PluginCatalog.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

using namespace ade;
using namespace ade::test;
namespace {
QString literal(const QString& text) {
    const auto json=QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1,json.size()-2));
}
bool waitFor(const std::function<bool()>& predicate,int timeout=10000) {
    QElapsedTimer elapsed;elapsed.start();
    while(elapsed.elapsed()<timeout){if(predicate())return true;QTest::qWait(5);}
    return predicate();
}
QByteArray readFile(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
struct PluginCase {QString id,field,base,helper,path,header,prefix;};
PluginCase plugin(const QString& id,const QString& region={}) {
    if(id=="github")return {id,"token","https://api.github.com","ghGet","/user","Authorization","Bearer "};
    if(id=="supabase")return {id,"accessToken","https://api.supabase.com/v1","sbGet","/projects","Authorization","Bearer "};
    if(id=="posthog")return {id,"personalApiKey",region=="eu"?"https://eu.posthog.com":"https://us.posthog.com","phGet","/api/projects/","Authorization","Bearer "};
    if(id=="vercel")return {id,"token","https://api.vercel.com","vercelGet","/v2/user","Authorization","Bearer "};
    return {"figma","token","https://api.figma.com/v1","figmaGet","/me","X-Figma-Token",{}};
}
QJsonObject preferences(const QString& disabled={}) {
    QJsonObject plugins;for(const auto& item:PluginCatalog::definitions())plugins[item.id]=QJsonObject{{"enabled",item.id!=disabled}};
    return {{"plugins",plugins}};
}
QString pinnedScript(const PluginCase& item) {
    const auto file=QFINDTESTDATA(qPrintable("../assets/plugins/"+item.id+"/SKILL.md"));
    const auto text=QString::fromUtf8(readFile(file));
    const QRegularExpression fence("```js\\s*\\n([\\s\\S]*?)\\n```");
    auto matches=fence.globalMatch(text);QString authentication,requests;
    while(matches.hasNext()){
        auto code=matches.next().captured(1);
        if(authentication.isEmpty()&&code.contains("const cred = await API.getCredential("))authentication=code;
        if(code.contains("async function "+item.helper+"(")){requests=code;break;}
    }
    if(authentication.isEmpty()||requests.isEmpty())return {};
    if(item.id=="posthog"&&item.base.contains("eu.posthog"))requests.replace("https://us.posthog.com","https://eu.posthog.com");
    return authentication+'\n'+requests+"\nglobalThis.savedPluginCredential=cred;console.log(JSON.stringify(cred));"
        "const result=await "+item.helper+'('+literal(item.path)+");return {result,credential:cred};";
}
QString savedFetch(const PluginCase& item,const QString& destination={}) {
    return "return await (await fetch("+literal(destination.isEmpty()?item.base+item.path:destination)
        +",{headers:{"+literal(item.header)+":"+literal(item.prefix)+"+savedPluginCredential["+literal(item.field)+"]}})).json();";
}
QByteArray sseEvent(const QJsonObject& object){return "data: "+QJsonDocument(object).toJson(QJsonDocument::Compact)+"\n\n";}
class ModelServer final:public QTcpServer {
public:
    QList<QByteArray> responses;
    QList<QJsonObject> requests;
    int serial=0;
    ModelServer(){
        listen(QHostAddress::LocalHost);
        connect(this,&QTcpServer::newConnection,this,[this]{while(auto* socket=nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                const auto data=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",data);
                const auto end=data.indexOf("\r\n\r\n");if(end<0)return;
                int length=0;for(const auto& line:data.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(data.size()<end+4+length)return;
                socket->setProperty("handled",true);requests.append(QJsonDocument::fromJson(data.mid(end+4,length)).object());
                const auto response=responses.isEmpty()?QByteArray("data: [DONE]\n\n"):responses.takeFirst();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(response.size())+"\r\nConnection: close\r\n\r\n"+response);socket->disconnectFromHost();
            });
        }});
    }
    void plan(const QString& script){
        const auto args=QString::fromUtf8(QJsonDocument(QJsonObject{{"explanation","Synthetic plugin runtime fixture"},{"script",script}}).toJson(QJsonDocument::Compact));
        const QJsonObject call{{"index",0},{"id","plugin-call-"+QString::number(++serial)},{"type","function"},{"function",QJsonObject{{"name","executeSandboxJs"},{"arguments",args}}}};
        const QJsonObject toolChoice{{"delta",QJsonObject{{"tool_calls",QJsonArray{call}}}},{"finish_reason","tool_calls"}};
        const QJsonObject finalChoice{{"delta",QJsonObject{{"content","Synthetic plugin fixture finished."}}},{"finish_reason","stop"}};
        responses.append(sseEvent(QJsonObject{{"choices",QJsonArray{toolChoice}}})+"data: [DONE]\n\n");
        responses.append(sseEvent(QJsonObject{{"choices",QJsonArray{finalChoice}}})+"data: [DONE]\n\n");
    }
    ProviderConfig provider()const{return {"model","Synthetic model",QString("http://127.0.0.1:%1/v1").arg(serverPort()),"FAKE_MODEL_PROVIDER_ONLY_6239","fixture-model","openai"};}
};
struct Turn {bool completed=false,error=true;QString output;};
class Runtime {
public:
    QTemporaryDir data;
    ModelServer model;
    PluginTransport transport;
    AgentEngine engine;
    std::unique_ptr<AgentToolHost> host;
    PluginCatalog catalog;
    QString failure;
    QMap<QString,QString> tokens;
    QStringList allTokens;
    QSignalSpy finished{&engine,&AgentEngine::finished},tools{&engine,&AgentEngine::toolFinished};
    std::unique_ptr<QSignalSpy> workerOutput,console,networkRequests;
    Runtime(){
        for(const auto& definition:PluginCatalog::definitions())tokens[definition.id]="FAKE_PLUGIN_RUNTIME_"+definition.id+"_7f3b91";
        allTokens=tokens.values();allTokens.append(model.provider().apiKey);
        engine.setProvider(model.provider());engine.setAlwaysAllow(true);
        host=std::make_unique<AgentToolHost>(&engine,nullptr,"plugin-chat",data.path(),nullptr,&transport);
        const auto worker=qEnvironmentVariable("ADE_TEST_WORKER");if(!worker.isEmpty())host->sandbox()->setWorkerExecutable(worker);
        workerOutput=std::make_unique<QSignalSpy>(host->sandbox(),&SandboxHost::output);
        console=std::make_unique<QSignalSpy>(host->sandbox(),&SandboxHost::consoleMessage);
        networkRequests=std::make_unique<QSignalSpy>(host->sandbox(),&SandboxHost::networkRequest);
    }
    bool save(){
        QList<ProviderConfig> records;
        for(auto it=tokens.cbegin();it!=tokens.cend();++it){ProviderConfig record;record.id=it.key();record.name=it.key();record.protocol="credential";record.apiKey=it.value();record.baseUrl="https://ignored-stored-origin.invalid";records.append(record);}
        return ProviderStore::save(data.filePath("plugin-credentials.json"),records,&failure)
            &&ProviderStore::save(data.filePath("providers.json"),{model.provider()},&failure);
    }
    bool configure(const QString& disabled={}){return catalog.configureAgent(*host->network(),host->agentId(),data.path(),preferences(disabled),&failure);}
    bool init(){return data.isValid()&&model.isListening()&&save()&&configure();}
    void begin(const QString& script){model.plan(script);engine.submit("Run the synthetic plugin fixture.");}
    Turn run(const QString& script){
        const auto priorFinished=finished.size(),priorTools=tools.size();begin(script);
        Turn result;result.completed=waitFor([&]{return finished.size()==priorFinished+1&&!engine.isBusy();});
        if(tools.size()==priorTools+1){result.output=tools.last()[1].toString();result.error=tools.last()[2].toBool();}
        return result;
    }
    QString handle(const PluginCase& item){return host->network()->credential(host->agentId(),item.id+"-pat").toObject()[item.field].toString();}
    QByteArray observableBytes()const{
        QByteArray bytes=QJsonDocument(engine.messages()).toJson()+QJsonDocument(engine.displayMessages()).toJson();
        for(const auto& request:model.requests)bytes+=QJsonDocument(request).toJson();
        for(const auto& row:tools)bytes+=row[1].toString().toUtf8();
        for(const auto& row:*workerOutput)bytes+=row[1].toString().toUtf8();
        for(const auto& row:*console)bytes+=row[2].toString().toUtf8();
        for(const auto& row:*networkRequests)bytes+=QJsonDocument(row[2].toJsonObject()).toJson();
        return bytes;
    }
    bool hasNoRawTokens()const{const auto visible=observableBytes();for(const auto& token:allTokens)if(visible.contains(token.toUtf8()))return false;return true;}
};
}

class PluginRuntimeTest:public QObject {
    Q_OBJECT
private slots:
    void unrelatedPluginChangesPreserveActiveWorkerFetch_data(){
        QTest::addColumn<QString>("change");
        for(const auto* name:{"rotate-vercel","remove-vercel","resave","disable-figma"})
            QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void unrelatedPluginChangesPreserveActiveWorkerFetch(){
        QFETCH(QString,change);Runtime runtime;QVERIFY2(runtime.init(),qPrintable(runtime.failure));
        const auto item=plugin("github");auto result=runtime.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);
        const auto originalHandle=runtime.handle(item);const auto priorFinished=runtime.finished.size(),priorTools=runtime.tools.size();
        ReplyPlan held;held.hold=true;runtime.transport.plans.append(held);runtime.begin(savedFetch(item));
        QVERIFY(waitFor([&]{return runtime.transport.requests.size()==2;}));QVERIFY(runtime.engine.isBusy());
        const auto pending=runtime.transport.replies.last();QVERIFY(pending);
        if(change=="rotate-vercel"){
            runtime.tokens["vercel"]="FAKE_UNRELATED_VERCEL_ROTATED_8215";runtime.allTokens.append(runtime.tokens["vercel"]);
        }else if(change=="remove-vercel")runtime.tokens.remove("vercel");
        if(change!="disable-figma")QVERIFY(runtime.save());
        QVERIFY2(runtime.configure(change=="disable-figma"?"figma":QString()),qPrintable(runtime.failure));
        QCOMPARE(runtime.handle(item),originalHandle);QVERIFY(runtime.engine.isBusy());QCOMPARE(runtime.tools.size(),priorTools);
        QVERIFY(pending&&!pending->isFinished());pending->complete(ReplyPlan{});
        QVERIFY(waitFor([&]{return runtime.finished.size()==priorFinished+1&&!runtime.engine.isBusy();}));
        QCOMPARE(runtime.tools.size(),priorTools+1);QVERIFY2(!runtime.tools.last()[2].toBool(),qPrintable(runtime.tools.last()[1].toString()));
        result=runtime.run(savedFetch(item));QVERIFY2(result.completed&&!result.error,qPrintable(result.output));
        QCOMPARE(runtime.handle(item),originalHandle);QVERIFY(runtime.hasNoRawTokens());
    }
    void encryptedCatalogToPinnedSkillToNativeFetch_data(){
        QTest::addColumn<QString>("id");QTest::addColumn<QString>("region");
        for(const auto& id:QStringList{"github","supabase","posthog","vercel","figma"})QTest::newRow(qPrintable(id))<<id<<QString();
        QTest::newRow("posthog-eu")<<QString("posthog")<<QString("eu");
    }
    void encryptedCatalogToPinnedSkillToNativeFetch(){
        QFETCH(QString,id);QFETCH(QString,region);Runtime runtime;QVERIFY2(runtime.init(),qPrintable(runtime.failure));const auto item=plugin(id,region);
        const auto bytes=readFile(runtime.data.filePath("plugin-credentials.json"));QVERIFY(!bytes.isEmpty());for(const auto& token:runtime.allTokens)QVERIFY(!bytes.contains(token.toUtf8()));
        const auto script=pinnedScript(item);QVERIFY2(!script.isEmpty(),qPrintable(id));const auto turn=runtime.run(script);QVERIFY(turn.completed);QVERIFY2(!turn.error,qPrintable(turn.output));
        QCOMPARE(runtime.transport.requests.size(),1);const auto request=runtime.transport.requests.first().request;
        QCOMPARE(request.url(),QUrl(item.base+item.path));QCOMPARE(request.rawHeader(item.header.toLatin1()),(item.prefix+runtime.tokens[id]).toUtf8());
        if(id=="github"){QCOMPARE(request.rawHeader("Accept"),QByteArray("application/vnd.github+json"));QCOMPARE(request.rawHeader("X-GitHub-Api-Version"),QByteArray("2022-11-28"));}
        const auto handle=runtime.handle(item);QVERIFY(handle.startsWith("{{CRED:"+id+"-pat:"));QVERIFY(turn.output.contains(handle));QVERIFY(turn.output.contains("plugin-runtime-ok"));
        QVERIFY(!runtime.console->isEmpty());QVERIFY(runtime.console->last()[2].toString().contains(handle));QCOMPARE(runtime.networkRequests->size(),1);
        QVERIFY(QJsonDocument(runtime.networkRequests->first()[2].toJsonObject()).toJson().contains(handle.toUtf8()));
        QVERIFY(runtime.hasNoRawTokens());QCOMPARE(runtime.model.requests.size(),2);
    }
    void disableAndRotationInvalidateWorkerHeldHandles_data(){
        QTest::addColumn<QString>("id");for(const auto& id:QStringList{"github","supabase","posthog","vercel","figma"})QTest::newRow(qPrintable(id))<<id;
    }
    void disableAndRotationInvalidateWorkerHeldHandles(){
        QFETCH(QString,id);Runtime runtime;QVERIFY2(runtime.init(),qPrintable(runtime.failure));const auto item=plugin(id);
        auto result=runtime.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);const auto oldHandle=runtime.handle(item);
        QVERIFY(runtime.configure());QCOMPARE(runtime.handle(item),oldHandle);
        ReplyPlan held;held.hold=true;runtime.transport.plans.append(held);const auto priorFinished=runtime.finished.size();runtime.begin(savedFetch(item));
        QVERIFY(waitFor([&]{return runtime.transport.requests.size()==2;}));QVERIFY(runtime.engine.isBusy());QVERIFY(runtime.configure(id));
        QVERIFY(waitFor([&]{return runtime.finished.size()==priorFinished+1&&!runtime.engine.isBusy();}));QVERIFY(runtime.tools.last()[2].toBool());QVERIFY(runtime.handle(item).isEmpty());
        const auto count=runtime.transport.requests.size();result=runtime.run("return {credential:await API.getCredential("+literal(id+"-pat")+")};");QVERIFY(result.completed&&!result.error);QVERIFY(result.output.contains("null"));
        result=runtime.run(savedFetch(item));QVERIFY(result.completed&&result.error);QCOMPARE(runtime.transport.requests.size(),count);
        QVERIFY(runtime.configure());QVERIFY(runtime.handle(item)!=oldHandle);result=runtime.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);const auto beforeRotation=runtime.handle(item);
        runtime.tokens[id]="FAKE_PLUGIN_ROTATED_"+id+"_93afd7";runtime.allTokens.append(runtime.tokens[id]);QVERIFY(runtime.save());QVERIFY(runtime.configure());QVERIFY(runtime.handle(item)!=beforeRotation);
        const auto afterRotationCount=runtime.transport.requests.size();result=runtime.run(savedFetch(item));QVERIFY(result.completed&&result.error);QCOMPARE(runtime.transport.requests.size(),afterRotationCount);
        result=runtime.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);QCOMPARE(runtime.transport.requests.last().request.rawHeader(item.header.toLatin1()),(item.prefix+runtime.tokens[id]).toUtf8());
        QVERIFY(runtime.hasNoRawTokens());
    }
    void originAndReflectionBoundariesUseTheRealRegistry(){
        Runtime runtime;QVERIFY2(runtime.init(),qPrintable(runtime.failure));const auto item=plugin("github");auto result=runtime.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);
        const auto count=runtime.transport.requests.size();result=runtime.run(savedFetch(item,"https://api.figma.com/v1/me"));QVERIFY(result.completed&&result.error);QCOMPARE(runtime.transport.requests.size(),count);
        ReplyPlan reflected;reflected.body=QJsonDocument(QJsonObject{{"echo",runtime.tokens["github"]}}).toJson();runtime.transport.plans.append(reflected);
        result=runtime.run(savedFetch(item));QVERIFY(result.completed&&result.error);QVERIFY(result.output.contains("protected credential"));QVERIFY(runtime.hasNoRawTokens());
        ReplyPlan redirect;redirect.status=302;redirect.redirect=QUrl("https://api.figma.com/v1/me");redirect.headers.append({"Location",redirect.redirect.toEncoded()});redirect.body="{}";runtime.transport.plans.append(redirect);
        const auto redirects=runtime.transport.requests.size();result=runtime.run("const r=await fetch('https://api.github.com/user',{headers:{Authorization:'Bearer '+savedPluginCredential.token}});return {status:r.status,redirected:r.redirected};");QVERIFY(result.completed&&!result.error);QVERIFY(result.output.contains("302"));QCOMPARE(runtime.transport.requests.size(),redirects+1);QVERIFY(runtime.hasNoRawTokens());
    }
    void handlesCannotCrossActualAgentHosts(){
        Runtime first;QVERIFY(first.init());const auto item=plugin("github");auto result=first.run(pinnedScript(item));QVERIFY(result.completed&&!result.error);const auto handle=first.handle(item);
        Runtime second;QVERIFY(second.init());const auto forged="globalThis.savedPluginCredential={token:"+literal(handle)+"};"+savedFetch(item);result=second.run(forged);
        QVERIFY(result.completed&&result.error);QVERIFY(second.transport.requests.isEmpty());QVERIFY(first.hasNoRawTokens());QVERIFY(second.hasNoRawTokens());
    }
    void borrowedTransportDestructionSettlesPendingRuntime(){
        Runtime runtime;QVERIFY(runtime.init());const auto item=plugin("github");auto manager=std::make_unique<PluginTransport>();
        runtime.host.reset();runtime.host=std::make_unique<AgentToolHost>(&runtime.engine,nullptr,"plugin-chat",runtime.data.path(),nullptr,manager.get());
        const auto worker=qEnvironmentVariable("ADE_TEST_WORKER");if(!worker.isEmpty())runtime.host->sandbox()->setWorkerExecutable(worker);
        QVERIFY(runtime.catalog.configureAgent(*runtime.host->network(),runtime.host->agentId(),runtime.data.path(),preferences(),&runtime.failure));
        ReplyPlan held;held.hold=true;manager->plans.append(held);runtime.begin(pinnedScript(item));QVERIFY(waitFor([&]{return manager->requests.size()==1;}));manager.reset();
        QVERIFY(waitFor([&]{return !runtime.engine.isBusy();}));QCOMPARE(runtime.tools.size(),1);QVERIFY(runtime.tools.last()[2].toBool());
        const auto next=runtime.run(pinnedScript(item));QVERIFY(next.completed&&next.error);QVERIFY(next.output.contains("transport"));
    }
    void borrowedTransportOwnershipAndDeletionReentry(){
        PluginTransport manager;QPointer<PluginTransport> preserved(&manager);{NetworkBroker broker(&manager,nullptr);}QVERIFY(preserved);
        auto* disposable=new PluginTransport;ReplyPlan held;held.hold=true;disposable->defaultPlan=held;
        QPointer<NetworkBroker> broker=new NetworkBroker(disposable,nullptr);int completed=0;
        connect(broker,&NetworkBroker::completed,this,[&](const QString&,const QJsonObject&,const QString& error){QVERIFY(!error.isEmpty());++completed;delete broker.data();});
        broker->fetch("first","agent",{{"url","https://fixture.invalid/first"}});broker->fetch("second","agent",{{"url","https://fixture.invalid/second"}});QCOMPARE(disposable->requests.size(),2);
        delete disposable;QVERIFY(broker.isNull());QCOMPARE(completed,1);
    }
    void abortObserverMayDestroyBroker(){
        PluginTransport transport;ReplyPlan held;held.hold=true;transport.defaultPlan=held;
        QPointer<NetworkBroker> broker=new NetworkBroker(&transport,nullptr);QSignalSpy completed(broker,&NetworkBroker::completed);
        broker->fetch("cancel-me","agent",{{"url","https://fixture.invalid/held"}});QCOMPARE(transport.replies.size(),1);
        const auto reply=transport.replies.first();connect(reply,&QNetworkReply::errorOccurred,this,[&]{delete broker.data();});
        broker->cancel("cancel-me");QVERIFY(broker.isNull());QCOMPARE(completed.size(),0);QTRY_VERIFY(reply.isNull());
    }
};
QTEST_MAIN(PluginRuntimeTest)
#include "test_plugin_runtime.moc"
