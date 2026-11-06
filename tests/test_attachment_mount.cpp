#include "runtime/AgentToolHost.h"
#include "runtime/AttachmentFileBroker.h"
#include "core/AgentEngine.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ade;
namespace {
struct Result {QString output;bool error=true;QString value()const{return QJsonDocument::fromJson(output.toUtf8()).object()["result"].toString();}};
QString quote(const QString& value){const auto array=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);return QString::fromUtf8(array.mid(1,array.size()-2));}
Result run(AgentToolHost& host,const QString& script,const QString& id="fixture"){
    QSignalSpy results(host.sandbox(),&SandboxHost::completed);host.sandbox()->execute(id,"executeSandboxJs",{{"explanation","Controlled attachment mount fixture"},{"script",script}});
    QElapsedTimer elapsed;elapsed.start();while(results.isEmpty()&&elapsed.elapsed()<7000)results.wait(50);
    return results.isEmpty()?Result{"No sandbox completion",true}:Result{results.last()[1].toString(),results.last()[2].toBool()};
}
bool write(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
}
class AttachmentMountTests final:public QObject {
    Q_OBJECT
private slots:
    void flatListingValidatesCapturedNames_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<bool>("directory");QTest::addColumn<bool>("accepted");
        QTest::newRow("normal")<<QString("valid_blob-1.bin")<<false<<true;
        QTest::newRow("dotfile")<<QString(".hidden")<<false<<true;
        QTest::newRow("space")<<QString("with space.txt")<<false<<false;
        QTest::newRow("unicode")<<QString::fromUtf8("unicode-é.txt")<<false<<false;
        QTest::newRow("directory")<<QString("folder")<<true<<false;
    }
    void flatListingValidatesCapturedNames(){
        QFETCH(QString,name);QFETCH(bool,directory);QFETCH(bool,accepted);
        QTemporaryDir data;AttachmentFileBroker broker("chat",data.path());const auto path=broker.rootDirectory()+'/'+name;
        QVERIFY(!broker.rootDirectory().isEmpty());if(directory)QVERIFY(QDir().mkdir(path));else QVERIFY(write(path,"fixture"));
        QSignalSpy completed(&broker,&AttachmentFileBroker::completed);broker.execute("listing","chat","readDirectory",{{"path","att"},{"recursive",true}});
        QTRY_COMPARE_WITH_TIMEOUT(completed.size(),1,3000);QCOMPARE(completed.first()[2].toBool(),!accepted);
        if(accepted)QCOMPARE(QJsonDocument::fromJson(completed.first()[1].toString().toUtf8()).array(),QJsonArray{name});
        else QVERIFY(!completed.first()[1].toString().contains(name));
    }

    void noWorkspaceReadsOriginalAndGeneratedBytes(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const QByteArray original=QByteArray::fromHex("00ff80efbbbf410d0a");
        const auto ref=store.write("chat","source.bin",original,&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));
        QVERIFY(store.saveSnapshot("chat",ref,"old snapshot bytes",&error));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"chat",data.path());QVERIFY(engine.workspace().isEmpty());
        auto result=run(host,"const bytes=await fsPromises.readFile("+quote(ref.path)+");return [Buffer.isBuffer(bytes),bytes.toString('base64')];");QVERIFY2(!result.error,qPrintable(result.output));
        QCOMPARE(QJsonDocument::fromJson(result.value().toUtf8()).array(),QJsonArray({true,QString::fromLatin1(original.toBase64())}));
        result=run(host,"const name=await API.createAttachment('made.bin',Buffer.from([0,254,127]));const bytes=await new Promise((resolve,reject)=>fs.readFile('att/'+name,(e,v)=>e?reject(e):resolve(v)));return {name,bytes:Array.from(bytes),files:(await fsPromises.readdir('att',{recursive:true})).sort()};");
        QVERIFY2(!result.error,qPrintable(result.output));const auto object=QJsonDocument::fromJson(result.value().toUtf8()).object();
        QCOMPARE(object["bytes"].toArray(),QJsonArray({0,254,127}));const auto names=object["files"].toArray();QCOMPARE(names.size(),2);QVERIFY(names.contains(ref.path.mid(4)));QVERIFY(names.contains(object["name"].toString()));
        QCOMPARE(store.read("chat",AttachmentRef{"att/"+object["name"].toString(),{}},{},&error),QByteArray::fromHex("00fe7f"));QVERIFY(error.isEmpty());
        result=run(host,"await fsPromises.writeFile('apps/copied.bin',await fsPromises.readFile("+quote(ref.path)+"));return 1;");QVERIFY2(!result.error,qPrintable(result.output));
        QCOMPARE(read(host.apps()->agentAppsDirectory()+"/copied.bin"),original);
    }

    void textAndEmptyFilesAreExactAndLargeReadsReject(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const QByteArray text=QByteArray::fromHex("efbbbf")+"line\r\nnext\n";
        const auto textRef=store.write("chat","text.txt",text,&error),empty=store.write("chat","empty.bin",{},&error),bad=store.write("chat","bad.bin",QByteArray::fromHex("e282"),&error);
        QVERIFY(!textRef.isEmpty()&&!empty.isEmpty()&&!bad.isEmpty());AgentEngine engine;AgentToolHost host(&engine,nullptr,"chat",data.path());
        auto result=run(host,"return await fsPromises.readFile("+quote(textRef.path)+",'utf8');");QVERIFY2(!result.error,qPrintable(result.output));QCOMPARE(result.value().toUtf8(),text);
        result=run(host,"return [(await fsPromises.readFile("+quote(empty.path)+")).length,await fsPromises.readFile("+quote(empty.path)+",'utf8')];");QVERIFY(!result.error);QCOMPARE(result.value(),"[0,\"\"]");
        result=run(host,"return await fsPromises.readFile("+quote(bad.path)+",'utf8');");QVERIFY(result.error);QVERIFY(result.output.contains("valid UTF-8"));
        const auto large=store.write("chat","large.bin",QByteArray(4*1024*1024+1,'x'),&error);QVERIFY(!large.isEmpty());
        result=run(host,"return await fsPromises.readFile("+quote(large.path)+");");QVERIFY(result.error);QVERIFY(result.output.contains("bound"));
        const auto longText=store.write("chat","large.txt",QByteArray(65537,'a'),&error);QVERIFY(!longText.isEmpty());
        result=run(host,"return await fsPromises.readFile("+quote(longText.path)+",'utf8');");QVERIFY(result.error);QVERIFY(result.output.contains("no truncated"));
        QCOMPARE(run(host,"return 42;").value(),"42");
    }

    void mutationsAndEscapePathsNeverTouchBlobs(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("chat","original.bin","unchanged",&error);QVERIFY(!ref.isEmpty());
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"chat",data.path());QSignalSpy ordinaryFiles(host.sandbox(),&SandboxHost::fileRequest);
        for(const auto& script:QStringList{
            "await fsPromises.writeFile("+quote(ref.path)+",'bad');",
            "await fsPromises.writeFile('att/new.bin',Buffer.from([0,255]));",
            "await fsPromises.mkdir('att/new',{recursive:true});",
            "return await fsPromises.readFile('att/../outside.bin');",
            "return await fsPromises.readFile('att/'+"+quote(ref.path.mid(4))+"+':stream');",
            "return await fsPromises.readFile('att/NUL');",
            "return await fsPromises.readFile('att/../../attachment-snapshots/chat/private.bin');",
            "return await fsPromises.readdir('att/..');"}){
            const auto result=run(host,script);QVERIFY2(result.error,qPrintable(script+" => "+result.output));
        }
        QCOMPARE(ordinaryFiles.count(),0);QCOMPARE(store.read("chat",ref,{},&error),QByteArray("unchanged"));QVERIFY(error.isEmpty());
        QVERIFY(!QFileInfo::exists(store.agentDirectory("chat")+"/new.bin"));QVERIFY(!QFileInfo::exists(store.agentDirectory("chat")+"/new"));
    }

    void agentIdentityAndSnapshotsAreIsolated(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto a=store.write("first","same.txt","first bytes",&error);QVERIFY(!a.isEmpty());
        const auto secondRoot=store.agentDirectory("second",&error,true);QVERIFY(!secondRoot.isEmpty());QVERIFY(write(secondRoot+'/'+a.path.mid(4),"second bytes"));
        QVERIFY(store.saveSnapshot("first",a,"private history snapshot",&error));
        AgentEngine firstEngine,secondEngine;AgentToolHost first(&firstEngine,nullptr,"first",data.path()),second(&secondEngine,nullptr,"second",data.path());
        QCOMPARE(run(first,"return await fsPromises.readFile("+quote(a.path)+",'utf8');").value(),"first bytes");
        QCOMPARE(run(second,"return await fsPromises.readFile("+quote(a.path)+",'utf8');").value(),"second bytes");
        const auto absolute=store.resolve("first",a,{},&error);QVERIFY(!absolute.isEmpty());QVERIFY(run(second,"return await fsPromises.readFile("+quote(absolute)+");").error);
        auto* broker=first.findChild<AttachmentFileBroker*>();QVERIFY(broker);QSignalSpy completed(broker,&AttachmentFileBroker::completed);
        broker->execute("forged","second","readBytes",{{"path",a.path}});QCOMPARE(completed.count(),1);QVERIFY(completed.first()[2].toBool());QVERIFY(!completed.first()[1].toString().contains("bytes"));
        const auto listing=run(first,"return await fsPromises.readdir('att');");QVERIFY(!listing.error);QCOMPARE(QJsonDocument::fromJson(listing.value().toUtf8()).array(),QJsonArray({a.path.mid(4)}));
    }

    void cancellationBeforeQueuedReadAndLateRepliesIsolated(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("first","private.txt","first private",&error),otherRef=store.write("second","other.txt","second survives",&error);QVERIFY(!ref.isEmpty()&&!otherRef.isEmpty());
        AgentEngine firstEngine,secondEngine;AgentToolHost first(&firstEngine,nullptr,"first",data.path()),second(&secondEngine,nullptr,"second",data.path());
        auto* broker=first.findChild<AttachmentFileBroker*>();QVERIFY(broker);QSignalSpy canceled(broker,&AttachmentFileBroker::completed);QString rpc;
        const auto connection=connect(first.sandbox(),&SandboxHost::attachmentFileRequest,&first,[&](const QString& id){rpc=id;first.sandbox()->cancel("cancel-read");});
        const auto result=run(first,"return await fsPromises.readFile("+quote(ref.path)+",'utf8');","cancel-read");QVERIFY(result.error);QVERIFY(!rpc.isEmpty());QCOMPARE(canceled.count(),1);QVERIFY(canceled.first()[2].toBool());QVERIFY(!canceled.first()[1].toString().contains("first private"));
        disconnect(connection);first.sandbox()->completeFileRequest(rpc,"stale private bytes",false);
        QCOMPARE(run(second,"return await fsPromises.readFile("+quote(otherRef.path)+",'utf8');").value(),"second survives");
        QCOMPARE(run(first,"return 42;","cancel-read").value(),"42");QCOMPARE(canceled.count(),1);
    }

    void brokerCancellationGenerationAndReentrantDestruction(){
        QTemporaryDir data;AttachmentStore store(data.path());QString error;const auto ref=store.write("chat","a.txt","value",&error);QVERIFY(!ref.isEmpty());
        AttachmentFileBroker broker("chat",data.path());QSignalSpy done(&broker,&AttachmentFileBroker::completed);
        broker.execute("reuse","chat","readFile",{{"path",ref.path}});broker.cancel("reuse");broker.execute("reuse","chat","readFile",{{"path",ref.path}});
        QTRY_COMPARE_WITH_TIMEOUT(done.count(),2,3000);QVERIFY(done[0][2].toBool());QVERIFY(!done[1][2].toBool());QCOMPARE(done[1][1].toString(),"value");
        auto* doomed=new AttachmentFileBroker("chat",data.path());QPointer<AttachmentFileBroker> guard(doomed);
        doomed->execute("one","chat","readFile",{{"path",ref.path}});doomed->execute("two","chat","readFile",{{"path",ref.path}});
        connect(doomed,&AttachmentFileBroker::completed,this,[doomed]{delete doomed;});doomed->cancelAll();QVERIFY(!guard);QTest::qWait(20);
    }

    void invalidOwnedRootCannotUseGlobalArtifactFallback(){
        QTemporaryDir data;AgentEngine engine;AgentToolHost host(&engine,nullptr,"invalid/agent",data.path());
        auto* broker=host.findChild<AttachmentFileBroker*>();QVERIFY(broker);QVERIFY(broker->rootDirectory().isEmpty());
        QSignalSpy created(&host,&AgentToolHost::attachmentCreated);
        auto result=run(host,"return await API.createAttachment('invalid-agent.txt',Buffer.from('must not be written'));");QVERIFY(result.error);QVERIFY(result.output.contains("storage is unavailable"));
        result=run(host,"return await fsPromises.readdir('att');");QVERIFY(result.error);QCOMPARE(created.count(),0);
        QVERIFY(!QFileInfo::exists(data.filePath("attachments/invalid/agent")));
    }

    void junctionsAndReplacedRootFailClosed(){
#ifdef Q_OS_WIN
        QTemporaryDir data,outside;AttachmentStore store(data.path());QString error;const auto ref=store.write("chat","a.txt","owned",&error);QVERIFY(!ref.isEmpty());QVERIFY(write(outside.filePath("secret.txt"),"outside-secret"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"chat",data.path());const auto root=store.agentDirectory("chat"),link=root+"/escape";
        const auto junction=[&](const QString& path){QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(outside.path())});return process.waitForFinished(5000)&&process.exitCode()==0;};
        QVERIFY(junction(link));const auto unlink=qScopeGuard([&]{QDir().rmdir(link);});
        auto result=run(host,"return await fsPromises.readFile('att/escape/secret.txt','utf8');");QVERIFY(result.error);QVERIFY(!result.output.contains("outside-secret"));
        result=run(host,"return await fsPromises.readdir('att',{recursive:true});");QVERIFY(result.error);QVERIFY(!result.output.contains("secret.txt"));
        QVERIFY(QDir().rmdir(link));const auto saved=root+"-saved";QVERIFY(QDir().rename(root,saved));
        const auto restore=qScopeGuard([&]{QDir().rmdir(root);QDir().rename(saved,root);});QVERIFY(junction(root));
        result=run(host,"return await fsPromises.readFile('att/secret.txt','utf8');");QVERIFY(result.error);QVERIFY(!result.output.contains("outside-secret"));
        result=run(host,"return await API.createAttachment('should-not-write.txt',Buffer.from('bad'));");QVERIFY(result.error);
        QCOMPARE(QDir(outside.path()).entryList(QDir::Files),QStringList{"secret.txt"});
#else
        QSKIP("Windows junction fixture");
#endif
    }
};
QTEST_MAIN(AttachmentMountTests)
#include "test_attachment_mount.moc"
