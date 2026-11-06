#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "shell/ShellService.h"
#include <functional>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
struct Call {QString id,name;QJsonObject args;};
QByteArray event(const QJsonObject& object){return "data: "+QJsonDocument(object).toJson(QJsonDocument::Compact)+"\n\n";}
QByteArray response(const QString& protocol,const QList<Call>& calls={}){
    if(protocol=="responses"){
        QJsonArray output;
        for(const auto& call:calls)output.append(QJsonObject{{"type","function_call"},{"id","item-"+call.id},{"call_id",call.id},{"name",call.name},{"arguments",QString::fromUtf8(QJsonDocument(call.args).toJson(QJsonDocument::Compact))}});
        if(calls.isEmpty())output.append(QJsonObject{{"type","message"},{"id","message"},{"role","assistant"},{"content",QJsonArray{QJsonObject{{"type","output_text"},{"text","DONE"}}}}});
        return event({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",output}}}});
    }
    if(protocol=="anthropic"){
        QByteArray body=event({{"type","message_start"},{"message",QJsonObject{{"role","assistant"}}}});int index=0;
        for(const auto& call:calls){
            body+=event({{"type","content_block_start"},{"index",index},{"content_block",QJsonObject{{"type","tool_use"},{"id",call.id},{"name",call.name},{"input",QJsonObject{}}}}});
            body+=event({{"type","content_block_delta"},{"index",index},{"delta",QJsonObject{{"type","input_json_delta"},{"partial_json",QString::fromUtf8(QJsonDocument(call.args).toJson(QJsonDocument::Compact))}}}});
            body+=event({{"type","content_block_stop"},{"index",index++}});
        }
        if(calls.isEmpty())body+=event({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","text"},{"text","DONE"}}}});
        return body+event({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason",calls.isEmpty()?"end_turn":"tool_use"}}}})+event({{"type","message_stop"}});
    }
    if(protocol=="google"){
        QJsonArray parts;for(const auto& call:calls)parts.append(QJsonObject{{"functionCall",QJsonObject{{"id",call.id},{"name",call.name},{"args",call.args}}}});
        if(calls.isEmpty())parts.append(QJsonObject{{"text","DONE"}});
        const QJsonObject candidate{{"content",QJsonObject{{"role","model"},{"parts",parts}}},{"finishReason","STOP"}};
        return event({{"candidates",QJsonArray{candidate}}});
    }
    QJsonObject delta;
    if(calls.isEmpty())delta["content"]="DONE";
    else {QJsonArray tools;int index=0;for(const auto& call:calls)tools.append(QJsonObject{{"index",index++},{"id",call.id},{"type","function"},{"function",QJsonObject{{"name",call.name},{"arguments",QString::fromUtf8(QJsonDocument(call.args).toJson(QJsonDocument::Compact))}}}});delta["tool_calls"]=tools;}
    return event({{"choices",QJsonArray{QJsonObject{{"delta",delta},{"finish_reason",calls.isEmpty()?"stop":"tool_calls"}}}}})+"data: [DONE]\n\n";
}
class Model:public QObject {
public:
    QTcpServer server;QString protocol="openai";QList<QByteArray> replies;QList<QJsonObject> requests;std::function<QList<Call>(int,const QJsonObject&)> handler;
    Model(){
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                const auto bytes=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",bytes);
                const auto split=bytes.indexOf("\r\n\r\n");if(split<0)return;int length=0;
                for(const auto& line:bytes.left(split).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<split+4+length)return;
                socket->setProperty("handled",true);
                requests.append(QJsonDocument::fromJson(bytes.mid(split+4,length)).object());
                const auto body=handler?response(protocol,handler(requests.size()-1,requests.last())):(replies.isEmpty()?response(protocol):replies.takeFirst());
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();
            });
        }});
    }
    ProviderConfig provider()const{return {"fixture","Fixture",QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),"fixture-only","gemini-3.1-flash-lite",protocol};}
};
QList<QJsonObject> toolResults(const QJsonArray& history){QList<QJsonObject> result;for(const auto& value:history)if(value.toObject()["role"]=="tool")result.append(value.toObject());return result;}
}


