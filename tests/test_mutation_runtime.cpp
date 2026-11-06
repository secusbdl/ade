#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "mutations/FileMutationJournal.h"
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
};
QTEST_MAIN(MutationRuntimeTest)
#include "test_mutation_runtime.moc"
