#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include "ui/SessionStore.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSemaphore>
#include <QScopeGuard>
#include <QtEndian>
#include <QtZlib/zlib.h>
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
QByteArray archive(){
    const QByteArray name="nested/visible.txt",body="NEVER_EXTRACT_THIS_CONTENT";
    const auto crc=crc32(0,reinterpret_cast<const Bytef*>(body.constData()),body.size());
    QByteArray local(30,0),central(46,0),end(22,0);
    qToLittleEndian<quint32>(0x04034b50,local.data());qToLittleEndian<quint16>(20,local.data()+4);
    qToLittleEndian<quint32>(crc,local.data()+14);qToLittleEndian<quint32>(body.size(),local.data()+18);qToLittleEndian<quint32>(body.size(),local.data()+22);qToLittleEndian<quint16>(name.size(),local.data()+26);
    qToLittleEndian<quint32>(0x02014b50,central.data());qToLittleEndian<quint16>(20,central.data()+4);qToLittleEndian<quint16>(20,central.data()+6);
    qToLittleEndian<quint32>(crc,central.data()+16);qToLittleEndian<quint32>(body.size(),central.data()+20);qToLittleEndian<quint32>(body.size(),central.data()+24);qToLittleEndian<quint16>(name.size(),central.data()+28);
    qToLittleEndian<quint32>(0x06054b50,end.data());qToLittleEndian<quint16>(1,end.data()+8);qToLittleEndian<quint16>(1,end.data()+10);
    qToLittleEndian<quint32>(central.size()+name.size(),end.data()+12);qToLittleEndian<quint32>(local.size()+name.size()+body.size(),end.data()+16);
    return local+name+body+central+name+end;
}
QByteArray tarArchive(){
    const QByteArray name="nested/visible.txt",body="NEVER_EXTRACT_THIS_CONTENT";QByteArray header(512,0);
    header.replace(0,name.size(),name);header.replace(100,8,QByteArray("0000644\0",8));header.replace(124,12,QByteArray::number(body.size(),8).rightJustified(11,'0')+'\0');header[156]='0';header.replace(257,6,QByteArray("ustar\0",6));header.replace(263,2,"00");header.replace(148,8,QByteArray(8,' '));
    quint64 sum=0;for(const auto byte:header)sum+=quint8(byte);header.replace(148,8,QByteArray::number(sum,8).rightJustified(6,'0')+QByteArray("\0 ",2));
    return header+body+QByteArray((512-body.size()%512)%512,0)+QByteArray(1024,0);
}
QByteArray gzip(const QByteArray& bytes){
    z_stream stream{};if(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,15+16,8,Z_DEFAULT_STRATEGY)!=Z_OK)return {};
    QByteArray output(compressBound(bytes.size())+32,0);stream.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(bytes.constData()));stream.avail_in=bytes.size();stream.next_out=reinterpret_cast<Bytef*>(output.data());stream.avail_out=output.size();
    const auto result=deflate(&stream,Z_FINISH);output.resize(stream.total_out);deflateEnd(&stream);return result==Z_STREAM_END?output:QByteArray();
}
QJsonObject execute(ToolExecutor& tool,const QString& name,const QJsonObject& args,const QString& id="fixture"){
    QSignalSpy results(&tool,&ToolExecutor::completed);tool.execute(id,name,args);QElapsedTimer elapsed;elapsed.start();while(results.isEmpty()&&elapsed.elapsed()<5000)results.wait(25);
    if(results.isEmpty())return {{"output","Timed out"},{"error",true}};
    return {{"output",results.last()[1].toString()},{"error",results.last()[2].toBool()},{"metadata",results.last()[3].toJsonObject()}};
}
}
class FormattedAttTest:public QObject{
    Q_OBJECT
private slots:
    void formattedReadReinjectsAfterDeletion_data(){QTest::addColumn<QString>("protocol");for(const auto& protocol:QStringList{"openai","responses","anthropic","google"})QTest::newRow(qPrintable(protocol))<<protocol;}
    void formattedReadReinjectsAfterDeletion(){
        QFETCH(QString,protocol);QTemporaryDir data;AttachmentStore store(data.path());QString error;
        const auto ref=store.write("chat","sample.txt","EXCLUDED_FIRST\nEXACT_CAPTURED_SECOND\nEXCLUDED_THIRD\n",&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));
        Model model;model.protocol=protocol;model.replies={response(protocol,{{"call-att","read",{{"path",ref.path},{"start_line",2},{"end_line",2}}}})};
        AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"chat",data.path());
        const auto original=store.resolve("chat",ref,{},&error);QVERIFY(!original.isEmpty());
        connect(&engine,&AgentEngine::toolFinished,&engine,[&](const QString&,const QString&,bool failed){if(!failed)QVERIFY(QFile::remove(original));});
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read the attachment range");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(model.requests.size(),2);
        const auto body=serialized(model.requests.last());QVERIFY2(body.contains("2|EXACT_CAPTURED_SECOND"),body.constData());QVERIFY(!body.contains("EXCLUDED_FIRST"));QVERIFY(!body.contains("EXCLUDED_THIRD"));
        const auto results=toolResults(engine.messages());QCOMPARE(results.size(),1);QCOMPARE(results[0]["content"].toString(),"File opened and loaded into context.");
        const auto metadata=results[0]["metadata"].toObject();QCOMPARE(metadata["fileReads"].toArray().first().toObject()["path"].toString(),ref.path);
        QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("EXACT_CAPTURED_SECOND"));QVERIFY(!QFileInfo::exists(original));
    }
    void directoryManifestSurvivesDeleteAndClone(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto a=store.write("source","one.txt","ONE",&error),b=store.write("source","two.bin",QByteArray::fromHex("00ff01"),&error);QVERIFY(!a.isEmpty()&&!b.isEmpty());
        QVERIFY(store.saveSnapshot("source",a,"SNAPSHOT_HIDDEN",&error));Model model;model.replies={response("openai",{{"list","ls",{{"path","att/"},{"depth",0}}}})};
        QJsonArray history;
        {AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"source",data.path());QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("List blobs");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());history=engine.messages();
        QVERIFY2(host.cloneFileContext("fork",&error),qPrintable(error));QVERIFY2(host.removeFileContext(&error),qPrintable(error));}
        QVERIFY(store.removeAgent("fork",&error)); // Retain only the independently cloned read snapshot.
        AgentEngine fork;fork.setProvider(model.provider());fork.setMessages(history);AgentToolHost host(&fork,nullptr,"fork",data.path());QSignalSpy done(&fork,&AgentEngine::finished);fork.submit("Use prior listing");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(fork.lastTurnSucceeded());
        const auto body=serialized(model.requests.last());QVERIFY(body.contains(a.path.mid(4).toUtf8()));QVERIFY(body.contains(b.path.mid(4).toUtf8()));QVERIFY(body.contains("type:directory"));QVERIFY(!body.contains("SNAPSHOT_HIDDEN"));
        QCOMPARE(toolResults(history)[0]["content"].toString(),"Directory opened and loaded into context.");QVERIFY(!QJsonDocument(history).toJson().contains(a.path.mid(4).toUtf8()));
    }
    void binaryArchiveAndImageUseExistingTransforms_data(){
        QTest::addColumn<QString>("extension");QTest::addColumn<QByteArray>("archiveBytes");
        for(const auto& extension:QStringList{"zip","jar","war"})QTest::newRow(qPrintable(extension))<<extension<<archive();
        QTest::newRow("tar")<<QString("tar")<<tarArchive();QTest::newRow("tgz")<<QString("tgz")<<gzip(tarArchive());
    }
    void binaryArchiveAndImageUseExistingTransforms(){
        QFETCH(QString,extension);QFETCH(QByteArray,archiveBytes);QVERIFY(!archiveBytes.isEmpty());
        QTemporaryDir data;AttachmentStore store(data.path());QString error;
        const auto binary=store.write("chat","raw.bin",QByteArray::fromHex("00ff01424144"),&error),zip=store.write("chat","bundle."+extension,archiveBytes,&error);QVERIFY(!binary.isEmpty()&&!zip.isEmpty());
        QImage image(2,2,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));const auto picture=store.write("chat","image.png",bytes,&error);QVERIFY(!picture.isEmpty());
        Model model;model.replies={response("openai",{{"binary","read",{{"path",binary.path}}},{"archive","read",{{"path",zip.path}}},{"image","read",{{"path",picture.path}}}})};
        AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"chat",data.path());QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Inspect local blobs");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());
        const auto body=serialized(model.requests.last());QVERIFY(body.contains("visible.txt"));QVERIFY(!body.contains("NEVER_EXTRACT_THIS_CONTENT"));QVERIFY(body.contains("image_url"));QVERIFY(body.contains("fs.readFile"));QVERIFY(body.contains(binary.path.toUtf8()));
        QCOMPARE(toolResults(engine.messages()).size(),3);QVERIFY(!QJsonDocument(engine.messages()).toJson().contains("data:image"));
        QVERIFY(!QFileInfo::exists(store.agentDirectory("chat")+"/nested"));
    }
    void readerIdentityIsFixedAndNoWorkspaceIsRequired(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("first","same.txt","FIRST_ONLY",&error);QVERIFY(!ref.isEmpty());
        const auto second=store.agentDirectory("second",&error,true);QVERIFY(write(second+'/'+ref.path.mid(4),"SECOND_ONLY"));
        AttachmentFileBroker one("first",data.path()),two("second",data.path());ToolExecutor first,other;first.setAttachmentReadSource(one.formattedSource());other.setAttachmentReadSource(two.formattedSource());
        const auto a=execute(first,"read",{{"path",ref.path},{"agentId","second"}}),b=execute(other,"read",{{"path",ref.path}});QVERIFY(!a["error"].toBool()&&!b["error"].toBool());QVERIFY(a["output"].toString().contains("FIRST_ONLY"));QVERIFY(!a["output"].toString().contains("SECOND_ONLY"));QVERIFY(b["output"].toString().contains("SECOND_ONLY"));
        QVERIFY(execute(other,"read",{{"path",store.resolve("first",ref,{})}})["error"].toBool());QVERIFY(execute(other,"read",{{"path","missing.txt"}})["error"].toBool());
    }
    void attOnlyDefinitionsDoNotGrantWorkspaceOrAppMutation(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("chat","sample.txt","ATT_ONLY_TEXT",&error);QVERIFY(!ref.isEmpty());Model model;model.replies={response("openai",{{"read","read",{{"path",ref.path}}}})};
        AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(engine.setAppsDirectory({}));
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read without a workspace or apps mount");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());
        QStringList names;for(const auto& value:model.requests.first()["tools"].toArray())names.append(value.toObject()["function"].toObject()["name"].toString());
        QVERIFY(names.contains("read")&&names.contains("ls"));for(const auto& name:QStringList{"write","multiEdit","mkdir","runCommand","grepSearch","glob"})QVERIFY(!names.contains(name));
        QVERIFY(serialized(model.requests.last()).contains("ATT_ONLY_TEXT"));
    }
    void forkedFileHistoryUsesClonedSnapshotNotAnotherChatsBlob(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("source","sample.txt","ORIGINAL_ONE\nORIGINAL_TWO\n",&error);QVERIFY(!ref.isEmpty());Model model;model.replies={response("openai",{{"read","read",{{"path",ref.path},{"start_line",2},{"end_line",2}}}})};QJsonArray history;
        {AgentEngine engine;engine.setProvider(model.provider());AgentToolHost host(&engine,nullptr,"source",data.path());QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Capture original");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());history=engine.messages();QVERIFY(host.cloneFileContext("fork",&error));QVERIFY(host.removeFileContext(&error));}
        const auto forkOriginal=store.resolve("fork",ref,{},&error);QVERIFY(write(forkOriginal,"CHANGED_FORK_BLOB"));
        const auto stranger=store.agentDirectory("stranger",&error,true);QVERIFY(write(stranger+'/'+ref.path.mid(4),"STRANGER_BLOB"));
        AgentEngine fork;fork.setProvider(model.provider());fork.setMessages(history);AgentToolHost forkHost(&fork,nullptr,"fork",data.path());QSignalSpy done(&fork,&AgentEngine::finished);fork.submit("Use original read context");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(fork.lastTurnSucceeded());const auto body=serialized(model.requests.last());QVERIFY(body.contains("2|ORIGINAL_TWO"));QVERIFY(!body.contains("ORIGINAL_ONE"));QVERIFY(!body.contains("CHANGED_FORK_BLOB"));QVERIFY(!body.contains("STRANGER_BLOB"));
        AttachmentPipeline unauthorized{AttachmentStore(data.path())};FileReadSnapshotStore snapshots(data.path());unauthorized.setFileReadLease(snapshots.acquire("stranger"));const auto prepared=unauthorized.prepare(history,"stranger",{}, {},{},&error);QVERIFY(error.isEmpty());QVERIFY(!QJsonDocument(prepared).toJson().contains("ORIGINAL_TWO"));QVERIFY(!QJsonDocument(prepared).toJson().contains("STRANGER_BLOB"));
    }
    void invalidPathsAndMutations_data(){
        QTest::addColumn<QString>("tool");QTest::addColumn<QString>("path");QTest::addColumn<bool>("raw");
        for(const auto& path:QStringList{"att/../second/secret.txt","att//secret.txt","att/second/secret.txt","att/a:stream","att/NUL","att/../../read-context-v1/x","att\\secret.txt"})QTest::newRow(qPrintable(path))<<QString("read")<<path<<false;
        QTest::newRow("ls-file")<<QString("ls")<<QString("att/existing.txt")<<false;
        QTest::newRow("write")<<QString("write")<<QString("att/existing.txt")<<false;
        QTest::newRow("mkdir")<<QString("mkdir")<<QString("att/new")<<false;
        QTest::newRow("raw-unavailable-here")<<QString("read")<<QString("att/existing.txt")<<true;
    }
    void invalidPathsAndMutations(){
        QFETCH(QString,tool);QFETCH(QString,path);QFETCH(bool,raw);QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());QVERIFY(write(broker.rootDirectory()+"/existing.txt","ORIGINAL"));
        ToolExecutor executor;executor.setAttachmentReadSource(broker.formattedSource());const auto result=execute(executor,tool,{{"path",path},{"raw",raw},{"content","MUTATED"}});QVERIFY2(result["error"].toBool(),qPrintable(result["output"].toString()));QVERIFY(result["metadata"].toObject().isEmpty());
        QFile original(broker.rootDirectory()+"/existing.txt");QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),QByteArray("ORIGINAL"));
    }
    void listingRefusesUnexpectedEntries_data(){QTest::addColumn<QString>("name");QTest::addColumn<bool>("directory");QTest::newRow("subdirectory")<<QString("folder")<<true;QTest::newRow("invalid-name")<<QString("with space.txt")<<false;}
    void listingRefusesUnexpectedEntries(){
        QFETCH(QString,name);QFETCH(bool,directory);QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());if(directory)QVERIFY(QDir().mkdir(broker.rootDirectory()+'/'+name));else QVERIFY(write(broker.rootDirectory()+'/'+name,"no"));
        ToolExecutor executor;executor.setAttachmentReadSource(broker.formattedSource());const auto result=execute(executor,"ls",{{"path","att"}});QVERIFY(result["error"].toBool());QVERIFY(result["metadata"].toObject().isEmpty());
    }
    void cancellationDoesNotPublishIntoReusedId(){
        QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());QVERIFY(write(broker.rootDirectory()+"/old.txt","OLD_UNPUBLISHED"));QVERIFY(write(broker.rootDirectory()+"/new.txt","NEW_RESULT"));
        const auto reader=broker.formattedSource();auto gate=std::make_shared<QSemaphore>();auto entered=std::make_shared<std::atomic_bool>(false),exited=std::make_shared<std::atomic_bool>(false);
        ToolExecutor tool;tool.setAttachmentReadSource([reader,gate,entered,exited](const auto& name,const auto& path,const auto& params,auto* capture,const auto* canceled,auto* error){entered->store(true);gate->acquire();const bool ok=reader(name,path,params,capture,canceled,error);exited->store(true);return ok;});
        QSignalSpy done(&tool,&ToolExecutor::completed);tool.execute("same-id","read",{{"path","att/old.txt"}});QTRY_VERIFY_WITH_TIMEOUT(entered->load(),5000);tool.cancel();tool.setAttachmentReadSource(reader);tool.execute("same-id","read",{{"path","att/new.txt"}});gate->release();QTRY_VERIFY(exited->load());QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,5000);
        QVERIFY(done[0][2].toBool());QVERIFY(!done[1][2].toBool());QVERIFY(done[1][1].toString().contains("NEW_RESULT"));QVERIFY(!done[1][1].toString().contains("OLD_UNPUBLISHED"));
    }
    void removedLeaseRejectsAValueReaderStillRunning(){
        QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());QVERIFY(write(broker.rootDirectory()+"/sample.txt","CAPTURED_PRIVATE"));
        FileReadSnapshotStore snapshots(data.path());const auto lease=snapshots.acquire("chat");const auto reader=broker.formattedSource();FileReadCapture capture;QString error;QVERIFY(reader("read","att/sample.txt",{},&capture,nullptr,&error));
        FileReadSnapshotStore::revoke(lease);QVERIFY(FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());
    }
    void persistedAttNamespaceRemainsFlat(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());const auto lease=store.acquire("chat");FileReadCapture capture;capture.tool="read";capture.bytes="content";capture.descriptor={"att/valid.txt","valid.txt",7,{}};QString error;
        QVERIFY(!FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty());
        for(const auto& path:QStringList{"att","att/with space.txt","att/folder/file.txt","att/NUL","att/name.","att/.."}){
            capture.descriptor.mountedPath=path;QVERIFY2(FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty(),qPrintable(path));QVERIFY(!error.isEmpty());
        }
        capture={};capture.tool="ls";capture.directory=true;capture.params.depth=0;capture.capturedDepth=0;capture.descriptor={"att/folder","folder",-1,{}};
        QVERIFY(FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty());capture.descriptor.mountedPath="att";QVERIFY(!FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty());
    }
    void exactBoundsAndEmptyBytes(){
        QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());QVERIFY(write(broker.rootDirectory()+"/empty.txt",{}));QVERIFY(write(broker.rootDirectory()+"/big.bin",QByteArray(4*1024*1024+1,'x')));
        const auto reader=broker.formattedSource();FileReadCapture capture;QString error;QVERIFY(reader("read","att/empty.txt",{},&capture,nullptr,&error));QCOMPARE(capture.descriptor.size,0);QVERIFY(capture.bytes.isEmpty());QVERIFY(!reader("read","att/big.bin",{},&capture,nullptr,&error));QVERIFY(!error.isEmpty());QVERIFY(!reader("read","att/missing.txt",{},&capture,nullptr,nullptr));
    }
    void junctionsAndRootReplacementFailClosed(){
#ifdef Q_OS_WIN
        QTemporaryDir data,outside;AttachmentFileBroker broker("chat",data.path());const auto root=broker.rootDirectory();QVERIFY(write(root+"/sample.txt","OWNED"));QVERIFY(write(outside.filePath("secret.txt"),"FOREIGN_SECRET"));
        const auto junction=[&](const QString& link){QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside.path())});return process.waitForFinished(5000)&&process.exitCode()==0;};
        ToolExecutor tool;tool.setAttachmentReadSource(broker.formattedSource());const auto link=root+"/alias";QVERIFY(junction(link));const auto removeLink=qScopeGuard([&]{QDir().rmdir(link);});
        QVERIFY(execute(tool,"read",{{"path","att/alias/secret.txt"}})["error"].toBool());const auto listed=execute(tool,"ls",{{"path","att"}});QVERIFY(listed["error"].toBool());QVERIFY(!listed["output"].toString().contains("secret.txt"));QVERIFY(QDir().rmdir(link));
        const auto saved=root+"-saved";QVERIFY(QDir().rename(root,saved));const auto restore=qScopeGuard([&]{QDir().rmdir(root);QDir().rename(saved,root);});QVERIFY(junction(root));
        const auto read=execute(tool,"read",{{"path","att/secret.txt"}}),list=execute(tool,"ls",{{"path","att"}});QVERIFY(read["error"].toBool());QVERIFY(list["error"].toBool());QVERIFY(!read["output"].toString().contains("FOREIGN_SECRET"));QVERIFY(!list["output"].toString().contains("secret.txt"));
#else
        QSKIP("Windows junction fixture");
#endif
    }
};
QTEST_GUILESS_MAIN(FormattedAttTest)
#include "test_formatted_att.moc"
