#include "sandbox/SandboxHost.h"
#include "network/NetworkBroker.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTimer>
#include <memory>

using namespace ade;
namespace {
struct HttpRequest { QByteArray method,target,body; QMap<QByteArray,QByteArray> headers; };
struct HttpResponse { int status=200; QByteArray body="ok"; QList<QPair<QByteArray,QByteArray>> headers; bool hold=false; };
class Fixture final : public QObject {
public:
    QTcpServer listener;
    QList<HttpRequest> requests;
    std::function<HttpResponse(const HttpRequest&)> respond;
    Fixture() {
        listener.listen(QHostAddress::LocalHost);
        connect(&listener,&QTcpServer::newConnection,this,[this]{while(listener.hasPendingConnections()){
            auto* socket=listener.nextPendingConnection();
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("done").toBool()){socket->readAll();return;}
                const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
                const auto end=bytes.indexOf("\r\n\r\n");if(end<0)return;
                HttpRequest request;const auto lines=bytes.left(end).split('\n');const auto first=lines.first().trimmed().split(' ');
                request.method=first.value(0);request.target=first.value(1);
                for(int i=1;i<lines.size();++i){const auto line=lines[i].trimmed();const auto colon=line.indexOf(':');if(colon>0)request.headers[line.left(colon).toLower()]=line.mid(colon+1).trimmed();}
                const auto length=request.headers.value("content-length").toInt();if(bytes.size()<end+4+length)return;
                request.body=bytes.mid(end+4,length);socket->setProperty("done",true);requests.append(request);
                const auto response=respond?respond(request):HttpResponse{};if(response.hold)return;
                QByteArray header="HTTP/1.1 "+QByteArray::number(response.status)+" Fixture\r\nContent-Length: "+QByteArray::number(response.body.size())+"\r\nConnection: close\r\n";
                for(const auto& field:response.headers)header+=field.first+": "+field.second+"\r\n";
                socket->write(header+"\r\n"+response.body);socket->disconnectFromHost();
            });
        }});
    }
    QString url(const QString& path={}) const {return QString("http://127.0.0.1:%1").arg(listener.serverPort())+path;}
};
QString literal(const QString& value){const auto encoded=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(encoded.mid(1,encoded.size()-2));}
struct Result {bool arrived=false,error=true;QString raw,value;};
}

class SandboxNetworkTests final : public QObject {
    Q_OBJECT
    std::unique_ptr<Fixture> server;
    std::unique_ptr<NetworkBroker> broker;
    std::unique_ptr<SandboxHost> host;
    int sequence=0;
    Result run(const QString& script,const QString& id={}) {
        const QString call=id.isEmpty()?QString("network-%1").arg(++sequence):id;
        QSignalSpy done(host.get(),&SandboxHost::completed);
        host->execute(call,"executeSandboxJs",{{"explanation","Test native fetch"},{"script",script}});
        QElapsedTimer elapsed;elapsed.start();
        while(elapsed.elapsed()<7000){
            for(const auto& row:done)if(row.at(0).toString()==call){const auto raw=row.at(1).toString();return {true,row.at(2).toBool(),raw,QJsonDocument::fromJson(raw.toUtf8()).object().value("result").toString()};}
            done.wait(50);
        }
        return {false,true,"Timed out waiting for sandbox",{}};
    }
    void configureCredentials() {
        QVERIFY(broker->registerCredentialType({"fixture-pat",{"token"},{server->url()},{"project"}}));
        QVERIFY(broker->setCredential("fixture-pat",{{"token","FAKE_SANDBOX_TOKEN_39only"},{"project","fixture"}}));
        broker->setAllowedCredentialTypes("alpha",{"fixture-pat"});
        broker->setAllowedCredentialTypes("beta",{"fixture-pat"});
    }
private slots:
    void init() {
        server=std::make_unique<Fixture>();QVERIFY(server->listener.isListening());
        broker=std::make_unique<NetworkBroker>();host=std::make_unique<SandboxHost>();host->setAgentId("alpha");host->setExecutionLimits(2000,4000,5000);
        connect(host.get(),&SandboxHost::networkRequest,broker.get(),&NetworkBroker::fetch);
        connect(broker.get(),&NetworkBroker::completed,host.get(),&SandboxHost::completeNetworkRequest);
        connect(host.get(),&SandboxHost::brokerCancellationRequested,broker.get(),&NetworkBroker::cancel);
        connect(host.get(),&SandboxHost::contextReset,broker.get(),&NetworkBroker::resetAgent);
        connect(host.get(),&SandboxHost::credentialRequest,host.get(),[this](const QString& id,const QString& agent,const QString& type){QString error;const auto value=broker->credential(agent,type,&error);host->completeCredentialRequest(id,value,error);});
    }
    void cleanup(){host.reset();broker.reset();server.reset();}

