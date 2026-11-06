#include "mutations/FileMutationStore.h"
#include "mutations/FileMutationJournal.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdlib>
#include <windows.h>
#include <new>
using namespace ade::mutations;
namespace {
bool write(const QString& path,const QByteArray& value){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(value)==value.size();}
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
Request request(const QString& nonce,const QString& tool,const QString& path="ws1/file.txt"){Request r;r.agentId="chat";r.nonce=nonce;r.toolCallId="model-call";r.tool=tool;r.path=path;return r;}
bool setup(FileMutationStore& store,const QString& root,QString* error){return store.open(error)&&store.bindMount("ws1",root,true,error);}
}
class MutationTests:public QObject {
 Q_OBJECT
private slots:
 void writeUndoPreservesEmptyAndBinary_data(){QTest::addColumn<QByteArray>("before");QTest::newRow("text")<<QByteArray("old\r\n");QTest::newRow("empty")<<QByteArray("");QTest::newRow("binary")<<QByteArray::fromHex("00ff102030");}
 void writeUndoPreservesEmptyAndBinary(){
    QFETCH(QByteArray,before);QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),before));FileMutationStore store(data.path(),"chat");QString error;QVERIFY2(setup(store,root.path(),&error),qPrintable(error));
    auto r=request("write","write");r.content="```txt\nnew\n```";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),QByteArray("new"));
    result=store.undo("chat","write","undo-write");QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),before);QCOMPARE(store.describe("write",&error)["state"],"committed");
 }
 void newFileUndoRestoresMissing(){
    QTemporaryDir data,root;FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("create","write");r.content="";QVERIFY(store.execute(r).success);QVERIFY(QFileInfo::exists(root.filePath("file.txt")));QVERIFY(store.undo("chat","create","undo").success);QVERIFY(!QFileInfo::exists(root.filePath("file.txt")));
 }
 void multiEditSkipsUnmatchedAndCountsSequentially(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"alpha alpha\r\n"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("edit","multiEdit");r.edits={{"alpha","beta",false},{"absent","ignored",true},{"beta alpha","done",false}};auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(result.output["result"].toObject()["editsApplied"],2);QCOMPARE(read(root.filePath("file.txt")),QByteArray("done\r\n"));
    r.nonce="none";r.edits={{"missing","x",false}};result=store.execute(r);QVERIFY(result.success);QCOMPARE(result.output["result"].toObject()["editsApplied"],0);QCOMPARE(read(root.filePath("file.txt")),QByteArray("done\r\n"));
 }
 void multiEditCountsNonOverlappingOccurrences(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"aaaa"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("overlap","multiEdit");r.edits={{"aa","b",true}};const auto result=store.execute(r);QVERIFY(result.success);QCOMPARE(result.output["result"].toObject()["editsApplied"],2);QCOMPARE(read(root.filePath("file.txt")),QByteArray("bb"));
 }
 void copyMoveDeleteUndo_data(){QTest::addColumn<QString>("operation");QTest::newRow("copy")<<QString("copy");QTest::newRow("move")<<QString("move");QTest::newRow("delete")<<QString("delete");}
 void copyMoveDeleteUndo(){
    QFETCH(QString,operation);QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"source"));QVERIFY(write(root.filePath("target.txt"),"target"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("change",operation=="delete"?"delete":"copy");r.outputPath="ws1/target.txt";r.move=operation=="move";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));
    QCOMPARE(QFileInfo::exists(root.filePath("file.txt")),operation=="copy");QCOMPARE(read(root.filePath("target.txt")),operation=="delete"?QByteArray("target"):QByteArray("source"));
    result=store.undo("chat","change","undo");QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),QByteArray("source"));QCOMPARE(read(root.filePath("target.txt")),QByteArray("target"));
 }
 void interruptedWriteRecoversAcrossRestart_data(){QTest::addColumn<QString>("point");QTest::addColumn<bool>("committed");for(const auto& p:{"after-prepare","after-stage","after-backup","after-publish","before-commit"})QTest::newRow(p)<<QString(p)<<false;QTest::newRow("after-commit")<<QString("after-commit")<<true;}
 void interruptedWriteRecoversAcrossRestart(){
    QFETCH(QString,point);QFETCH(bool,committed);QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));QString error;
    {FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));store.setFaultHook([&](const QString& at,int){return at==point?Fault::Interrupt:Fault::None;});auto r=request("interrupted","write");r.content="after";const auto result=store.execute(r);QVERIFY(!result.success);QVERIFY(result.recoveryRequired);}
    {FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));const auto results=store.recover();QCOMPARE(results.size(),1);QVERIFY2(results.first().success,qPrintable(results.first().error));QCOMPARE(read(root.filePath("file.txt")),committed?QByteArray("after"):QByteArray("before"));QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden).size(),1);QVERIFY(store.recover().isEmpty());}
 }
 void failuresRollbackBothMoveSides_data(){QTest::addColumn<QString>("point");QTest::addColumn<int>("entry");QTest::newRow("before-source")<<QString("before-entry")<<1;QTest::newRow("source-backed-up")<<QString("after-backup")<<1;}
 void failuresRollbackBothMoveSides(){
    QFETCH(QString,point);QFETCH(int,entry);QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"source"));QVERIFY(write(root.filePath("target.txt"),"target"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));bool hit=false;store.setFaultHook([&](const QString& at,int i){if(at==point&&i==entry){hit=true;return Fault::Fail;}return Fault::None;});auto r=request("move","copy");r.outputPath="ws1/target.txt";r.move=true;const auto result=store.execute(r);QVERIFY(hit);QVERIFY(!result.success);QVERIFY2(!result.recoveryRequired,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),QByteArray("source"));QCOMPARE(read(root.filePath("target.txt")),QByteArray("target"));QCOMPARE(store.describe("move",&error)["state"],"rolled_back");
 }
 void conflictIsPreserved(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));QString error;
    {FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));store.setFaultHook([](const QString& at,int){return at=="after-publish"?Fault::Interrupt:Fault::None;});auto r=request("crash","write");r.content="after";QVERIFY(store.execute(r).recoveryRequired);}
    QVERIFY(write(root.filePath("file.txt"),"external"));FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));const auto results=store.recover();QCOMPARE(results.size(),1);QVERIFY(results[0].recoveryRequired);QCOMPARE(read(root.filePath("file.txt")),QByteArray("external"));QCOMPARE(store.describe("crash",&error)["state"],"recovery_required");
 }
 void undoAgentAndExternalConflict(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("write","write");r.content="after";QVERIFY(store.execute(r).success);QVERIFY(!store.undo("other","write","wrong-agent").success);QVERIFY(write(root.filePath("file.txt"),"external"));QVERIFY(!store.undo("chat","write","conflict").success);QCOMPARE(read(root.filePath("file.txt")),QByteArray("external"));
 }
 void cancellationAndDuplicateIdDoNotMutate(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));std::atomic_bool cancel=true;auto r=request("cancel","write");r.content="after";QVERIFY(!store.execute(r,&cancel).success);QCOMPARE(read(root.filePath("file.txt")),QByteArray("before"));QVERIFY(store.describe("cancel",&error).isEmpty());cancel=false;
    store.setFaultHook([&](const QString& point,int){if(point=="after-publish")cancel=true;return Fault::None;});auto result=store.execute(r,&cancel);QVERIFY(!result.success);QVERIFY(!result.recoveryRequired);QCOMPARE(read(root.filePath("file.txt")),QByteArray("before"));store.setFaultHook({});QVERIFY(!store.execute(r).success);QCOMPARE(read(root.filePath("file.txt")),QByteArray("before"));
 }
 void abruptProcessExitRecovery_data(){QTest::addColumn<QString>("boundary");QTest::addColumn<bool>("committed");for(const auto& value:{"after-prepare","after-stage-created","after-stage-chunk","after-backup","after-publish"})QTest::newRow(value)<<QString(value)<<false;QTest::newRow("after-commit")<<QString("after-commit")<<true;}
 void abruptProcessExitRecovery(){
    QFETCH(QString,boundary);QFETCH(bool,committed);QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));
    QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--crash-worker",data.path(),root.path(),boundary});QVERIFY(child.waitForFinished(10000));QCOMPARE(child.exitCode(),73);
    FileMutationStore store(data.path(),"chat");QString error;QVERIFY2(setup(store,root.path(),&error),qPrintable(error));const auto recovered=store.recover();QCOMPARE(recovered.size(),1);QVERIFY2(recovered.first().success,qPrintable(recovered.first().error));QCOMPARE(read(root.filePath("file.txt")),committed?QByteArray("after"):QByteArray("before"));QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden).size(),1);
 }
 void pinnedAncestorsAndLateDestinationConflict(){
    QTemporaryDir data,fixture;const auto parent=fixture.filePath("parent"),root=parent+"/workspace";QVERIFY(QDir().mkpath(root));QVERIFY(write(root+"/file.txt","before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root,&error));bool checked=false;
    store.setFaultHook([&](const QString& at,int){if(at=="after-prepare"){checked=true;if(QDir().rename(parent,fixture.filePath("moved")))return Fault::Fail;}return Fault::None;});auto r=request("pinned","write");r.content="after";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QVERIFY(checked);QVERIFY(QFileInfo::exists(parent));
    bool inserted=false;store.setFaultHook([&](const QString& at,int){if(at=="after-backup")inserted=write(root+"/file.txt","foreign");return Fault::None;});r.nonce="conflict";r.content="new";result=store.execute(r);QVERIFY(inserted);QVERIFY(!result.success);QVERIFY(result.recoveryRequired);QCOMPARE(read(root+"/file.txt"),QByteArray("foreign"));QCOMPARE(read(root+"/.ade-mutation-conflict-0.backup"),QByteArray("after"));
 }
 void junctionAndReplacementMountFailClosed(){
    QTemporaryDir data,fixture;const auto root=fixture.filePath("root"),outside=fixture.filePath("outside");QVERIFY(QDir().mkpath(root));QVERIFY(QDir().mkpath(outside));QVERIFY(write(outside+"/secret.txt","outside"));
    QProcess link;link.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(root+"/escape"),QDir::toNativeSeparators(outside)});QVERIFY(link.waitForFinished(5000));QCOMPARE(link.exitCode(),0);
    FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root,&error));auto r=request("escape","write","ws1/escape/secret.txt");r.content="bad";QVERIFY(!store.execute(r).success);QCOMPARE(read(outside+"/secret.txt"),QByteArray("outside"));QVERIFY(QDir().rmdir(root+"/escape"));
    QVERIFY(QDir().rename(root,fixture.filePath("old-root")));QVERIFY(QDir().mkpath(root));QVERIFY(write(root+"/file.txt","replacement"));r.path="ws1/file.txt";r.nonce="replacement";QVERIFY(!store.execute(r).success);QCOMPARE(read(root+"/file.txt"),QByteArray("replacement"));
 }
 void readOnlyCopyAndAppsWithoutWorkspace(){
    QTemporaryDir data,apps,attachments;QVERIFY(write(attachments.filePath("blob.bin"),QByteArray::fromHex("00ff55")));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(store.open(&error));QVERIFY(store.bindMount("apps",apps.path(),true,&error));QVERIFY(store.bindMount("att",attachments.path(),false,&error));auto r=request("copy","copy","att/blob.bin");r.outputPath="apps/blob.bin";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(apps.filePath("blob.bin")),QByteArray::fromHex("00ff55"));r.nonce="move";r.move=true;QVERIFY(!store.execute(r).success);QCOMPARE(read(attachments.filePath("blob.bin")),QByteArray::fromHex("00ff55"));QVERIFY(store.undo("chat","copy","undo").success);QVERIFY(!QFileInfo::exists(apps.filePath("blob.bin")));
 }
 void corruptSnapshotPreventsMutationAndUndo(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("first","write");r.content="after";QVERIFY(store.execute(r).success);
    const auto connection=QString("test-corrupt");{auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(data.filePath("mutation-journal-v1/journal.sqlite"));QVERIFY(db.open());{QSqlQuery q(db);QVERIFY(q.exec("UPDATE snapshots SET bytes=x'78'"));}db.close();}QSqlDatabase::removeDatabase(connection);
    r.nonce="second";r.content="third";const auto result=store.execute(r);QVERIFY(!result.success);QVERIFY(result.error.contains("corrupt"));QCOMPARE(read(root.filePath("file.txt")),QByteArray("after"));QVERIFY(!store.undo("chat","first","undo").success);QCOMPARE(read(root.filePath("file.txt")),QByteArray("after"));
 }
 void immutableAgentIdentityAndSnapshotIsolation(){
    QTemporaryDir data,first,second;FileMutationStore one(data.path(),"one"),two(data.path(),"two");QString error;QVERIFY(one.open(&error));QVERIFY(two.open(&error));QVERIFY(one.bindMount("apps",first.path(),true,&error));QVERIFY(two.bindMount("apps",second.path(),true,&error));
    auto r=request("first","write","apps/file.txt");r.agentId="one";r.content="first";QVERIFY(one.execute(r).success);QVERIFY(!two.execute(r).success);QVERIFY(two.describe("first",&error).isEmpty());QVERIFY(!two.undo("one","first","foreign").success);QVERIFY(two.recover().isEmpty());
    r.nonce="second";r.agentId="two";r.content="second";QVERIFY(two.execute(r).success);QCOMPARE(read(first.filePath("file.txt")),QByteArray("first"));QCOMPARE(read(second.filePath("file.txt")),QByteArray("second"));
 }
 void anotherLiveOwnerCannotRecoverPreparedWork(){
    QTemporaryDir data,root;FileMutationStore first(data.path(),"chat"),second(data.path(),"chat");QString error;QVERIFY(setup(first,root.path(),&error));QVERIFY(setup(second,root.path(),&error));QList<Result> nested;QString during;bool visited=false;
    first.setFaultHook([&](const QString& at,int){if(at=="after-prepare"){visited=true;nested=second.recover();during=first.describe("active",&error)["state"].toString();}return Fault::None;});auto r=request("active","write");r.content="created";const auto result=first.execute(r);QVERIFY(result.success);QVERIFY(visited);QCOMPARE(during,QString("prepared"));QCOMPARE(nested.size(),1);QVERIFY(nested.first().recoveryRequired);QVERIFY(nested.first().error.contains("busy",Qt::CaseInsensitive));
 }
 void multiEditExpansionRejectedBeforeLargeAllocation(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),QByteArray(1024,'a')));QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--growth-worker",data.path(),root.path()});QVERIFY(child.waitForFinished(10000));QCOMPARE(child.exitCode(),0);QCOMPARE(read(root.filePath("file.txt")),QByteArray(1024,'a'));
 }
 void partialStagedWriteRollsBackCleanly(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));bool hit=false;store.setFaultHook([&](const QString& at,int){if(at=="after-stage-chunk"){hit=true;return Fault::Fail;}return Fault::None;});auto r=request("partial","write");r.content=QString(200000,'x');const auto result=store.execute(r);QVERIFY(hit);QVERIFY(!result.success);QVERIFY2(!result.recoveryRequired,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),QByteArray("before"));QCOMPARE(QDir(root.path()).entryList(QDir::Files|QDir::Hidden).size(),1);
 }
 void rollbackRestorationSurvivesAnotherProcessExit_data(){QTest::addColumn<QString>("boundary");for(const auto& point:{"rollback-after-restore-created","rollback-after-restore-chunk","rollback-after-restore"})QTest::newRow(point)<<QString(point);}
 void rollbackRestorationSurvivesAnotherProcessExit(){
    QFETCH(QString,boundary);QTemporaryDir data,root;const QByteArray before(200000,'p');QVERIFY(write(root.filePath("file.txt"),before));QString error;
    {FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));store.setFaultHook([](const QString& at,int){return at=="after-backup"?Fault::Interrupt:Fault::None;});auto r=request("lost-backup","write");r.content="after";QVERIFY(store.execute(r).recoveryRequired);}
    QVERIFY(QFile::remove(root.filePath(".ade-mutation-lost-backup-0.backup")));
    QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--recover-worker",data.path(),root.path(),boundary});QVERIFY(child.waitForFinished(10000));QCOMPARE(child.exitCode(),73);
    FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));const auto results=store.recover();QCOMPARE(results.size(),1);QVERIFY2(results.first().success,qPrintable(results.first().error));QCOMPARE(read(root.filePath("file.txt")),before);QVERIFY(store.recover().isEmpty());
 }
 void originalMountAndReadOnlyRights(){
    QTemporaryDir data,root,other;QVERIFY(write(root.filePath("file.txt"),"before"));QVERIFY(write(other.filePath("file.txt"),"untouched"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));QVERIFY(store.bindMount("att",other.path(),false,&error));auto r=request("write","write");r.content="after";QVERIFY(store.execute(r).success);r.nonce="readonly";r.path="att/file.txt";QVERIFY(!store.execute(r).success);QVERIFY(!store.bindMount("att",other.path(),true,&error));QVERIFY(store.bindMount("ws1",other.path(),true,&error));QVERIFY(!store.undo("chat","write","undo").success);QCOMPARE(read(other.filePath("file.txt")),QByteArray("untouched"));
 }
 void parentsAreJournaledAndUndoRemovesOnlyOwnedEmptyDirectories(){
    QTemporaryDir data,root;FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("parents","write","ws1/new/deep/file.txt");r.content="bytes";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("new/deep/file.txt")),QByteArray("bytes"));result=store.undo("chat","parents","undo");QVERIFY2(result.success,qPrintable(result.error));QVERIFY(!QFileInfo::exists(root.filePath("new")));
 }
 void recursiveMergeMoveDeleteAndUndo_data(){QTest::addColumn<QString>("operation");for(const auto& name:{"copy","move","delete"})QTest::newRow(name)<<QString(name);}
 void recursiveMergeMoveDeleteAndUndo(){
    QFETCH(QString,operation);QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("source/deep/empty")));QVERIFY(QDir().mkpath(root.filePath("target")));QVERIFY(write(root.filePath("source/a.bin"),QByteArray::fromHex("00ff000a")));QVERIFY(write(root.filePath("source/deep/b.txt"),"source\r\n"));QVERIFY(write(root.filePath("target/a.bin"),"old"));QVERIFY(write(root.filePath("target/unrelated.txt"),"keep"));
    FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("tree",operation=="delete"?"delete":"copy","ws1/source");r.outputPath="ws1/target";r.move=operation=="move";auto result=store.execute(r);QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(QFileInfo::exists(root.filePath("source")),operation=="copy");QCOMPARE(read(root.filePath("target/unrelated.txt")),QByteArray("keep"));
    if(operation!="delete"){QCOMPARE(read(root.filePath("target/a.bin")),QByteArray::fromHex("00ff000a"));QVERIFY(QFileInfo(root.filePath("target/deep/empty")).isDir());}
    result=store.undo("chat","tree","undo-tree");QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("source/a.bin")),QByteArray::fromHex("00ff000a"));QCOMPARE(read(root.filePath("source/deep/b.txt")),QByteArray("source\r\n"));QVERIFY(QFileInfo(root.filePath("source/deep/empty")).isDir());QCOMPARE(read(root.filePath("target/a.bin")),QByteArray("old"));QCOMPARE(read(root.filePath("target/unrelated.txt")),QByteArray("keep"));QVERIFY(!QFileInfo::exists(root.filePath("target/deep")));
 }
 void recursiveFailureRollsBackTopology_data(){QTest::addColumn<QString>("point");for(const auto& name:{"after-directory-create","after-directory-delete"})QTest::newRow(name)<<QString(name);}
 void recursiveFailureRollsBackTopology(){
    QFETCH(QString,point);QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("source/deep/empty")));QVERIFY(write(root.filePath("source/deep/a.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));bool hit=false;store.setFaultHook([&](const QString& at,int){if(at==point&&!hit){hit=true;return Fault::Fail;}return Fault::None;});auto r=request("failed-tree","copy","ws1/source");r.outputPath="ws1/target";r.move=true;const auto result=store.execute(r);QVERIFY(hit);QVERIFY(!result.success);QVERIFY2(!result.recoveryRequired,qPrintable(result.error));QCOMPARE(read(root.filePath("source/deep/a.txt")),QByteArray("before"));QVERIFY(QFileInfo(root.filePath("source/deep/empty")).isDir());QVERIFY(!QFileInfo::exists(root.filePath("target")));
 }
 void directoryUndoPreservesUnrelatedUserFiles(){
    QTemporaryDir data,root;FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("parents","write","ws1/new/file.txt");r.content="assistant";QVERIFY(store.execute(r).success);QVERIFY(write(root.filePath("new/user.txt"),"user"));const auto undone=store.undo("chat","parents","undo");QVERIFY(!undone.success);QVERIFY2(!undone.recoveryRequired,qPrintable(undone.error));QCOMPARE(read(root.filePath("new/file.txt")),QByteArray("assistant"));QCOMPARE(read(root.filePath("new/user.txt")),QByteArray("user"));
 }
 void sourceBasenameAndGroupedUndo(){
    QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("dest")));QVERIFY(write(root.filePath("file.txt"),"before"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("copy","copy");r.outputPath="ws1/dest";QVERIFY(store.execute(r).success);QCOMPARE(read(root.filePath("dest/file.txt")),QByteArray("before"));r=request("write","write");r.content="middle";QVERIFY(store.execute(r).success);r.nonce="later";r.content="after";QVERIFY(store.execute(r).success);auto result=store.undoMany({"copy","write","later"},"undo-all");QVERIFY2(result.success,qPrintable(result.error));QCOMPARE(read(root.filePath("file.txt")),QByteArray("before"));QVERIFY(!QFileInfo::exists(root.filePath("dest/file.txt")));QCOMPARE(store.history(&error).size(),4);
 }
 void deletionKeepsOtherAgentHistory(){
    QTemporaryDir data,root,other;FileMutationStore one(data.path(),"chat"),two(data.path(),"other");QString error;QVERIFY(setup(one,root.path(),&error));QVERIFY(two.open(&error));QVERIFY(two.bindMount("ws1",other.path(),true,&error));auto r=request("one","write");r.content="one";QVERIFY(one.execute(r).success);r.nonce="two";r.agentId="other";r.content="two";QVERIFY(two.execute(r).success);QVERIFY(one.removeAgent(&error));QVERIFY(one.history(&error).isEmpty());QCOMPARE(two.history(&error).size(),1);QVERIFY(two.undo("other","two","undo-two").success);QCOMPARE(read(root.filePath("file.txt")),QByteArray("one"));QVERIFY(!QFileInfo::exists(other.filePath("file.txt")));
 }
 void rawWriteKeepsFencesAndBinaryExactly(){
    QTemporaryDir data,root;FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("raw","write");r.raw=true;r.content="```text\r\nraw\r\n```";QVERIFY(store.execute(r).success);QCOMPARE(read(root.filePath("file.txt")),r.content.toUtf8());r.nonce="binary";r.tool="writeBytes";r.bytes=QByteArray::fromHex("00fffe0d0a");QVERIFY(store.execute(r).success);QCOMPARE(read(root.filePath("file.txt")),r.bytes);QVERIFY(store.undoMany({"raw","binary"},"undo-raw").success);QVERIFY(!QFileInfo::exists(root.filePath("file.txt")));
 }

 void directoryProcessExitRecovers_data(){QTest::addColumn<QString>("point");QTest::addColumn<bool>("committed");for(const auto& p:{"after-directory-create","after-directory-delete","after-publish"})QTest::newRow(p)<<QString(p)<<false;QTest::newRow("after-commit")<<QString("after-commit")<<true;}
 void directoryProcessExitRecovers(){
    QFETCH(QString,point);QFETCH(bool,committed);QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("source/deep/empty")));QVERIFY(write(root.filePath("source/deep/a.txt"),"before"));QProcess child;child.start(QCoreApplication::applicationFilePath(),{"--tree-crash-worker",data.path(),root.path(),point});QVERIFY(child.waitForFinished(10000));QCOMPARE(child.exitCode(),73);
    FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));const auto recovered=store.recover();QCOMPARE(recovered.size(),1);QVERIFY2(recovered.first().success,qPrintable(recovered.first().error));QCOMPARE(QFileInfo::exists(root.filePath("source")),!committed);QCOMPARE(QFileInfo::exists(root.filePath("target")),committed);QCOMPARE(read(root.filePath((committed?"target":"source")+QString("/deep/a.txt"))),QByteArray("before"));QVERIFY(QFileInfo(root.filePath((committed?"target":"source")+QString("/deep/empty"))).isDir());QVERIFY(store.recover().isEmpty());
 }
 void directoryEntryLimitRejectsBeforeMutation(){
    QTemporaryDir data,root;QVERIFY(QDir().mkdir(root.filePath("source")));for(int i=0;i<201;++i)QVERIFY(write(root.filePath(QString("source/%1.txt").arg(i)),"keep"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));auto r=request("large","delete","ws1/source");const auto result=store.execute(r);QVERIFY(!result.success);QVERIFY(!result.recoveryRequired);QCOMPARE(QDir(root.filePath("source")).entryList(QDir::Files).size(),201);QVERIFY(store.history(&error).isEmpty());
 }
 void revokeAndCleanupRejectOldExecutorWithoutTouchingFiles(){
    QTemporaryDir data,root;ade::FileMutationJournal journal(data.path(),"chat");QString error;QVERIFY(journal.bindMount("ws1",root.path(),true,&error));auto execute=journal.executor();ade::FileMutationInvocation call{"first","write",root.path(),{},{{"path","file.txt"},{"content","kept"}}};auto result=execute(call,nullptr);QVERIFY2(!result.error,qPrintable(result.output));QCOMPARE(journal.history(&error).size(),1);QVERIFY(ade::FileMutationJournal::removeAgent(data.path(),"chat",&error));call.requestId="late";call.arguments["content"]="forbidden";QVERIFY(execute(call,nullptr).error);QCOMPARE(read(root.filePath("file.txt")),QByteArray("kept"));ade::FileMutationJournal fresh(data.path(),"chat");QVERIFY(fresh.bindMount("ws1",root.path(),true,&error));QVERIFY(fresh.history(&error).isEmpty());QVERIFY(!fresh.executor()(call,nullptr).error);QCOMPARE(read(root.filePath("file.txt")),QByteArray("forbidden"));
 }
 void journalPinsProfileAndRejectsMountReplacement(){
    QTemporaryDir fixture,root;const auto profile=fixture.filePath("profile");QVERIFY(QDir().mkdir(profile));ade::FileMutationJournal journal(profile,"chat");QString error;QVERIFY(journal.bindMount("ws1",root.path(),true,&error));QVERIFY(!QDir().rename(profile,fixture.filePath("moved-profile")));const auto old=root.path()+"-old";QVERIFY(QDir().rename(root.path(),old));QVERIFY(QDir().mkdir(root.path()));QVERIFY(write(root.filePath("file.txt"),"replacement"));ade::FileMutationInvocation call{"first","write",root.path(),{},{{"path","file.txt"},{"content","forbidden"}}};QVERIFY(journal.executor()(call,nullptr).error);QCOMPARE(read(root.filePath("file.txt")),QByteArray("replacement"));QVERIFY(QDir().rmdir(old));
 }

 void recursiveDeletePreservesChildCreatedAfterPlanning(){
    QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("source/deep")));QVERIFY(write(root.filePath("source/deep/original.txt"),"original"));FileMutationStore store(data.path(),"chat");QString error;QVERIFY(setup(store,root.path(),&error));bool inserted=false;
    store.setFaultHook([&](const QString& point,int){if(point=="before-directory-delete"&&!inserted)inserted=write(root.filePath("source/deep/foreign.txt"),"foreign");return Fault::None;});
    const auto result=store.execute(request("late-child","delete","ws1/source"));QVERIFY(inserted);QVERIFY(!result.success);QVERIFY2(!result.recoveryRequired,qPrintable(result.error));QCOMPARE(read(root.filePath("source/deep/original.txt")),QByteArray("original"));QCOMPARE(read(root.filePath("source/deep/foreign.txt")),QByteArray("foreign"));
 }
 void committedCleanupIsVisibleThroughHostHistory(){
    QTemporaryDir data,root;QVERIFY(write(root.filePath("file.txt"),"before"));QString error;
    {FileMutationStore store(data.path(),"chat");QVERIFY(setup(store,root.path(),&error));store.setFaultHook([](const QString& at,int){return at=="after-commit"?Fault::Interrupt:Fault::None;});auto r=request("committed-cleanup","write");r.content="after";const auto result=store.execute(r);QVERIFY(result.recoveryRequired);QCOMPARE(store.describe(r.nonce,&error)["state"].toString(),QString("committed"));}
    ade::FileMutationJournal journal(data.path(),"chat");QVERIFY(journal.bindMount("ws1",root.path(),true,&error));auto history=journal.history(&error);QCOMPARE(history.size(),1);QVERIFY(history[0].toObject()["recoveryRequired"].toBool());QCOMPARE(read(root.filePath("file.txt")),QByteArray("after"));const auto recovery=journal.recover();QVERIFY2(!recovery.error,qPrintable(recovery.output));history=journal.history(&error);QCOMPARE(history.size(),1);QVERIFY(history[0].toObject().contains("recoveryRequired"));QVERIFY(!history[0].toObject()["recoveryRequired"].toBool());QCOMPARE(read(root.filePath("file.txt")),QByteArray("after"));
 }
 void revocationStopsApplyButMustAllowDurableRollback(){
    QTemporaryDir data,root;QVERIFY(QDir().mkpath(root.filePath("source/deep/empty")));QVERIFY(write(root.filePath("source/deep/a.txt"),"before"));ade::FileMutationJournal journal(data.path(),"chat");QString error;QVERIFY(journal.bindMount("ws1",root.path(),true,&error));bool revoked=false;journal.setBoundaryObserverForTests([&](const QString& boundary,int){if(boundary=="after-directory-delete"&&!revoked){revoked=true;journal.revoke();}});ade::FileMutationInvocation call{"call","copy",root.path(),{},{{"input_path","ws1/source"},{"output_path","ws1/target"},{"move",true}}};const auto result=journal.executor()(call,nullptr);QVERIFY(revoked);QVERIFY(result.error);QVERIFY2(!result.metadata["mutations"].toArray()[0].toObject()["recoveryRequired"].toBool(),qPrintable(result.output));QCOMPARE(read(root.filePath("source/deep/a.txt")),QByteArray("before"));QVERIFY(QFileInfo(root.filePath("source/deep/empty")).isDir());QVERIFY(!QFileInfo::exists(root.filePath("target")));
 }

};
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    if(args.size()==4&&args[1]=="--growth-worker"){
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);HANDLE job=CreateJobObjectW(nullptr,nullptr);if(!job)return 76;JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY;limits.ProcessMemoryLimit=128*1024*1024;if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))||!AssignProcessToJobObject(job,GetCurrentProcess()))return 76;
        try{FileMutationStore store(args[2],"chat");QString error;if(!setup(store,args[3],&error))return 74;auto r=request("growth","multiEdit");r.edits={{"a",QString(1024*1024,'x'),true}};const auto result=store.execute(r);return !result.success&&result.error.contains("bound")?0:78;}catch(const std::bad_alloc&){return 77;}
    }
    if(args.size()==5&&args[1]=="--tree-crash-worker"){
        FileMutationStore store(args[2],"chat");QString error;if(!setup(store,args[3],&error))return 74;store.setFaultHook([&](const QString& point,int){if(point==args[4])std::_Exit(73);return Fault::None;});auto r=request("tree-crash","copy","ws1/source");r.outputPath="ws1/target";r.move=true;store.execute(r);return 75;
    }
    if(args.size()==5&&args[1]=="--recover-worker"){
        FileMutationStore store(args[2],"chat");QString error;if(!setup(store,args[3],&error))return 74;store.setFaultHook([&](const QString& point,int){if(point==args[4])std::_Exit(73);return Fault::None;});store.recover();return 75;
    }
    if(args.size()==5&&args[1]=="--crash-worker"){
        FileMutationStore store(args[2],"chat");QString error;if(!setup(store,args[3],&error))return 74;
        store.setFaultHook([&](const QString& point,int){if(point==args[4])std::_Exit(73);return Fault::None;});
        auto r=request("crashed","write");r.content=args[4]=="after-stage-chunk"?QString(200000,'x'):QString("after");store.execute(r);return 75;
    }
    MutationTests test;return QTest::qExec(&test,argc,argv);
}
#include "test_file_mutations.moc"