namespace {
QString sessionId(const QJsonValue& value){
    if(value.isObject()){const auto object=value.toObject();if(object["session_id"].isString())return object["session_id"].toString();for(auto it=object.begin();it!=object.end();++it){const auto id=sessionId(it.value());if(!id.isEmpty())return id;}}
    else if(value.isArray()){for(const auto& child:value.toArray()){const auto id=sessionId(child);if(!id.isEmpty())return id;}}
    else if(value.isString()){const auto text=value.toString().trimmed();if(text.startsWith('{')||text.startsWith('[')){QJsonParseError error;const auto json=QJsonDocument::fromJson(text.toUtf8(),&error);if(error.error==QJsonParseError::NoError)return sessionId(json.isObject()?QJsonValue(json.object()):QJsonValue(json.array()));}}
    return {};
}
QJsonObject waitForExit(){return {{"exited",true},{"timeout_ms",10000}};}
}
class ShellRuntimeTests:public QObject{
 Q_OBJECT
private slots:
 void persistentSessionsAndReadMount_data(){QTest::addColumn<QString>("protocol");for(const auto& value:{"openai","responses","anthropic","google"})QTest::newRow(value)<<QString(value);}
 void persistentSessionsAndReadMount(){
    QFETCH(QString,protocol);QTemporaryDir data,workspace;Model model;model.protocol=protocol;QString ownedId;
    model.handler=[&](int round,const QJsonObject& request)->QList<Call>{
        if(round==0)return {{"create","createShellSession",{{"cwd","ws1"}}}};
        if(ownedId.isEmpty())ownedId=sessionId(request);
        if(round==1)return {{"first","executeShellCommand",{{"explanation","Fixture variable"},{"session_id",ownedId},{"command","$adeFixture='PERSISTED_VALUE'; Write-Output 'FIRST_MARK'"},{"wait_until",waitForExit()}}}};
        if(round==2)return {{"second","executeShellCommand",{{"explanation","Fixture persistence"},{"session_id",ownedId},{"command","Write-Output $adeFixture"},{"wait_until",waitForExit()}}}};
        if(round==3)return {{"read","read",{{"path","shells/"+ownedId+".shell.log"},{"start_line",1},{"end_line",100}}}};
        if(round==4)return {{"raw","executeSandboxJs",{{"explanation","Read owned log"},{"script","return (await fsPromises.readFile('shells/"+ownedId+".shell.log','utf8')).includes('PERSISTED_VALUE');"}}}};
        return {};
    };
    AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());engine.setAlwaysAllow(true);AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Persistent shell fixture");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,60000);QVERIFY2(engine.lastTurnSucceeded(),qPrintable(QJsonDocument(engine.messages()).toJson()));QVERIFY(!ownedId.isEmpty());const auto results=toolResults(engine.messages());QCOMPARE(results.size(),5);QVERIFY(results[2]["content"].toString().contains("PERSISTED_VALUE"));QVERIFY(results[4]["content"].toString().contains("true"));QVERIFY(results[3]["metadata"].toObject().contains("fileReads"));
    for(const auto& request:model.requests){QVERIFY(QJsonDocument(request).toJson().contains("shell-sessions"));}
    QVERIFY(QJsonDocument(model.requests[2]).toJson().contains(ownedId.toUtf8()));QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("<shell-sessions>"));const auto sessions=host.shells()->snapshot()["sessions"].toArray();QCOMPARE(sessions.size(),1);QVERIFY(!sessions[0].toObject()["exited"].toBool());
 }
 void workspaceSwapClosesSessionsAndForkCopiesLogsOnly(){
    QTemporaryDir data,workspace,replacement;AgentEngine engine;engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy complete(host.shells(),&ShellService::completed);host.shells()->execute("create","createShellSession",{{"cwd","ws1"}});QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,20000);QVERIFY2(!complete[0][2].toBool(),qPrintable(complete[0][1].toString()));const auto id=sessionId(QJsonDocument::fromJson(complete[0][1].toString().toUtf8()).object());QVERIFY(!id.isEmpty());host.shells()->execute("run","executeShellCommand",{{"explanation","Fixture"},{"session_id",id},{"command","Write-Output 'FORK_LOG_MARK'"},{"wait_until",waitForExit()}});QTRY_COMPARE_WITH_TIMEOUT(complete.size(),2,20000);QString error;QVERIFY2(host.cloneFileContext("fork",&error),qPrintable(error));AgentEngine forkEngine;AgentToolHost fork(&forkEngine,nullptr,"fork",data.path());QVERIFY(fork.shells()->snapshot()["sessions"].toArray().isEmpty());
    QSignalSpy raw(fork.sandbox(),&SandboxHost::completed);fork.sandbox()->execute("read","executeSandboxJs",{{"explanation","Fixture"},{"script","return (await fsPromises.readFile('shells/"+id+".shell.log','utf8')).includes('FORK_LOG_MARK');"}});QTRY_COMPARE_WITH_TIMEOUT(raw.size(),1,10000);QVERIFY2(!raw[0][2].toBool(),qPrintable(raw[0][1].toString()));QVERIFY(raw[0][1].toString().contains("true"));QVERIFY(host.setWorkspace(replacement.path(),&error));QVERIFY(host.shells()->snapshot()["sessions"].toArray()[0].toObject()["exited"].toBool());QVERIFY(!host.shells()->manifest().contains(id));
 }
 void engineCancellationKillsOnlyOwnedSession(){
    QTemporaryDir data,workspace;AgentEngine engine;engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy shellDone(host.shells(),&ShellService::completed);host.shells()->execute("create","createShellSession",{{"cwd","ws1"}});QTRY_COMPARE_WITH_TIMEOUT(shellDone.size(),1,20000);QVERIFY(!shellDone[0][2].toBool());const auto id=sessionId(QJsonDocument::fromJson(shellDone[0][1].toString().toUtf8()).object());host.shells()->execute("unrelated","createShellSession",{{"cwd","ws1"}});QTRY_COMPARE_WITH_TIMEOUT(shellDone.size(),2,20000);QVERIFY(!shellDone[1][2].toBool());Model model;model.replies={response("openai",{{"slow","executeShellCommand",{{"explanation","Cancellation fixture"},{"session_id",id},{"command","Start-Sleep -Seconds 30; Write-Output 'FORBIDDEN_LATE'"},{"wait_until",waitForExit()}}}})};engine.setProvider(model.provider());engine.setAlwaysAllow(true);connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString&,const QString& name){if(name=="executeShellCommand")engine.cancel();});QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Cancel slow owned shell");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);int unrelated=0;for(const auto& value:host.shells()->snapshot()["sessions"].toArray()){const auto row=value.toObject();if(row["id"].toString()==id)QVERIFY(row["exited"].toBool());else{QVERIFY(!row["exited"].toBool());++unrelated;}}QCOMPARE(unrelated,1);QVERIFY(!engine.lastTurnSucceeded());
 }
 void failedWatcherQueueCommitRetriesOnceWithoutResuming(){
    QTemporaryDir data,workspace;AgentEngine engine;engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));int attempts=0;QJsonArray saved;engine.setQueueCommitter([&](const QJsonArray&,const QJsonArray& queue,QString* error){if(++attempts==1){*error="Fixture temporary save failure";return false;}saved=queue;return true;});QSignalSpy events(host.shells(),&ShellService::watcherCompleted),errors(&engine,&AgentEngine::queueError);
    host.shells()->execute("watch","createWatcherSession",{{"cwd","ws1"},{"title","Retry fixture"},{"command","Write-Output 'RETRY_EVENT'; exit 0"},{"timeout_ms",10000}});QTRY_COMPARE_WITH_TIMEOUT(events.size(),1,20000);QTRY_COMPARE_WITH_TIMEOUT(engine.queuedMessages().size(),1,3000);QCOMPARE(attempts,2);QCOMPARE(saved.size(),1);QCOMPARE(errors.size(),1);QVERIFY(!engine.isBusy());QTest::qWait(600);QCOMPARE(attempts,2);host.shells()->watcherCompleted(events[0][0].toJsonObject());QCOMPARE(engine.queuedMessages().size(),1);
 }
 void deletingHostCancelsFailedWatcherRetry(){
    QTemporaryDir data,workspace;AgentEngine engine;engine.setWorkspace(workspace.path());auto* host=new AgentToolHost(&engine,nullptr,"chat",data.path());QVERIFY(host->setWorkspace(workspace.path()));int attempts=0;engine.setQueueCommitter([&](const QJsonArray&,const QJsonArray&,QString* error){++attempts;*error="Fixture save unavailable";return false;});QSignalSpy events(host->shells(),&ShellService::watcherCompleted);host->shells()->execute("watch","createWatcherSession",{{"cwd","ws1"},{"title","Delete retry fixture"},{"command","Write-Output 'DELETE_RETRY_EVENT'; exit 0"},{"timeout_ms",10000}});QTRY_COMPARE_WITH_TIMEOUT(events.size(),1,20000);QVERIFY(attempts>=1);delete host;const auto afterDelete=attempts;QTest::qWait(700);QCOMPARE(attempts,afterDelete);QVERIFY(engine.queuedMessages().isEmpty());QVERIFY(!engine.isBusy());
 }
 void malformedWatcherExitCodeCannotEscapePayloadBound(){
    QTemporaryDir data,workspace;AgentEngine engine;engine.setWorkspace(workspace.path());
    AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));
    engine.setQueueCommitter([](const QJsonArray&,const QJsonArray&,QString*){return true;});
    QSignalSpy completed(host.shells(),&ShellService::completed);
    host.shells()->execute("watch","createWatcherSession",{{"cwd","ws1"},{"title","Bounds fixture"},{"command","Start-Sleep -Seconds 30"},{"timeout_ms",30000}});
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,20000);QVERIFY(!completed[0][2].toBool());
    const auto rows=host.shells()->snapshot()["sessions"].toArray();QCOMPARE(rows.size(),1);
    const auto id=rows[0].toObject()["id"].toString();
    QJsonObject event{{"agentId","chat"},{"sessionId",id},{"outcome","triggered"},
        {"exitCode",QJsonObject{{"invalid",QString(70000,QChar('x'))}}},{"output","malformed"}};
    host.shells()->watcherCompleted(event);QVERIFY(engine.queuedMessages().isEmpty());
    event["exitCode"]=0.5;host.shells()->watcherCompleted(event);QVERIFY(engine.queuedMessages().isEmpty());
    event["exitCode"]=0;event["output"]=QString(100000,QChar(0x4e2d));
    host.shells()->watcherCompleted(event);QCOMPARE(engine.queuedMessages().size(),1);QVERIFY(!engine.isBusy());
    const auto message=engine.queuedMessages()[0].toObject()["content"].toString();
    const auto json=message.mid(message.indexOf('\n')+1).toUtf8();QVERIFY(json.size()<=64*1024);
    const auto payload=QJsonDocument::fromJson(json).object();QVERIFY(payload["truncated"].toBool());
    QCOMPARE(payload["exit_code"].toInt(),0);QCOMPARE(payload["session_id"].toString(),id);
 }
 void watcherWakeDoesNotInterruptActualPendingQuestion(){
    QTemporaryDir data,workspace;Model model;
    const QJsonObject field{{"type","input"},{"questionId","name"},{"label","Name"}};
    const QJsonObject form{{"title","Fixture question"},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{field}}}}}};
    model.replies={response("openai",{{"question","askUserQuestions",form}})};AgentEngine engine;engine.setQuestionToolEnabled(true);engine.setProvider(model.provider());engine.setWorkspace(workspace.path());engine.setAlwaysAllow(true);AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QJsonArray persisted;engine.setQueueCommitter([&](const QJsonArray&,const QJsonArray& queue,QString*){persisted=queue;return true;});QVERIFY(engine.sendUserMessage("Ask fixture question"));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),10000);const auto question=engine.pendingQuestion();QSignalSpy events(host.shells(),&ShellService::watcherCompleted);
    host.shells()->execute("watch","createWatcherSession",{{"cwd","ws1"},{"title","Question-safe fixture"},{"command","Write-Output 'QUESTION_EVENT'; exit 0"},{"timeout_ms",10000}});QTRY_COMPARE_WITH_TIMEOUT(events.size(),1,20000);QCOMPARE(engine.pendingQuestion(),question);QVERIFY(engine.isBusy());QCOMPARE(engine.queuedMessages().size(),1);QCOMPARE(persisted.size(),1);QCOMPARE(model.requests.size(),1);engine.cancel();QTest::qWait(50);QCOMPARE(model.requests.size(),1);QCOMPARE(engine.queuedMessages().size(),1);
 }
 void watcherWakeRespectsPausedQueueAndOwner(){
    QTemporaryDir data,workspace;AgentEngine engine,otherEngine;engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path()),other(&otherEngine,nullptr,"other",data.path());QVERIFY(host.setWorkspace(workspace.path()));QJsonArray persisted;engine.setQueueCommitter([&](const QJsonArray&,const QJsonArray& queue,QString*){persisted=queue;return true;});QSignalSpy events(host.shells(),&ShellService::watcherCompleted),done(host.shells(),&ShellService::completed);
    bool replayed=false;connect(&engine,&AgentEngine::queuedMessagesChanged,&engine,[&]{if(replayed||engine.queuedMessages().isEmpty())return;replayed=true;const auto rows=host.shells()->snapshot()["sessions"].toArray();for(const auto& value:rows){const auto row=value.toObject();if(!row["watcher"].toObject().isEmpty())host.shells()->watcherCompleted({{"agentId","source"},{"sessionId",row["id"]},{"outcome","triggered"},{"output","duplicate callback"}});}});
    host.shells()->execute("watch","createWatcherSession",{{"cwd","ws1"},{"title","Fixture event"},{"command","Write-Output 'UNTRUSTED_WATCHER_MARK'; exit 0"},{"timeout_ms",10000}});QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,20000);QVERIFY2(!done[0][2].toBool(),qPrintable(done[0][1].toString()));QTRY_COMPARE_WITH_TIMEOUT(events.size(),2,20000);QVERIFY(replayed);QCOMPARE(engine.queuedMessages().size(),1);QCOMPARE(persisted.size(),1);QVERIFY(!engine.isBusy());QVERIFY(engine.queuedMessages()[0].toObject()["content"].toString().contains("untrusted"));const auto event=events[0][0].toJsonObject();host.shells()->watcherCompleted(event);other.shells()->watcherCompleted(event);QCOMPARE(engine.queuedMessages().size(),1);QVERIFY(otherEngine.queuedMessages().isEmpty());
 }
};
QTEST_MAIN(ShellRuntimeTests)
#include "test_shell_runtime.moc"