    void bufferedResponsesHeadersAndBodyConsumption() {
        server->respond=[](const HttpRequest&){return HttpResponse{200,"{\"number\":42}",{{"X-Fixture","yes"},{"Content-Type","application/json"},{"Set-Cookie","private=ignored"}}};};
        const auto result=run("const r=await fetch("+literal(server->url("/json"))+");const c=r.clone();const before=r.bodyUsed;const json=await r.json();let consumed=false,immutable=false;try{await r.text();}catch(e){consumed=e instanceof TypeError;}try{r.headers.set('x','y');}catch(e){immutable=true;}return {status:r.status,statusText:r.statusText,ok:r.ok,url:r.url,redirected:r.redirected,before,after:r.bodyUsed,json,copy:await c.text(),header:r.headers.get('X-FIXTURE'),cookie:r.headers.has('set-cookie'),consumed,immutable};");
        QVERIFY2(result.arrived&&!result.error,qPrintable(result.raw));const auto value=QJsonDocument::fromJson(result.value.toUtf8()).object();
        QCOMPARE(value["status"].toInt(),200);QCOMPARE(value["statusText"].toString(),"Fixture");QVERIFY(value["ok"].toBool());QCOMPARE(value["url"].toString(),server->url("/json"));QVERIFY(!value["redirected"].toBool());QVERIFY(!value["before"].toBool());QVERIFY(value["after"].toBool());QVERIFY(value["consumed"].toBool());QVERIFY(value["immutable"].toBool());QVERIFY(!value["cookie"].toBool());QCOMPARE(value["header"].toString(),"yes");QCOMPARE(value["json"].toObject()["number"].toInt(),42);QCOMPARE(value["copy"].toString(),"{\"number\":42}");
        QCOMPARE(run("const h=new Headers([['X-Test','a'],['x-test',' b ']]);h.set('z','3');h.delete('z');const pairs=[];h.forEach((v,k)=>pairs.push([k,v]));return [h.get('X-Test'),h.has('z'),[...h],pairs];").value,"[\"a, b\",false,[[\"x-test\",\"a, b\"]],[[\"x-test\",\"a, b\"]]]");
        QVERIFY(run("new Headers({'bad name':'x'});").error);QVERIFY(run("new Headers({'x':'bad\\r\\nvalue'});").error);
    }

    void requestBodiesBinaryOffsetsAndResponseConstructors() {
        server->respond=[](const HttpRequest& r){return HttpResponse{200,r.body,{{"Content-Type","application/octet-stream"}}};};
        const auto binary=run("const bytes=new Uint8Array([9,0,255,65,9]);const request=new Request("+literal(server->url("/binary"))+",{method:'post',body:bytes.subarray(1,4),headers:new Headers({'X-Test':'binary'})});const copy=request.clone();const result=await fetch(request);let used=false;try{await fetch(request);}catch(e){used=true;}return [Array.from(await result.bytes()),request.bodyUsed,used,Array.from(new Uint8Array(await copy.arrayBuffer()))];");
        QVERIFY2(!binary.error,qPrintable(binary.raw));QCOMPARE(binary.value,"[[0,255,65],true,true,[0,255,65]]");QCOMPARE(server->requests.last().body,QByteArray::fromHex("00ff41"));QCOMPARE(server->requests.last().method,"POST");
        const auto text=run("return await (await fetch("+literal(server->url("/text"))+",{method:'POST',body:'hello ✓'})).text();");QVERIFY2(!text.error,qPrintable(text.raw));QCOMPARE(text.value,QString::fromUtf8("hello ✓"));QCOMPARE(server->requests.last().headers.value("content-type"),"text/plain;charset=UTF-8");
        const auto response=run("const r=Response.json({x:1},{status:201});const c=r.clone();const bom=new Response(new Uint8Array([239,187,191,65]));return [r.status,r.ok,r.headers.get('content-type'),await r.json(),await c.text(),await bom.text(),Response.error().status,Response.redirect('http://127.0.0.1/a').headers.get('location')];");
        QVERIFY2(!response.error,qPrintable(response.raw));QCOMPARE(response.value,"[201,true,\"application/json\",{\"x\":1},\"{\\\"x\\\":1}\",\"A\",0,\"http://127.0.0.1/a\"]");
        for(const auto& script:{"new Response('x',{status:204});","new Response('',{status:99});","new Request('http://127.0.0.1/',{method:'GET',body:'x'});","new Request('http://127.0.0.1/',{method:'POST',body:Buffer.alloc(1048577)});","new Request('http://127.0.0.1/',{credentials:'include'});"})QVERIFY2(run(script).error,script);
    }

