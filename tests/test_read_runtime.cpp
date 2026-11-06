#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "ui/SessionStore.h"
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
QByteArray serialized(const QJsonObject& object){return QJsonDocument(object).toJson(QJsonDocument::Compact);}
QList<QJsonObject> toolResults(const QJsonArray& history){QList<QJsonObject> result;for(const auto& value:history)if(value.toObject()["role"]=="tool")result.append(value.toObject());return result;}
}
class ReadRuntimeTest:public QObject{
    Q_OBJECT
private slots:
    void currentEngineAndHostPrepareCapturedRanges_data(){QTest::addColumn<QString>("protocol");for(const auto& name:QStringList{"openai","responses","anthropic","google"})QTest::newRow(qPrintable(name))<<name;}
    void currentEngineAndHostPrepareCapturedRanges(){
        QFETCH(QString,protocol);QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"LINE_ONE\nLINE_TWO\nLINE_THREE\n"));
        Model model;model.protocol=protocol;model.replies={response(protocol,{{"call-read","read",{{"path","w1/sample.txt"},{"start_line",2},{"end_line",2}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));
        connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString&,const QString&,bool){QFile::remove(workspace.filePath("sample.txt"));});
        QSignalSpy finished(&engine,&AgentEngine::finished),failed(&engine,&AgentEngine::failed);engine.submit("Read only the requested range");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,10000);QVERIFY2(failed.isEmpty(),failed.isEmpty()?"":qPrintable(failed.first().first().toString()));QVERIFY(engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),2);
        const auto body=serialized(model.requests.last());QVERIFY(body.contains("2|LINE_TWO"));QVERIFY(!body.contains("LINE_ONE"));QVERIFY(!body.contains("LINE_THREE"));
        const auto results=toolResults(engine.messages());QCOMPARE(results.size(),1);QCOMPARE(results.first()["tool_call_id"].toString(),"call-read");QVERIFY(results.first()["metadata"].toObject()["fileReads"].isArray());QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("LINE_TWO"));
    }
    void samePathDifferentCallsCaptureEachVersion(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"OLD_ONE\nOLD_TWO\nOLD_THREE\n"));Model model;
        model.replies={response("openai",{{"first","read",{{"path","sample.txt"},{"start_line",1},{"end_line",1}}},{"second","read",{{"path","sample.txt"},{"start_line",3},{"end_line",3}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));
        connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString& id,const QString&,bool){if(id=="first")QVERIFY(write(workspace.filePath("sample.txt"),"NEW_ONE\nNEW_TWO\nNEW_THREE\n"));});
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read twice");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),2);
        const auto body=serialized(model.requests.last());QVERIFY(body.contains("1|OLD_ONE"));QVERIFY(body.contains("3|NEW_THREE"));QVERIFY(!body.contains("2|NEW_TWO"));
        const auto results=toolResults(engine.messages());QCOMPARE(results.size(),2);QVERIFY(results[0]["metadata"].toObject()["pathReferences"]!=results[1]["metadata"].toObject()["pathReferences"]);
    }
    void appsImageAndDirectoryProduceTransientParts(){
        QTemporaryDir data;Model model;AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"source",data.path());
        QImage image(2,2,QImage::Format_RGB32);image.fill(Qt::red);QVERIFY(image.save(host.apps()->agentAppsDirectory()+"/image.png","PNG"));
        model.replies={response("openai",{{"image","read",{{"path","apps/image.png"}}},{"directory","ls",{{"path","apps"},{"depth",0}}}})};
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read the app files");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),2);
        const auto body=serialized(model.requests.last());QVERIFY(body.contains("image_url"));QVERIFY(body.contains("image.png"));QVERIFY(body.contains("type:directory"));
        const auto saved=QJsonDocument(engine.messages()).toJson();QVERIFY(!saved.contains("dataBase64"));QVERIFY(!saved.contains("data:image"));QCOMPARE(toolResults(engine.messages()).size(),2);
    }
    void sessionCloneRetainsReadsAndAttachmentsAfterSourceDeletion(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"CAPTURED_SOURCE"));Model model;model.replies={response("openai",{{"read","read",{{"path","sample.txt"}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost source(&engine,nullptr,"source",data.path());QVERIFY(source.setWorkspace(workspace.path()));
        QString error;AttachmentStore attachments(data.path());const auto upload=attachments.write("source","upload.txt","UPLOADED_SOURCE",&error);QVERIFY(!upload.isEmpty());const auto meta=source.captureAttachments(QJsonArray{upload.toJson()},&error);QVERIFY2(!meta.isEmpty(),qPrintable(error));
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submitWithAttachments("Read file",meta["attachments"].toArray(),meta["pathReferences"].toObject(),meta["pathMetadata"].toObject());QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());
        Conversation original;original.id="source";original.title="Source";original.messages=engine.messages();Conversation fork=original;fork.id="fork";fork.title="Fork";SessionStore sessions(data.path());QVERIFY(sessions.save({original},&error));
        QVERIFY2(source.cloneFileContext("fork",&error),qPrintable(error));QVERIFY(sessions.save({original,fork},&error));QVERIFY(QFile::remove(workspace.filePath("sample.txt")));
        AgentEngine forkEngine;forkEngine.setProvider(model.provider());forkEngine.setMessages(fork.messages);AgentToolHost forkHost(&forkEngine,nullptr,"fork",data.path());
        QVERIFY(sessions.save({fork},&error));QVERIFY2(source.removeFileContext(&error),qPrintable(error));QCOMPARE(sessions.load(&error).size(),1);
        QSignalSpy forkDone(&forkEngine,&AgentEngine::finished);forkEngine.submit("Use the original context");QTRY_COMPARE_WITH_TIMEOUT(forkDone.size(),1,10000);QVERIFY(forkEngine.lastTurnSucceeded());
        const auto body=serialized(model.requests.last());QVERIFY(body.contains("CAPTURED_SOURCE"));QVERIFY(body.contains("UPLOADED_SOURCE"));QCOMPARE(attachments.read("fork",upload,{},&error),QByteArray("UPLOADED_SOURCE"));
        const auto before=model.requests.size();engine.submit("Removed context must not return");QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,3000);QVERIFY(!engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),before);
    }
    void deleteDuringCaptureDoesNotRecreateStorage(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"PRIVATE_CAPTURE"));Model model;model.replies={response("openai",{{"read","read",{{"path","sample.txt"}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"source",data.path());QVERIFY(host.setWorkspace(workspace.path()));
        FileReadSnapshotStore snapshots(data.path());const auto lease=snapshots.acquire("source");auto gate=std::make_shared<QSemaphore>();auto entered=std::make_shared<std::atomic_bool>(false),completed=std::make_shared<std::atomic_bool>(false);
        engine.setFileReadCommitter([lease,gate,entered,completed](const auto& capture,const auto* cancel,QString* error){entered->store(true);gate->acquire();const auto result=FileReadSnapshotStore::commit(lease,capture,cancel,error);completed->store(true);return result;});
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read and cancel");QTRY_VERIFY_WITH_TIMEOUT(entered->load(),5000);QString error;QVERIFY2(host.removeFileContext(&error),qPrintable(error));gate->release();QTRY_VERIFY(completed->load());QTRY_COMPARE(done.size(),1);QVERIFY(!engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),1);
        QVERIFY(!QFileInfo::exists(data.path()+"/read-context-v1/attachment-snapshots/source"));for(const auto& result:toolResults(engine.messages()))QVERIFY(!result["metadata"].toObject().contains("fileReads"));
    }
    void cloneRefusesAnOwnedDestinationWithoutChangingIt(){
        QTemporaryDir data;AgentEngine firstEngine,secondEngine;AgentToolHost first(&firstEngine,nullptr,"first",data.path()),second(&secondEngine,nullptr,"second",data.path());QString error;
        QVERIFY(!first.cloneFileContext("second",&error));QVERIFY(!error.isEmpty());AttachmentStore attachments(data.path());const auto ref=attachments.write("second","still.txt","SECOND_STILL_VALID",&error);QVERIFY(!ref.isEmpty());QVERIFY(!second.captureAttachments(QJsonArray{ref.toJson()},&error).isEmpty());
    }
    void emptySourceCannotCloneOverUnownedAttachmentContext(){
        QTemporaryDir data;AgentEngine engine;AgentToolHost source(&engine,nullptr,"source",data.path());QString error;
        AttachmentStore attachments(data.path());const auto sourceDirectory=attachments.agentDirectory("source",&error);QVERIFY(QDir().rmdir(sourceDirectory));
        const auto existing=attachments.write("target","original.txt","TARGET_MUST_REMAIN",&error);QVERIFY(!existing.isEmpty());
        QVERIFY(!source.cloneFileContext("target",&error));QVERIFY(!error.isEmpty());QCOMPARE(attachments.read("target",existing,{},&error),QByteArray("TARGET_MUST_REMAIN"));
        QVERIFY(!QFileInfo::exists(data.path()+"/read-context-v1/attachment-snapshots/target"));
    }
    void sessionConflictKeepsSourceAndRollsBackOnlyFreshClone(){
        QTemporaryDir data;AgentEngine engine;AgentToolHost source(&engine,nullptr,"source",data.path());QString error;
        AttachmentStore attachments(data.path());const auto ref=attachments.write("source","original.txt","SOURCE_MUST_REMAIN",&error);QVERIFY(!ref.isEmpty());
        const auto captured=source.captureAttachments(QJsonArray{ref.toJson()},&error);QVERIFY(!captured.isEmpty());
        Conversation original;original.id="source";original.title="Original";SessionStore first(data.path());QVERIFY(first.save({original},&error));
        SessionStore other(data.path());auto latest=other.load(&error);QCOMPARE(latest.size(),1);latest[0].title="Concurrent title";QVERIFY(other.save(latest,&error));
        Conversation fork=original;fork.id="new-fork";QVERIFY(source.cloneFileContext(fork.id,&error));QVERIFY(!first.save({original,fork},&error));QVERIFY(!error.isEmpty());
        FileReadSnapshotStore snapshots(data.path());QVERIFY(snapshots.removeAgent(fork.id,&error));QVERIFY(attachments.removeAgent(fork.id,&error));
        QVERIFY(!QFileInfo::exists(attachments.agentDirectory(fork.id,&error)));QCOMPARE(attachments.read("source",ref,{},&error),QByteArray("SOURCE_MUST_REMAIN"));
        QVERIFY(!first.save({},&error)); // A failed durable deletion must not call removeFileContext.
        QVERIFY(!source.captureAttachments(QJsonArray{ref.toJson()},&error).isEmpty());SessionStore check(data.path());const auto retained=check.load(&error);QCOMPARE(retained.size(),1);QCOMPARE(retained[0].title,"Concurrent title");
    }
    void busySourceCanClonePublishedToolContextWithoutCancellation(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"BUSY_PUBLISHED_ORIGINAL"));Model model;model.replies={response("openai",{{"read","read",{{"path","sample.txt"}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost source(&engine,nullptr,"source",data.path());QVERIFY(source.setWorkspace(workspace.path()));
        QString error;bool wasBusy=false,cloned=false;QJsonArray forkHistory;
        connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString&,const QString&,bool){wasBusy=engine.isBusy();forkHistory=engine.messages();cloned=source.cloneFileContext("fork",&error);});
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(wasBusy);QVERIFY2(cloned,qPrintable(error));QVERIFY(engine.lastTurnSucceeded());
        QVERIFY(QFile::remove(workspace.filePath("sample.txt")));AgentEngine fork;fork.setProvider(model.provider());fork.setMessages(forkHistory);AgentToolHost forkHost(&fork,nullptr,"fork",data.path());QSignalSpy forkDone(&fork,&AgentEngine::finished);fork.submit("Continue");QTRY_COMPARE_WITH_TIMEOUT(forkDone.size(),1,10000);QVERIFY(fork.lastTurnSucceeded());QVERIFY(serialized(model.requests.last()).contains("BUSY_PUBLISHED_ORIGINAL"));
    }
    void busyUnpublishedCaptureDoesNotBlockOrAlterClonedHistory(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("sample.txt"),"PUBLISHED_BEFORE_BUSY"));Model model;model.replies={response("openai",{{"read","read",{{"path","sample.txt"}}}})};
        AgentEngine engine;engine.setProvider(model.provider());engine.setWorkspace(workspace.path());AgentToolHost source(&engine,nullptr,"source",data.path());QVERIFY(source.setWorkspace(workspace.path()));
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());const auto settledHistory=engine.messages();
        QVERIFY(write(workspace.filePath("sample.txt"),"UNPUBLISHED_BUSY_BYTES"));FileReadSnapshotStore snapshots(data.path());const auto lease=snapshots.acquire("source");auto gate=std::make_shared<QSemaphore>();auto entered=std::make_shared<std::atomic_bool>(false),completed=std::make_shared<std::atomic_bool>(false);
        engine.setFileReadCommitter([lease,gate,entered,completed](const auto& capture,const auto* canceled,QString* error){entered->store(true);gate->acquire();const auto metadata=FileReadSnapshotStore::commit(lease,capture,canceled,error);completed->store(true);return metadata;});model.replies={response("openai",{{"pending","read",{{"path","sample.txt"}}}})};
        engine.submit("Read changed file");QTRY_VERIFY_WITH_TIMEOUT(entered->load(),5000);const auto pendingHistory=engine.messages();QString error;const bool cloned=source.cloneFileContext("fork",&error);const bool stayedBusy=engine.isBusy();const bool unchanged=engine.messages()==pendingHistory;engine.cancel();gate->release();QTRY_VERIFY(completed->load());QVERIFY2(cloned,qPrintable(error));QVERIFY(stayedBusy);QVERIFY(unchanged);
        AgentEngine fork;fork.setProvider(model.provider());fork.setMessages(settledHistory);AgentToolHost forkHost(&fork,nullptr,"fork",data.path());QSignalSpy forkDone(&fork,&AgentEngine::finished);fork.submit("Use prior context");QTRY_COMPARE_WITH_TIMEOUT(forkDone.size(),1,10000);const auto body=serialized(model.requests.last());QVERIFY(body.contains("PUBLISHED_BEFORE_BUSY"));QVERIFY(!body.contains("UNPUBLISHED_BUSY_BYTES"));
    }
};
QTEST_GUILESS_MAIN(ReadRuntimeTest)
#include "test_read_runtime.moc"
