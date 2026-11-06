#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "mutations/FileMutationJournal.h"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSemaphore>
#include <QScopeGuard>
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
    QTcpServer server;QString protocol="openai";QList<QByteArray> replies;QList<QJsonObject> requests;
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
                const auto body=replies.isEmpty()?response(protocol):replies.takeFirst();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();
            });
        }});
    }
    ProviderConfig provider()const{return {"fixture","Fixture",QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),"fixture-only","gemini-3.1-flash-lite",protocol};}
};
bool write(const QString& path,const QByteArray& body){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(body)==body.size();}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
QByteArray serialized(const QJsonObject& object){return QJsonDocument(object).toJson(QJsonDocument::Compact);}
QList<QJsonObject> toolResults(const QJsonArray& history){QList<QJsonObject> result;for(const auto& value:history)if(value.toObject()["role"]=="tool")result.append(value.toObject());return result;}
}

class MutationRuntimeTest:public QObject{
 Q_OBJECT
private slots:
 void actualEngineToolsPersistAndUndo_data(){QTest::addColumn<QString>("protocol");for(const auto& value:{"openai","responses","anthropic","google"})QTest::newRow(value)<<QString(value);}
 void actualEngineToolsPersistAndUndo(){
    QFETCH(QString,protocol);QTemporaryDir data,workspace;const auto original=QByteArray::fromHex("00ff0d0a");QVERIFY(write(workspace.filePath("file.txt"),original));Model model;model.protocol=protocol;
    model.replies={response(protocol,{{"write-call","write",{{"path","file.txt"},{"content","```txt\nbefore\r\n```"}}},{"edit-call","multiEdit",{{"path","file.txt"},{"edits",QJsonArray{QJsonObject{{"old_string","absent"},{"new_string","ignored"}},QJsonObject{{"old_string","before"},{"new_string","after"}}}}}},{"copy-call","copy",{{"input_path","file.txt"},{"output_path","copied.bin"},{"move",false}}},{"delete-call","delete",{{"path","file.txt"}}}})};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QString error;QVERIFY(host.setWorkspace(workspace.path(),&error));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Perform fixture mutations");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);QVERIFY(engine.lastTurnSucceeded());QVERIFY(!QFileInfo::exists(workspace.filePath("file.txt")));QCOMPARE(read(workspace.filePath("copied.bin")),QByteArray("after\r"));
    const auto results=toolResults(engine.messages());QCOMPARE(results.size(),4);QStringList ids;for(const auto& row:results){const auto changes=row["metadata"].toObject()["mutations"].toArray();QCOMPARE(changes.size(),1);QVERIFY(changes[0].toObject()["committed"].toBool());ids.append(changes[0].toObject()["mutationId"].toString());}
    const auto history=host.mutationHistory(&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(history.size(),4);QVERIFY(history[0].toObject()["createdAt"].toDouble()>0);QVERIFY(!serialized(model.requests.last()).contains("mutationId"));QVERIFY(!QJsonDocument(engine.messages()).toJson().contains(original.toBase64()));
    QSignalSpy undo(&host,&AgentToolHost::mutationUndoCompleted),changed(&host,&AgentToolHost::fileChanged);host.undoMutations("undo",ids);QVERIFY(host.mutationBusy());QVERIFY(!host.setWorkspace({},&error));QTRY_COMPARE_WITH_TIMEOUT(undo.size(),1,10000);QVERIFY2(!undo[0][2].toBool(),qPrintable(undo[0][1].toString()));QVERIFY(!host.mutationBusy());QCOMPARE(read(workspace.filePath("file.txt")),original);QVERIFY(!QFileInfo::exists(workspace.filePath("copied.bin")));QCOMPARE(changed.size(),2);
 }
 void restartConflictAndForkAuthority(){
    QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("file.txt"),"original"));Model model;model.replies={response("openai",{{"call","write",{{"path","file.txt"},{"content","changed"}}}})};QString mutation,error;
    {AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Write");QTRY_COMPARE(done.size(),1);QVERIFY(engine.lastTurnSucceeded());mutation=host.mutationHistory(&error)[0].toObject()["mutationId"].toString();QVERIFY2(host.cloneFileContext("fork",&error),qPrintable(error));}
    AgentEngine engine,forkEngine;engine.setWorkspace(workspace.path());forkEngine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path()),fork(&forkEngine,nullptr,"fork",data.path());QVERIFY(host.setWorkspace(workspace.path()));QVERIFY(fork.setWorkspace(workspace.path()));QCOMPARE(host.mutationHistory(&error).size(),1);QVERIFY(fork.mutationHistory(&error).isEmpty());QSignalSpy foreign(&fork,&AgentToolHost::mutationUndoCompleted);fork.undoMutations("foreign",{mutation});QTRY_COMPARE(foreign.size(),1);QVERIFY(foreign[0][2].toBool());QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("changed"));
    QVERIFY(write(workspace.filePath("file.txt"),"user edit"));QSignalSpy conflict(&host,&AgentToolHost::mutationUndoCompleted);host.undoMutations("conflict",{mutation});QTRY_COMPARE(conflict.size(),1);QVERIFY(conflict[0][2].toBool());QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("user edit"));QVERIFY2(host.removeFileContext(&error),qPrintable(error));QVERIFY(fork.mutationHistory(&error).isEmpty());QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("user edit"));
 }
 void sandboxRawWritesAreExactAndAttributed(){
    QTemporaryDir data,workspace;Model model;const QString script="await fsPromises.writeFile('ws1/raw.txt','```txt\\r\\nraw\\r\\n```');await fsPromises.writeFile('ws1/binary.bin',Buffer.from([0,255,128,10]));return 'done';";model.replies={response("openai",{{"sandbox-call","executeSandboxJs",{{"explanation","Fixture"},{"script",script}}}})};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Raw file fixture");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(read(workspace.filePath("raw.txt")),QByteArray("```txt\r\nraw\r\n```"));QCOMPARE(read(workspace.filePath("binary.bin")),QByteArray::fromHex("00ff800a"));QString error;const auto rows=host.mutationHistory(&error);QCOMPARE(rows.size(),2);QStringList ids;for(const auto& value:rows){QCOMPARE(value.toObject()["toolCallId"].toString(),QString("sandbox-call"));ids.append(value.toObject()["mutationId"].toString());}QSignalSpy undo(&host,&AgentToolHost::mutationUndoCompleted);host.undoMutations("undo",ids);QTRY_COMPARE(undo.size(),1);QVERIFY(!undo[0][2].toBool());QVERIFY(!QFileInfo::exists(workspace.filePath("raw.txt")));QVERIFY(!QFileInfo::exists(workspace.filePath("binary.bin")));
 }
 void cancellationBeforeQueuedSandboxMutationDoesNotWrite(){
    QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("file.txt"),"keep"));Model model;model.replies={response("openai",{{"sandbox-call","executeSandboxJs",{{"explanation","Fixture"},{"script","await fsPromises.writeFile('ws1/file.txt','forbidden');"}}}})};AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));connect(host.sandbox(),&SandboxHost::fileRequest,&engine,[&]{engine.cancel();});QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Cancel queued write");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QTest::qWait(100);QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("keep"));QString error;QVERIFY(host.mutationHistory(&error).isEmpty());
 }
 void mutationActionsHaveOneHostLane(){
    QTemporaryDir data,workspace;AgentEngine engine;engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy completed(&host,&AgentToolHost::mutationUndoCompleted);
    host.recoverMutations("first");QVERIFY(host.mutationBusy());host.recoverMutations("second");
    QCOMPARE(completed.size(),1);QCOMPARE(completed[0][0].toString(),QString("second"));QVERIFY(completed[0][2].toBool());QVERIFY(host.mutationBusy());
    QTRY_COMPARE(completed.size(),2);QCOMPARE(completed[1][0].toString(),QString("first"));QVERIFY(!completed[1][2].toBool());QVERIFY(!host.mutationBusy());
 }
 void undoObserverMayDestroyHost(){
    QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("file.txt"),"before"));Model model;model.replies={response("openai",{{"call","write",{{"path","file.txt"},{"content","after"}}}})};AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());auto* host=new AgentToolHost(&engine,nullptr,"chat",data.path());QPointer<AgentToolHost> guard(host);QVERIFY(host->setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Write");QTRY_COMPARE(done.size(),1);QString error;const auto id=host->mutationHistory(&error)[0].toObject()["mutationId"].toString();connect(host,&AgentToolHost::fileChanged,&engine,[host]{delete host;});host->undoMutations("undo",{id});QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),10000);QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("before"));
 }
 void repeatedToolIdsRetainExactSavedTurnIdentity(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"reused","write",{{"path","file.txt"},{"content","first\n"},{"raw",true}}}}),response("openai"),response("openai",{{"reused","write",{{"path","file.txt"},{"content","second\n"},{"raw",true}}}}),response("openai")};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("First turn");QTRY_COMPARE(done.size(),1);engine.submit("Second turn");QTRY_COMPARE(done.size(),2);
    const auto results=toolResults(engine.messages());QCOMPARE(results.size(),2);QStringList ids;for(const auto& result:results)ids.append(result["metadata"].toObject()["mutations"].toArray().first().toObject()["mutationId"].toString());QVERIFY(ids[0]!=ids[1]);
    QJsonArray projected;for(const auto& value:engine.displayMessages())for(const auto& record:value.toObject()["mutations"].toArray())projected.append(record);QCOMPARE(projected.size(),2);QCOMPARE(projected[0].toObject()["mutationId"],ids[0]);QCOMPARE(projected[1].toObject()["mutationId"],ids[1]);
    QSignalSpy summaries(&host,&AgentToolHost::turnFilesReady);host.requestTurnFiles(1,{ids[0]},"ws1/file.txt");host.requestTurnFiles(2,{ids[1]},"ws1/file.txt");QTRY_COMPARE(summaries.size(),2);
    for(const auto& reply:summaries){QVERIFY(reply[2].toString().isEmpty());const auto preview=reply[1].toJsonObject()["preview"].toObject()["files"].toArray().first().toObject();QCOMPARE(preview["after"].toString(),reply[0].toULongLong()==1?QString("first\n"):QString("second\n"));}
 }
 void sandboxOuterResultRetainsAllCommittedMutationIds(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"same-model-id","executeSandboxJs",{{"explanation","Saved change fixture"},{"script","await fsPromises.writeFile('ws1/a.txt','first\\n');await fsPromises.writeFile('ws1/b.txt','second\\n');throw new Error('after committed writes');"}}}})};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Sandbox commits then errors");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,15000);
    const auto results=toolResults(engine.messages());QVERIFY(!results.isEmpty());const auto mutations=results[0]["metadata"].toObject()["mutations"].toArray();QCOMPARE(mutations.size(),2);QStringList ids;for(const auto& value:mutations){QCOMPARE(value.toObject()["agentId"],"chat");QVERIFY(value.toObject()["committed"].toBool());ids.append(value.toObject()["mutationId"].toString());}QVERIFY(ids[0]!=ids[1]);
    QSignalSpy summary(&host,&AgentToolHost::turnFilesReady);host.requestTurnFiles(1,ids);QTRY_COMPARE(summary.size(),1);QVERIFY(summary[0][2].toString().isEmpty());QCOMPARE(summary[0][1].toJsonObject()["files"].toArray().size(),2);QVERIFY(!serialized(model.requests.last()).contains("mutationId"));
 }
 void cancelFromCommittedChangeNotificationRetainsMutationIdentity(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"write-call","write",{{"path","file.txt"},{"content","committed"}}}})};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));connect(&engine,&AgentEngine::fileChanged,&engine,[&]{engine.cancel();});QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Commit and stop");QTRY_COMPARE(done.size(),1);QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("committed"));QString error;const auto journal=host.mutationHistory(&error);QCOMPARE(journal.size(),1);QJsonArray recorded;for(const auto& value:engine.messages())for(const auto& record:value.toObject()["metadata"].toObject()["mutations"].toArray())recorded.append(record);QCOMPARE(recorded.size(),1);QCOMPARE(recorded[0].toObject()["mutationId"],journal[0].toObject()["mutationId"]);
 }
 void lateCommittedResultCannotAttachToReusedModelId(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"same","write",{{"path","first.txt"},{"content","first"}}}}),response("openai",{{"same","write",{{"path","second.txt"},{"content","second"}}}}),response("openai")};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));
    FileMutationJournal journal(data.path(),"chat");QString error;QVERIFY(journal.bindMount("ws1",workspace.path(),true,&error));QVERIFY(journal.bindMount("apps",host.apps()->agentAppsDirectory(),true,&error));
    struct Hold {QSemaphore entered,release;};auto hold=std::make_shared<Hold>();auto release=qScopeGuard([hold]{hold->release.release();});const auto apply=journal.executor();
    engine.setFileMutationExecutor([apply,hold](const FileMutationInvocation& input,const std::atomic_bool* canceled){auto result=apply(input,canceled);if(input.arguments["path"]=="first.txt"){hold->entered.release();hold->release.acquire();}return result;});
    QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("First");QTRY_VERIFY_WITH_TIMEOUT(hold->entered.available(),10000);engine.cancel();QCOMPARE(done.size(),1);QCOMPARE(read(workspace.filePath("first.txt")),QByteArray("first"));
    engine.submit("Second");QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,10000);auto results=toolResults(engine.messages());QCOMPARE(results.size(),2);QVERIFY(results[0]["metadata"].toObject()["mutations"].toArray().isEmpty());QCOMPARE(results[1]["metadata"].toObject()["mutations"].toArray().size(),1);
    const auto second=results[1]["metadata"].toObject()["mutations"].toArray().first().toObject()["mutationId"].toString();hold->release.release();
    QTRY_COMPARE(toolResults(engine.messages())[0]["metadata"].toObject()["mutations"].toArray().size(),1);results=toolResults(engine.messages());QCOMPARE(results.size(),2);const auto first=results[0]["metadata"].toObject()["mutations"].toArray().first().toObject()["mutationId"].toString();QVERIFY(first!=second);QCOMPARE(results[1]["metadata"].toObject()["mutations"].toArray().first().toObject()["mutationId"].toString(),second);
    QVERIFY(!results[0]["content"].toString().contains("cancelled"));QCOMPARE(done.size(),2);QTest::qWait(50);QCOMPARE(model.requests.size(),3);
 }
 void persistedInvocationRecoversAfterOwnerDiesBeforeDelivery(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"same","write",{{"path","file.txt"},{"content","durable"}}}})};
    auto* engine=new AgentEngine;engine->setProvider(model.provider());engine->setAlwaysAllow(true);engine->setWorkspace(workspace.path());auto* host=new AgentToolHost(engine,nullptr,"chat",data.path(),engine);QVERIFY(host->setWorkspace(workspace.path()));
    QJsonArray persisted;engine->setQueueCommitter([&](const QJsonArray& history,const QJsonArray&,QString*){persisted=history;return true;});
    FileMutationJournal journal(data.path(),"chat");QString error;QVERIFY(journal.bindMount("ws1",workspace.path(),true,&error));QVERIFY(journal.bindMount("apps",host->apps()->agentAppsDirectory(),true,&error));
    struct Hold {QSemaphore entered,release,finished;};auto hold=std::make_shared<Hold>();auto release=qScopeGuard([hold]{hold->release.release();});const auto apply=journal.executor();
    engine->setFileMutationExecutor([apply,hold](const FileMutationInvocation& input,const std::atomic_bool* canceled){auto result=apply(input,canceled);hold->entered.release();hold->release.acquire();hold->finished.release();return result;});
    engine->submit("Commit then close");QTRY_VERIFY_WITH_TIMEOUT(hold->entered.available(),10000);QVERIFY(!persisted.isEmpty());QVERIFY(toolResults(persisted).isEmpty());QCOMPARE(persisted.last().toObject()["metadata"].toObject()["mutationInvocations"].toArray().size(),1);delete engine;hold->release.release();QTRY_VERIFY(hold->finished.available());
    AgentEngine restored;restored.setMessages(persisted);AgentToolHost restoredHost(&restored,nullptr,"chat",data.path());const auto results=toolResults(restored.messages());QCOMPARE(results.size(),1);QCOMPARE(results[0]["metadata"].toObject()["mutations"].toArray().size(),1);QVERIFY(!results[0]["content"].toString().contains("interrupted"));QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("durable"));
    QJsonArray savedRecovery;restored.setQueueCommitter([&](const QJsonArray& history,const QJsonArray&,QString*){savedRecovery=history;return true;});QCOMPARE(savedRecovery,restored.messages());
    AgentEngine fork;fork.setMessages(persisted);AgentToolHost forkHost(&fork,nullptr,"different-chat",data.path());QVERIFY(toolResults(fork.messages()).isEmpty());
 }
 void failedDispatchPersistencePreventsMutation(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"call","write",{{"path","file.txt"},{"content","must not write"}}}})};AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));
    engine.setQueueCommitter([](const QJsonArray& history,const QJsonArray&,QString* error){for(const auto& value:history)if(!value.toObject()["metadata"].toObject()["mutationInvocations"].toArray().isEmpty()){*error="Synthetic persistence failure";return false;}return true;});
    QSignalSpy errors(&engine,&AgentEngine::queueError),done(&engine,&AgentEngine::finished);engine.submit("Denied durable dispatch");QTRY_COMPARE(done.size(),1);QCOMPARE(errors.size(),1);QVERIFY(!QFile::exists(workspace.filePath("file.txt")));QString error;QVERIFY(host.mutationHistory(&error).isEmpty());
 }
 void restartedSandboxCommitDoesNotClaimOuterCompletion(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"sandbox","executeSandboxJs",{{"explanation","Commit then keep waiting"},{"script","await fsPromises.writeFile('ws1/file.txt','committed');await new Promise(()=>{});"}}}})};
    auto* engine=new AgentEngine;engine->setProvider(model.provider());engine->setAlwaysAllow(true);engine->setWorkspace(workspace.path());auto* host=new AgentToolHost(engine,nullptr,"chat",data.path(),engine);QVERIFY(host->setWorkspace(workspace.path()));
    QJsonArray persisted;engine->setQueueCommitter([&](const QJsonArray& history,const QJsonArray&,QString*){persisted=history;return true;});QSignalSpy changed(host,&AgentToolHost::fileChanged);engine->submit("Sandbox remains running");QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,15000);QVERIFY(engine->isBusy());QVERIFY(toolResults(persisted).isEmpty());const auto entry=persisted.last().toObject()["metadata"].toObject()["mutationInvocations"].toArray().first().toObject();QVERIFY(entry["external"].toBool());QVERIFY(entry["settled"].toBool());delete engine;
    AgentEngine restored;restored.setMessages(persisted);AgentToolHost recovered(&restored,nullptr,"chat",data.path());const auto results=toolResults(restored.messages());QCOMPARE(results.size(),1);QVERIFY(results[0]["content"].toString().contains("interrupted"));QCOMPARE(results[0]["metadata"].toObject()["display"].toObject()["toolState"],"canceled");QCOMPARE(results[0]["metadata"].toObject()["mutations"].toArray().size(),1);QCOMPARE(restored.displayMessages().last().toObject()["mutations"].toArray().size(),1);QCOMPARE(read(workspace.filePath("file.txt")),QByteArray("committed"));
 }
 void rejectedDispatchObserverCannotCancelReplacementMutation(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"old","write",{{"path","old.txt"},{"content","not permitted"}}}}),response("openai",{{"replacement","write",{{"path","replacement.txt"},{"content","kept"}}}}),response("openai")};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));
    FileMutationJournal journal(data.path(),"chat");QString error;QVERIFY(journal.bindMount("ws1",workspace.path(),true,&error));QVERIFY(journal.bindMount("apps",host.apps()->agentAppsDirectory(),true,&error));
    struct Hold {QSemaphore entered,release;};auto hold=std::make_shared<Hold>();auto release=qScopeGuard([hold]{hold->release.release();});const auto apply=journal.executor();
    engine.setFileMutationExecutor([apply,hold](const FileMutationInvocation& input,const std::atomic_bool* canceled){auto result=apply(input,canceled);hold->entered.release();hold->release.acquire();return result;});
    bool reject=true,reentered=false,replacementReached=false;engine.setQueueCommitter([&](const QJsonArray& history,const QJsonArray&,QString* failure){if(reject)for(const auto& value:history)if(!value.toObject()["metadata"].toObject()["mutationInvocations"].toArray().isEmpty()){reject=false;*failure="Synthetic first dispatch failure";return false;}return true;});
    connect(&engine,&AgentEngine::queueError,&engine,[&]{if(reentered)return;reentered=true;engine.cancel();engine.submit("Replacement");QElapsedTimer timer;timer.start();while(!hold->entered.available()&&timer.elapsed()<10000)QTest::qWait(1);replacementReached=hold->entered.available()>0;});
    QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Rejected first dispatch");QTRY_VERIFY_WITH_TIMEOUT(replacementReached,15000);QTest::qWait(100);QCOMPARE(done.size(),1);QVERIFY(engine.isBusy());QCOMPARE(model.requests.size(),2);QVERIFY(!QFile::exists(workspace.filePath("old.txt")));QCOMPARE(read(workspace.filePath("replacement.txt")),QByteArray("kept"));
    hold->release.release();QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,10000);QCOMPARE(toolResults(engine.messages()).size(),2);QCOMPARE(toolResults(engine.messages())[1]["metadata"].toObject()["mutations"].toArray().size(),1);
 }
 void rejectedSandboxDispatchCannotCancelReplacementRpc(){
    QTemporaryDir data,workspace;Model model;model.replies={response("openai",{{"old","executeSandboxJs",{{"explanation","Rejected"},{"script","await fsPromises.writeFile('ws1/old.txt','not permitted');"}}}}),response("openai",{{"replacement","executeSandboxJs",{{"explanation","Replacement"},{"script","await fsPromises.writeFile('ws1/new.txt','kept');return 'replacement-complete';"}}}}),response("openai")};
    AgentEngine engine;engine.setProvider(model.provider());engine.setAlwaysAllow(true);engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));
    bool reject=true,reentered=false,replacementRegistered=false;QEventLoop nested;
    engine.setQueueCommitter([&](const QJsonArray& history,const QJsonArray&,QString* failure){
        if(!history.isEmpty())for(const auto& value:history.last().toObject()["metadata"].toObject()["mutationInvocations"].toArray()){
            const auto entry=value.toObject();if(reject){reject=false;*failure="Synthetic sandbox dispatch failure";return false;}
            if(entry["toolCallId"]=="replacement"&&!entry["settled"].toBool()){replacementRegistered=true;nested.quit();}
        }
        return true;
    });
    connect(&engine,&AgentEngine::queueError,&engine,[&]{if(reentered)return;reentered=true;engine.cancel();engine.submit("Replacement");QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,&nested,&QEventLoop::quit);timeout.start(10000);nested.exec();});
    QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Reject old sandbox write");QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,15000);QVERIFY(replacementRegistered);QVERIFY(!QFile::exists(workspace.filePath("old.txt")));QCOMPARE(read(workspace.filePath("new.txt")),QByteArray("kept"));
    const auto results=toolResults(engine.messages());QCOMPARE(results.size(),2);QVERIFY2(results[1]["content"].toString().contains("replacement-complete"),qPrintable(results[1]["content"].toString()));QVERIFY(!results[1]["content"].toString().startsWith("Error:"));QCOMPARE(results[1]["metadata"].toObject()["mutations"].toArray().size(),1);
 }
};
QTEST_MAIN(MutationRuntimeTest)
#include "test_mutation_runtime.moc"