    void httpErrorsRedirectsAndTransportErrors() {
        server->respond=[this](const HttpRequest& r){if(r.target=="/redirect")return HttpResponse{302,"redirect",{{"Location",server->url("/final").toUtf8()}}};if(r.target=="/missing")return HttpResponse{404,"missing",{}};return HttpResponse{200,"final",{}};};
        QCOMPARE(run("const r=await fetch("+literal(server->url("/missing"))+");return [r.status,r.ok,await r.text()];").value,"[404,false,\"missing\"]");
        const auto redirect=run("const r=await fetch("+literal(server->url("/redirect"))+");return [r.redirected,r.url,await r.text()];");QVERIFY2(!redirect.error,qPrintable(redirect.raw));QCOMPARE(QJsonDocument::fromJson(redirect.value.toUtf8()).array()[1].toString(),server->url("/final"));
        QCOMPARE(run("const r=await fetch("+literal(server->url("/redirect"))+",{redirect:'manual'});return [r.status,r.redirected];").value,"[302,false]");
        QVERIFY(run("await fetch("+literal(server->url("/redirect"))+",{redirect:'error'});").error);
        const QString stopped=server->url();server->listener.close();QVERIFY(run("await fetch("+literal(stopped)+");").error);
    }

    void abortIsPerFetchAndLeavesContextUsable() {
        server->respond=[](const HttpRequest& r){return r.target=="/hold"?HttpResponse{200,{}, {},true}:HttpResponse{};};
        QSignalSpy canceled(host.get(),&SandboxHost::brokerCancellationRequested);
        QCOMPARE(run("try{await fetch("+literal(server->url())+",{signal:AbortSignal.abort()});}catch(e){return e.name;}").value,"AbortError");QCOMPARE(server->requests.size(),0);
        const auto aborted=run("globalThis.survives=7;const c=new AbortController();setTimeout(()=>c.abort(),40);const held=fetch("+literal(server->url("/hold"))+",{signal:c.signal}).catch(e=>e.name);const fast=fetch("+literal(server->url())+").then(r=>r.text());return await Promise.all([held,fast]);");
        QVERIFY2(!aborted.error,qPrintable(aborted.raw));QCOMPARE(aborted.value,"[\"AbortError\",\"ok\"]");QCOMPARE(canceled.count(),1);QCOMPARE(run("return globalThis.survives;").value,"7");
        QCOMPARE(run("try{await fetch("+literal(server->url("/hold"))+",{signal:AbortSignal.timeout(25)});}catch(e){return e.name;}").value,"TimeoutError");
        QCOMPARE(run("const a=new AbortController(),b=new AbortController(),s=AbortSignal.any([a.signal,b.signal]);let n=0;s.addEventListener('abort',()=>n++,{once:true});b.abort('custom');b.abort();return [s.aborted,s.reason,n];").value,"[true,\"custom\",1]");
        const auto afterHeaders=run("const c=new AbortController();const r=await fetch("+literal(server->url())+",{signal:c.signal});c.abort();try{return await r.text();}catch(e){return e.name;}");QCOMPARE(afterHeaders.value,"AbortError");
    }

