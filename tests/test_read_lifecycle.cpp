#include "attachments/FileReadSnapshotStore.h"
#include "attachments/AttachmentPipeline.h"
#include "core/ToolExecutor.h"
#include "core/ProviderProtocol.h"
#include <QBuffer>
#include <QImage>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
bool write(const QString& path,const QByteArray& body){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(body)==body.size();}
FileReadCapture file(const QByteArray& bytes=QByteArray("first\nsecond\nthird\n")){
    FileReadCapture result;result.tool="read";result.descriptor={"ws1/example.txt","example.txt",bytes.size(),{}};result.bytes=bytes;
    result.params.startLine=2;result.params.endLine=3;return result;
}
QJsonObject completedMeta(const QSignalSpy& spy,int index=0){return spy[index][3].toJsonObject();}
}
class ReadLifecycleTest:public QObject{
    Q_OBJECT
private slots:
    void filesRangesRestartAndDedup(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());QString error;const auto lease=store.acquire("chat",&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        auto input=file();input.descriptor.modified=QDateTime::fromString("2026-11-05T01:02:03.000Z",Qt::ISODateWithMs);
        const auto meta=FileReadSnapshotStore::commit(lease,input,nullptr,&error);QVERIFY2(!meta.isEmpty(),qPrintable(error));
        QCOMPARE(FileReadSnapshotStore::commit(lease,input,nullptr,&error),meta);
        const auto directory=data.path()+"/read-context-v1/attachment-snapshots/chat";QCOMPARE(QDir(directory).entryList({"*.bin"},QDir::Files).size(),1);
        FileReadSnapshotStore restarted(data.path());FileReadCapture restored;
        QVERIFY2(FileReadSnapshotStore::read(restarted.acquire("chat"),meta,&restored,&error),qPrintable(error));
        QCOMPARE(restored.bytes,input.bytes);QCOMPARE(restored.params,input.params);QCOMPARE(restored.descriptor.modified,input.descriptor.modified);
        const auto transformed=FileTransformer::fromBytes(restored.bytes,restored.descriptor,restored.params);
        const auto text=transformed.parts.first().toObject()["text"].toString();QVERIFY(text.contains("2|second"));QVERIFY(!text.contains("1|first"));
        QVERIFY(!QJsonDocument(meta).toJson().contains(input.bytes));QVERIFY(!meta.contains("content"));
    }
    void emptyFileAndDistinctVersions(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");QString error;auto first=file(QByteArray(""));first.params={};
        const auto empty=FileReadSnapshotStore::commit(lease,first,nullptr,&error);QVERIFY2(!empty.isEmpty(),qPrintable(error));FileReadCapture restored;
        QVERIFY2(FileReadSnapshotStore::read(lease,empty,&restored,&error),qPrintable(error));QVERIFY(restored.bytes.isEmpty());
        const auto second=FileReadSnapshotStore::commit(lease,file("replacement"),nullptr,&error);QVERIFY(!second.isEmpty());
        QVERIFY(empty["pathReferences"]!=second["pathReferences"]);QVERIFY(FileReadSnapshotStore::read(lease,empty,&restored,&error));QVERIFY(restored.bytes.isEmpty());
    }
    void directoryRootDepthAndStableOrdering(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");QString error;
        FileReadCapture input;input.tool="ls";input.directory=true;input.descriptor={"apps",{},-1,{}};input.params.depth=1;input.capturedDepth=1;
        input.entries={{"z.txt",false,{},{}},{"dir",true,{{"child",false,{},{}}},{}}};
        const auto meta=FileReadSnapshotStore::commit(lease,input,nullptr,&error);QVERIFY2(!meta.isEmpty(),qPrintable(error));
        std::reverse(input.entries.begin(),input.entries.end());QCOMPARE(FileReadSnapshotStore::commit(lease,input,nullptr,&error),meta);
        FileReadCapture restored;QVERIFY2(FileReadSnapshotStore::read(lease,meta,&restored,&error),qPrintable(error));QVERIFY(restored.directory);QCOMPARE(restored.capturedDepth,1);
        const auto text=FileTransformer::fromDirectory(restored.entries,restored.descriptor,restored.params).parts.first().toObject()["text"].toString();QVERIFY(text.contains("child"));
        input.capturedDepth=0;input.params.depth=0;QVERIFY(FileReadSnapshotStore::commit(lease,input,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());
    }
    void isolationCloneRemoveAndLeaseEpoch(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto source=store.acquire("source");QString error;const auto meta=FileReadSnapshotStore::commit(source,file(),nullptr,&error);QVERIFY(!meta.isEmpty());FileReadCapture result;
        QVERIFY(!FileReadSnapshotStore::read(store.acquire("other"),meta,&result,&error));
        QVERIFY2(store.cloneAgent("source","copy",&error),qPrintable(error));auto copy=store.acquire("copy");QVERIFY(FileReadSnapshotStore::read(copy,meta,&result,&error));
        QVERIFY(store.removeAgent("source",&error));QVERIFY(FileReadSnapshotStore::commit(source,file(),nullptr,&error).isEmpty());
        QVERIFY(!QFileInfo::exists(data.path()+"/read-context-v1/attachment-snapshots/source"));QVERIFY(FileReadSnapshotStore::read(copy,meta,&result,&error));
        auto replacement=store.acquire("source");QVERIFY(!FileReadSnapshotStore::commit(replacement,file(),nullptr,&error).isEmpty());QVERIFY(FileReadSnapshotStore::commit(source,file(),nullptr,&error).isEmpty());
    }
    void malformedMetadataAndCorruptSnapshot(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");QString error;const auto meta=FileReadSnapshotStore::commit(lease,file(),nullptr,&error);FileReadCapture out;
        auto changed=meta;auto request=changed["fileReads"].toArray().first().toObject();request["version"]=2;changed["fileReads"]=QJsonArray{request};QVERIFY(!FileReadSnapshotStore::read(lease,changed,&out,&error));
        changed=meta;auto descriptors=changed["pathMetadata"].toObject(),details=descriptors["ws1/example.txt"].toObject();details["size"]=99;descriptors["ws1/example.txt"]=details;changed["pathMetadata"]=descriptors;QVERIFY(!FileReadSnapshotStore::read(lease,changed,&out,&error));
        const auto folder=data.path()+"/read-context-v1/attachment-snapshots/chat";const auto files=QDir(folder).entryList({"*.bin"},QDir::Files);QCOMPARE(files.size(),1);QVERIFY(write(folder+'/'+files.first(),"CORRUPT"));
        QVERIFY(!FileReadSnapshotStore::read(lease,meta,&out,&error));QVERIFY(FileReadSnapshotStore::commit(lease,file(),nullptr,&error).isEmpty());
    }
    void preCanceledAndRevoked(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");QString error;std::atomic_bool cancel=true;
        QVERIFY(FileReadSnapshotStore::commit(lease,file(),&cancel,&error).isEmpty());QVERIFY(!QFileInfo::exists(data.path()+"/read-context-v1"));
        FileReadSnapshotStore::revoke(lease);cancel=false;QVERIFY(FileReadSnapshotStore::commit(lease,file(),&cancel,&error).isEmpty());
    }
    void failedClonePreservesDestinationLease(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto source=store.acquire("source"),target=store.acquire("target");QString error;
        QVERIFY(!FileReadSnapshotStore::commit(source,file("source"),nullptr,&error).isEmpty());
        const auto original=FileReadSnapshotStore::commit(target,file("target"),nullptr,&error);QVERIFY(!original.isEmpty());
        QVERIFY(!store.cloneAgent("source","target",&error));FileReadCapture restored;
        QVERIFY2(FileReadSnapshotStore::read(target,original,&restored,&error),qPrintable(error));QCOMPARE(restored.bytes,QByteArray("target"));
    }
    void emptySourceCannotCloneIntoPopulatedDestination(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto target=store.acquire("target");QString error;
        const auto original=FileReadSnapshotStore::commit(target,file("target"),nullptr,&error);QVERIFY(!original.isEmpty());
        QVERIFY(!store.cloneAgent("empty-source","target",&error));FileReadCapture restored;
        QVERIFY(FileReadSnapshotStore::read(target,original,&restored,&error));QCOMPARE(restored.bytes,QByteArray("target"));
    }
    void staleRevocationCannotInvalidateReplacementLease(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto old=store.acquire("chat");QString error;
        FileReadSnapshotStore::revoke(old);auto replacement=store.acquire("chat");
        FileReadSnapshotStore::revoke(old);
        QVERIFY2(!FileReadSnapshotStore::commit(replacement,file(),nullptr,&error).isEmpty(),qPrintable(error));
        QVERIFY(FileReadSnapshotStore::commit(old,file(),nullptr,&error).isEmpty());
    }
    void windowsAgentCaseAliasesShareAuthority(){
#ifdef Q_OS_WIN
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto upper=store.acquire("Chat"),lower=store.acquire("chat");QString error;
        QVERIFY(!FileReadSnapshotStore::commit(upper,file(),nullptr,&error).isEmpty());
        FileReadSnapshotStore::revoke(upper);QVERIFY(FileReadSnapshotStore::commit(lower,file(),nullptr,&error).isEmpty());
        lower=store.acquire("chat");QVERIFY(!FileReadSnapshotStore::commit(lower,file(),nullptr,&error).isEmpty());
        QVERIFY(!store.cloneAgent("Chat","chat",&error));QVERIFY(!error.isEmpty());
        QVERIFY(store.removeAgent("CHAT",&error));QVERIFY(FileReadSnapshotStore::commit(lower,file(),nullptr,&error).isEmpty());
#else
        QSKIP("Windows case-insensitive directory identity");
#endif
    }
    void sameCapturedBytesDespiteSourceDeletionAndLegacySlot(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("example.txt"),"one\ntwo\nthree\n"));FileReadSnapshotStore store(data.path());const auto lease=store.acquire("chat");
        ToolExecutor tools;QVERIFY(tools.setWorkspace(workspace.path()));int legacyCalls=0;
        connect(&tools,&ToolExecutor::completed,this,[&](QString,QString,bool){++legacyCalls;});QSignalSpy spy(&tools,&ToolExecutor::completed);
        tools.setFileReadCommitter([lease,path=workspace.filePath("example.txt")](const auto& capture,const auto* cancel,QString* error){QFile::remove(path);return FileReadSnapshotStore::commit(lease,capture,cancel,error);});
        tools.execute("call","read",{{"path","w1/example.txt"},{"start_line",2},{"end_line",2}});QTRY_COMPARE(spy.size(),1);QCOMPARE(legacyCalls,1);QVERIFY(!spy[0][2].toBool());
        QVERIFY(!QFileInfo::exists(workspace.filePath("example.txt")));QVERIFY(!spy[0][1].toString().contains("two"));FileReadCapture out;QString error;
        QVERIFY2(FileReadSnapshotStore::read(lease,completedMeta(spy),&out,&error),qPrintable(error));QCOMPARE(out.bytes,QByteArray("one\ntwo\nthree\n"));QCOMPARE(out.descriptor.mountedPath,QString("ws1/example.txt"));QCOMPARE(out.params.startLine.value_or(0),2);
    }
    void rawReadsAndStandaloneInlineRemainCompatible(){
        QTemporaryDir workspace;QVERIFY(write(workspace.filePath("raw.txt"),"hello\n"));ToolExecutor tools;QVERIFY(tools.setWorkspace(workspace.path()));QSignalSpy spy(&tools,&ToolExecutor::completed);
        tools.execute("inline","read",{{"path","raw.txt"}});QTRY_COMPARE(spy.size(),1);QVERIFY(spy[0][1].toString().contains("1|hello"));QVERIFY(completedMeta(spy).isEmpty());
        std::atomic_int callbacks=0;tools.setFileReadCommitter([&](const auto&,const auto*,QString*){++callbacks;return QJsonObject{};});
        tools.execute("raw","read",{{"path","raw.txt"},{"raw",true}});QTRY_COMPARE(spy.size(),2);QCOMPARE(spy[1][1].toString(),QString("hello\n"));QCOMPARE(callbacks.load(),0);QVERIFY(completedMeta(spy,1).isEmpty());
        tools.execute("bytes","readBytes",{{"path","raw.txt"}});QTRY_COMPARE(spy.size(),3);QCOMPARE(callbacks.load(),0);QVERIFY(completedMeta(spy,2).isEmpty());
    }
    void cancelThenReuseToolIdDiscardsOldMetadata(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("a.txt"),"a"));FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");ToolExecutor tools;QVERIFY(tools.setWorkspace(workspace.path()));QSignalSpy spy(&tools,&ToolExecutor::completed);
        auto gate=std::make_shared<QSemaphore>();auto entered=std::make_shared<std::atomic_bool>(false);auto done=std::make_shared<std::atomic_bool>(false);
        tools.setFileReadCommitter([lease,gate,entered,done](const auto& capture,const auto* cancel,QString* error){entered->store(true);gate->acquire();const auto result=FileReadSnapshotStore::commit(lease,capture,cancel,error);done->store(true);return result;});
        tools.execute("same","read",{{"path","a.txt"}});QTRY_VERIFY(entered->load());tools.cancel();QCOMPARE(spy.size(),1);QVERIFY(spy[0][2].toBool());
        tools.execute("same","read",{{"path","a.txt"},{"raw",true}});QTRY_COMPARE(spy.size(),2);gate->release();QTRY_VERIFY(done->load());QTest::qWait(20);QCOMPARE(spy.size(),2);QVERIFY(completedMeta(spy,0).isEmpty());QVERIFY(completedMeta(spy,1).isEmpty());
    }
    void ownerDeletionAndRevocationNeverRecreatesChat(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("a.txt"),"a"));FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");auto* tools=new ToolExecutor;QVERIFY(tools->setWorkspace(workspace.path()));
        auto gate=std::make_shared<QSemaphore>();auto entered=std::make_shared<std::atomic_bool>(false);auto done=std::make_shared<std::atomic_bool>(false);
        tools->setFileReadCommitter([lease,gate,entered,done](const auto& capture,const auto* cancel,QString* error){entered->store(true);gate->acquire();const auto result=FileReadSnapshotStore::commit(lease,capture,cancel,error);done->store(true);return result;});
        tools->execute("old","read",{{"path","a.txt"}});QTRY_VERIFY(entered->load());delete tools;QString error;QVERIFY(store.removeAgent("chat",&error));gate->release();QTRY_VERIFY(done->load());QVERIFY(!QFileInfo::exists(data.path()+"/read-context-v1/attachment-snapshots/chat"));
    }
    void pipelinePerCallRangesOrderingAndHistoryIdentity(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");AttachmentPipeline pipeline{AttachmentStore(data.path())};pipeline.setFileReadLease(lease);QString error;
        QJsonArray calls,results;
        for(int index=0;index<3;++index){
            const QString id="call"+QString::number(index);const QJsonObject args{{"path","ws1/example.txt"},{"start_line",index==1?3:1},{"end_line",index==1?3:1}};
            auto capture=file("first\nsecond\nthird\n");QVERIFY(FileTransformer::readParams(args,&capture.params,&error));capture.inputHash=fileReadInputHash("read",args);
            const auto meta=FileReadSnapshotStore::commit(lease,capture,nullptr,&error);QVERIFY2(!meta.isEmpty(),qPrintable(error));
            calls.append(QJsonObject{{"id",id},{"type","function"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}});
            results.append(QJsonObject{{"role","tool"},{"tool_call_id",id},{"content","File opened and loaded into context."},{"metadata",meta}});
        }
        QJsonArray history{QJsonObject{{"role","user"},{"content","read ranges"}},QJsonObject{{"role","assistant"},{"tool_calls",calls}}};for(const auto& result:results)history.append(result);
        const auto original=QJsonDocument(history).toJson(QJsonDocument::Compact);
        const auto prepared=pipeline.prepare(history,"chat",{}, {},{},&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(prepared.size(),6);QCOMPARE(prepared[2].toObject()["tool_call_id"].toString(),"call0");QCOMPARE(prepared[4].toObject()["tool_call_id"].toString(),"call2");
        const auto parts=prepared.last().toObject()["content"].toArray();QCOMPARE(parts.size(),3);QVERIFY(parts[0].toObject()["text"].toString().contains("1|first"));QVERIFY(parts[1].toObject()["text"].toString().contains("3|third"));QVERIFY(parts[2].toObject()["text"].toString().contains("1|first"));
        QVERIFY(!parts[0].toObject()["text"].toString().contains("2|second"));QCOMPARE(QJsonDocument(history).toJson(QJsonDocument::Compact),original);
        auto corrupted=history;auto tool=corrupted[2].toObject();auto meta=tool["metadata"].toObject();auto refs=meta["fileReads"].toArray();auto read=refs[0].toObject();read["params"]=QJsonObject{{"startLine",2},{"endLine",2}};refs[0]=read;meta["fileReads"]=refs;tool["metadata"]=meta;corrupted[2]=tool;
        const auto bad=pipeline.prepare(corrupted,"chat",{}, {},{},&error);QVERIFY(bad.last().toObject()["content"].toArray().first().toObject()["text"].toString().contains("does not match"));
    }
    void imageReadAndFourProtocolToolAssociations(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");AttachmentPipeline pipeline{AttachmentStore(data.path())};pipeline.setFileReadLease(lease);QString error;
        QImage image(3,2,QImage::Format_RGB32);image.fill(Qt::red);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));
        const QJsonObject args{{"path","apps/image.png"}};auto capture=file(bytes);capture.descriptor.mountedPath="apps/image.png";capture.descriptor.originalFileName="image.png";capture.params={};capture.inputHash=fileReadInputHash("read",args);
        const auto meta=FileReadSnapshotStore::commit(lease,capture,nullptr,&error);QVERIFY(!meta.isEmpty());
        const QJsonArray calls{QJsonObject{{"id","image-call"},{"type","function"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}},
            QJsonObject{{"id","sibling"},{"type","function"},{"function",QJsonObject{{"name","write"},{"arguments","{}"}}}}};
        const QJsonArray history{QJsonObject{{"role","assistant"},{"tool_calls",calls}},QJsonObject{{"role","tool"},{"tool_call_id","image-call"},{"content","File opened and loaded into context."},{"metadata",meta}},QJsonObject{{"role","tool"},{"tool_call_id","sibling"},{"content","Error: denied"}}};
        ModelMediaCapabilities caps;caps.image=true;caps.imageMimeTypes={"image/png"};const auto prepared=pipeline.prepare(history,"chat",{},caps,{},&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(prepared.size(),4);QCOMPARE(prepared.last().toObject()["content"].toArray()[1].toObject()["type"].toString(),"image");
        QVERIFY(!QJsonDocument(history).toJson().contains("dataBase64"));
        for(const auto& protocol:QStringList{"openai","responses","anthropic","google"}){
            ProviderConfig provider;provider.id="fixture";provider.model="fixture-image";provider.protocol=protocol;provider.baseUrl="http://127.0.0.1:1/v1";
            const auto body=ProviderProtocol::prepare(provider,prepared,"fixture",{}).body;const auto serialized=QJsonDocument(body).toJson(QJsonDocument::Compact);
            QVERIFY2(serialized.contains("image-call")||protocol=="google",qPrintable(protocol));QVERIFY(serialized.contains("File opened and loaded into context."));QVERIFY(serialized.contains("Error: denied"));QVERIFY(!serialized.contains("pathReferences"));
            if(protocol=="openai"){const auto messages=body["messages"].toArray();QCOMPARE(messages[2].toObject()["tool_call_id"].toString(),"image-call");QCOMPARE(messages[3].toObject()["tool_call_id"].toString(),"sibling");QCOMPARE(messages[4].toObject()["content"].toArray()[1].toObject()["type"].toString(),"image_url");}
            if(protocol=="responses"){const auto input=body["input"].toArray();QCOMPARE(input[2].toObject()["call_id"].toString(),"image-call");QCOMPARE(input[3].toObject()["call_id"].toString(),"sibling");QCOMPARE(input[4].toObject()["content"].toArray()[1].toObject()["type"].toString(),"input_image");}
            if(protocol=="anthropic"){const auto content=body["messages"].toArray()[1].toObject()["content"].toArray();QCOMPARE(content[0].toObject()["tool_use_id"].toString(),"image-call");QCOMPARE(content[1].toObject()["tool_use_id"].toString(),"sibling");QCOMPARE(content[3].toObject()["type"].toString(),"image");}
            if(protocol=="google"){const auto parts=body["contents"].toArray()[1].toObject()["parts"].toArray();QCOMPARE(parts[0].toObject()["functionResponse"].toObject()["name"].toString(),"read");QCOMPARE(parts[1].toObject()["functionResponse"].toObject()["name"].toString(),"write");QVERIFY(parts[3].toObject().contains("inlineData"));}
        }
        caps.image=false;const auto noImage=pipeline.prepare(history,"chat",{},caps,{},&error);QVERIFY(noImage.last().toObject()["content"].toArray().first().toObject()["text"].toString().contains("not available to this model"));
    }
    void pipelineLeaseCannotCrossChatIdentity(){
        QTemporaryDir data;FileReadSnapshotStore store(data.path());auto lease=store.acquire("first");AttachmentPipeline pipeline{AttachmentStore(data.path())};pipeline.setFileReadLease(lease);QString error;
        const QJsonObject args{{"path","ws1/example.txt"}};auto capture=file("private-first-only");capture.params={};capture.inputHash=fileReadInputHash("read",args);const auto metadata=FileReadSnapshotStore::commit(lease,capture,nullptr,&error);
        const QJsonArray history{QJsonObject{{"role","assistant"},{"tool_calls",QJsonArray{QJsonObject{{"id","one"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}}}},QJsonObject{{"role","tool"},{"tool_call_id","one"},{"content","opened"},{"metadata",metadata}}};
        const auto wrong=pipeline.prepare(history,"second",{}, {},{},&error);QVERIFY(!QJsonDocument(wrong).toJson().contains("private-first-only"));
        const auto correct=pipeline.prepare(history,"first",{}, {},{},&error);QVERIFY(QJsonDocument(correct).toJson().contains("private-first-only"));
    }
    void explicitReadThenUserMentionDedupAndCompressionWindow(){
        QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("example.txt"),"first\nsecond\nthird\n"));FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");AttachmentPipeline pipeline{AttachmentStore(data.path())};pipeline.setFileReadLease(lease);QString error;
        const QJsonObject args{{"path","ws1/example.txt"}};auto capture=file();capture.params={};capture.inputHash=fileReadInputHash("read",args);const auto readMeta=FileReadSnapshotStore::commit(lease,capture,nullptr,&error);
        const AttachmentMounts mounts{{"ws1",workspace.path()}};const auto userMeta=pipeline.capture("chat",QJsonArray{QJsonObject{{"path","ws1/example.txt"}}},mounts,&error);QVERIFY2(!userMeta.isEmpty(),qPrintable(error));
        QJsonObject user{{"role","user"},{"content","same file"},{"metadata",userMeta}};
        QJsonArray history{QJsonObject{{"role","assistant"},{"tool_calls",QJsonArray{QJsonObject{{"id","one"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}}}}}}},QJsonObject{{"role","tool"},{"tool_call_id","one"},{"content","opened"},{"metadata",readMeta}},user};
        const auto prepared=pipeline.prepare(history,"chat",mounts,{}, {},&error);QCOMPARE(prepared.size(),4);QVERIFY(prepared[2].toObject()["content"].isArray());QCOMPARE(prepared[3].toObject()["content"].toString(),"same file");
        const auto newWindow=pipeline.prepare(QJsonArray{user,user},"chat",mounts,{}, {},&error);QVERIFY(newWindow[0].toObject()["content"].isArray());QCOMPARE(newWindow[1].toObject()["content"].toString(),"same file");
        QVERIFY(write(workspace.filePath("example.txt"),"new bytes"));user["metadata"]=pipeline.capture("chat",QJsonArray{QJsonObject{{"path","ws1/example.txt"}}},mounts,&error);history.append(user);const auto changed=pipeline.prepare(history,"chat",mounts,{}, {},&error);QVERIFY(changed.last().toObject()["content"].isArray());
    }
    void snapshotDirectoryPinsAndJunctionRefusal(){
#ifdef Q_OS_WIN
        QTemporaryDir data,outside;FileReadSnapshotStore store(data.path());auto lease=store.acquire("chat");QString error;QVERIFY(!FileReadSnapshotStore::commit(lease,file(),nullptr,&error).isEmpty());
        const auto directory=data.path()+"/read-context-v1/attachment-snapshots/chat";QVERIFY(!QDir().rename(directory,directory+"-moved"));
        QVERIFY(store.removeAgent("chat",&error));QVERIFY(!QFileInfo::exists(directory));
        QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(directory),QDir::toNativeSeparators(outside.path())});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
        const auto cleanup=qScopeGuard([&]{QDir().rmdir(directory);});
        auto fresh=store.acquire("chat");QVERIFY(FileReadSnapshotStore::commit(fresh,file(),nullptr,&error).isEmpty());QVERIFY(QDir(outside.path()).entryList(QDir::Files).isEmpty());
#else
        QSKIP("Windows handle pin fixture");
#endif
    }
};
QTEST_GUILESS_MAIN(ReadLifecycleTest)
#include "test_read_lifecycle.moc"