    void opaqueCredentialsAreScopedRevokedAndNeverReflected() {
        configureCredentials();Fixture other;QVERIFY(other.listener.isListening());
        const auto credential=run("globalThis.credential=await API.getCredential('fixture-pat');return credential;");QVERIFY2(!credential.error,qPrintable(credential.raw));QVERIFY(!credential.raw.contains("FAKE_SANDBOX_TOKEN"));const QString handle=QJsonDocument::fromJson(credential.value.toUtf8()).object()["token"].toString();QVERIFY(handle.startsWith("{{CRED:"));
        QCOMPARE(run("return await (await fetch("+literal(server->url())+",{headers:{Authorization:'Bearer '+credential.token}})).text();").value,"ok");QCOMPARE(server->requests.last().headers.value("authorization"),"Bearer FAKE_SANDBOX_TOKEN_39only");
        QVERIFY(run("await fetch("+literal(other.url())+",{headers:{Authorization:credential.token}});").error);QCOMPARE(other.requests.size(),0);
        host->setAgentId("beta");QVERIFY(run("await fetch("+literal(server->url())+",{headers:{Authorization:"+literal(handle)+"}});").error);host->setAgentId("alpha");
        server->respond=[](const HttpRequest& r){return HttpResponse{200,r.headers.value("authorization"),{}};};
        const auto reflected=run("return await (await fetch("+literal(server->url())+",{headers:{Authorization:credential.token}})).text();");QVERIFY(reflected.error);QVERIFY(!reflected.raw.contains("FAKE_SANDBOX_TOKEN"));
        host->resetAgent("alpha");QCOMPARE(run("return typeof credential;").value,"undefined");
        QVERIFY(run("await fetch("+literal(server->url())+",{headers:{Authorization:"+literal(handle)+"}});").error);
        QCOMPARE(run("return await API.getCredential('unknown');").value,QString());
    }

    void credentialRedirectsAndCancellationResetHandles() {
        configureCredentials();Fixture other;
        server->respond=[&other](const HttpRequest& r){if(r.target=="/hold")return HttpResponse{200,{}, {},true};return HttpResponse{302,"redirect",{{"Location",other.url().toUtf8()}}};};
        QCOMPARE(run("globalThis.c=await API.getCredential('fixture-pat');const r=await fetch("+literal(server->url("/redirect"))+",{headers:{Authorization:c.token}});return [r.status,r.redirected];").value,"[302,false]");QCOMPARE(other.requests.size(),0);
        const auto prior=run("return c.token;").value;QSignalSpy reset(host.get(),&SandboxHost::contextReset);
        QTimer::singleShot(50,host.get(),[this]{host->cancel("cancel-network");});
        const auto canceled=run("await fetch("+literal(server->url("/hold"))+",{headers:{Authorization:c.token}});","cancel-network");QVERIFY2(canceled.error,qPrintable(canceled.raw));QCOMPARE(reset.count(),1);
        const auto fresh=run("return (await API.getCredential('fixture-pat')).token;");QVERIFY2(!fresh.error,qPrintable(fresh.raw));QVERIFY(fresh.value!=prior);
        QCOMPARE(run("return typeof c;").value,"undefined");QCOMPARE(reset.count(),1);
    }

    void identityCorrelationAndResetNotificationSafety() {
        disconnect(host.get(),&SandboxHost::networkRequest,broker.get(),&NetworkBroker::fetch);
        QStringList agents;QHash<QString,QString> ids;
        connect(host.get(),&SandboxHost::networkRequest,host.get(),[this,&agents,&ids](const QString& id,const QString& agent,const QJsonObject& request){
            agents.append(agent);ids.insert(agent,id);const int delay=agent=="alpha"?60:5;QTimer::singleShot(delay,host.get(),[this,id,request,agent]{host->completeNetworkRequest(id,{{"status",200},{"statusText","Fixture"},{"url",request["url"]},{"headers",QJsonArray{}},{"bodyBase64",QString::fromLatin1(agent.toUtf8().toBase64())}},{});});
        });
        QSignalSpy done(host.get(),&SandboxHost::completed);
        host->execute("alpha-run","executeSandboxJs",{{"explanation","Wait"},{"script","return await (await fetch('http://127.0.0.1/')).text();"}});
        host->setAgentId("beta");const auto beta=run("return await (await fetch('http://127.0.0.1/')).text();");QCOMPARE(beta.value,"beta");QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(agents.contains("alpha"));QVERIFY(agents.contains("beta"));QVERIFY(ids["alpha"]!=ids["beta"]);
        QSignalSpy resets(host.get(),&SandboxHost::contextReset);host->setAgentId("alpha");
        QCOMPARE(run("setTimeout(()=>{throw new Error('idle failure');},30);return 1;").value,"1");QTRY_COMPARE_WITH_TIMEOUT(resets.count(),1,3000);QCOMPARE(resets.first()[0].toString(),"alpha");
        QCOMPARE(run("return 'recovered';").value,"recovered");
    }

    void boundedBodiesAndTimeoutResetRevokeCredentials() {
        server->respond=[](const HttpRequest& r){return HttpResponse{200,QByteArray(r.target=="/too-big"?2*1024*1024+1:2*1024*1024,'b'),{}};};
        const auto accepted=run("const r=await fetch("+literal(server->url("/limit"))+");const bytes=await r.bytes();return [bytes.length,bytes[0],bytes[bytes.length-1]];");
        QVERIFY2(!accepted.error,qPrintable(accepted.raw));QCOMPARE(accepted.value,"[2097152,98,98]");
        const auto rejected=run("await fetch("+literal(server->url("/too-big"))+");");QVERIFY(rejected.error);QVERIFY2(rejected.raw.contains("limit"),qPrintable(rejected.raw));
        configureCredentials();const auto credential=run("return (await API.getCredential('fixture-pat')).token;");QVERIFY(!credential.error);
        QSignalSpy resets(host.get(),&SandboxHost::contextReset);host->setExecutionLimits(60,1000,2000);
        QVERIFY(run("while(true){};").error);QCOMPARE(resets.count(),1);
        host->setExecutionLimits(2000,4000,5000);
        QVERIFY(run("await fetch("+literal(server->url())+",{headers:{Authorization:"+literal(credential.value)+"}});").error);
        QCOMPARE(run("return 42;").value,"42");QCOMPARE(resets.count(),1);
    }

    void reentrantDeletionFromContextResetIsSafe() {
        auto* doomed=new SandboxHost;QPointer<SandboxHost> guard(doomed);doomed->setExecutionLimits(60,1000,2000);
        connect(doomed,&SandboxHost::contextReset,this,[doomed]{delete doomed;});
        doomed->execute("delete-reset","executeSandboxJs",{{"explanation","Reset worker"},{"script","while(true){};"}});
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),3000);
    }

    void toolCancellationStopsItsNetworkImmediately() {
        server->respond=[](const HttpRequest&){return HttpResponse{200,{}, {},true};};
        QSignalSpy requests(host.get(),&SandboxHost::networkRequest),networkDone(broker.get(),&NetworkBroker::completed),toolDone(host.get(),&SandboxHost::completed);
        const QString script="await fetch("+literal(server->url("/hold"))+");";
        host->execute("cancel-alpha","executeSandboxJs",{{"explanation","Wait"},{"script",script}});
        QTRY_COMPARE_WITH_TIMEOUT(requests.count(),1,3000);
        host->setAgentId("beta");host->execute("keep-beta","executeSandboxJs",{{"explanation","Wait"},{"script",script}});
        QTRY_COMPARE_WITH_TIMEOUT(requests.count(),2,3000);QTRY_COMPARE_WITH_TIMEOUT(server->requests.count(),2,3000);
        const QString alphaId=requests.at(0).at(0).toString(),betaId=requests.at(1).at(0).toString();
        host->cancel("cancel-alpha");
        QCOMPARE(networkDone.count(),1);QCOMPARE(networkDone.first().at(0).toString(),alphaId);QVERIFY(!networkDone.first().at(2).toString().isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(toolDone.count(),1,3000);QCOMPARE(networkDone.count(),1);
        host->cancel("keep-beta");QCOMPARE(networkDone.count(),2);QCOMPARE(networkDone.last().at(0).toString(),betaId);
        QTRY_COMPARE_WITH_TIMEOUT(toolDone.count(),2,3000);QCOMPARE(networkDone.count(),2);
    }
};
QTEST_GUILESS_MAIN(SandboxNetworkTests)
#include "test_sandbox_network.moc"
