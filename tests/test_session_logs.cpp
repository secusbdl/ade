#include "shell/SessionLogStore.h"
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
const QString A="11111111-1111-1111-1111-111111111111",B="22222222-2222-2222-2222-222222222222";
QString logPath(const QString& id){return "shells/"+id+".shell.log";}
bool write(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
}
class SessionLogTests:public QObject{
 Q_OBJECT
private slots:
 void appendReadAndRestart(){
    QTemporaryDir data;QString root,error;const auto bytes=QString::fromUtf8("line1\r\nأهلا\n").toUtf8();
    {SessionLogStore store(data.path(),"chat");root=store.rootDirectory();QVERIFY(!root.isEmpty());QVERIFY(store.create(A,&error));QVERIFY(!store.create(A,&error));QVERIFY(store.append(A,bytes,&error));QCOMPARE(store.rawRead(logPath(A),1024,nullptr,&error),bytes);QVERIFY(error.isEmpty());QCOMPARE(store.stats(A)["lineCount"].toInt(),2);QCOMPARE(store.stats(A)["bytesWritten"].toInt(),bytes.size());QVERIFY(store.flush(A,&error));QVERIFY(store.close(A,&error));QVERIFY(!store.append(A,"late",&error));}
    SessionLogStore reopened(data.path(),"chat");QCOMPARE(reopened.rootDirectory(),root);QCOMPARE(reopened.rawRead(logPath(A),1024,nullptr,&error),bytes);QVERIFY(error.isEmpty());QVERIFY(!reopened.create(A,&error));QCOMPARE(reopened.list(&error),QStringList{A+".shell.log"});
 }
 void formattedLeaseIsRevoked(){
    QTemporaryDir data;SessionLogStore store(data.path(),"chat");QString error;QVERIFY(store.create(A,&error));QVERIFY(store.append(A,"one\ntwo\n",&error));auto source=store.formattedSource();FileReadCapture capture;FileReadParams params;params.startLine=2;
    QVERIFY(source("read",logPath(A),params,&capture,nullptr,&error));QCOMPARE(capture.bytes,QByteArray("one\ntwo\n"));QCOMPARE(capture.params.startLine.value(),2);QCOMPARE(capture.descriptor.mountedPath,logPath(A));QVERIFY(capture.descriptor.modified.isValid());
    QVERIFY(source("ls","shells",{},&capture,nullptr,&error));QCOMPARE(capture.entries.size(),1);QVERIFY(capture.directory);store.revoke();QVERIFY(!source("read",logPath(A),{},&capture,nullptr,&error));QVERIFY(capture.bytes.isEmpty());QVERIFY(!store.append(A,"late",&error));
 }
 void ownerLockAndPin(){
    QTemporaryDir parent;const auto data=parent.filePath("data");QVERIFY(QDir().mkdir(data));SessionLogStore first(data,"chat"),second(data,"CHAT");QString error;QVERIFY(!first.rootDirectory().isEmpty());QVERIFY(second.rootDirectory().isEmpty());QVERIFY(!second.create(A,&error));QVERIFY(!QDir().rename(data,parent.filePath("renamed")));first.revoke();SessionLogStore next(data,"chat");QVERIFY(!next.rootDirectory().isEmpty());
 }
 void invalidPaths_data(){QTest::addColumn<QString>("path");for(const auto& value:{"shells/../other","shells/x.shell.log","shells/11111111-1111-1111-1111-111111111111.shell.log:stream","shells\\11111111-1111-1111-1111-111111111111.shell.log","other/11111111-1111-1111-1111-111111111111.shell.log"})QTest::newRow(value)<<QString(value);}
 void invalidPaths(){QFETCH(QString,path);QTemporaryDir data;SessionLogStore store(data.path(),"chat");QString error;QVERIFY(store.create(A,&error));QVERIFY(store.append(A,"keep",&error));QVERIFY(store.rawRead(path,1024,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());QCOMPARE(store.rawRead(logPath(A),1024,nullptr,&error),QByteArray("keep"));}
 void noPartialReadAndCancellation(){QTemporaryDir data;SessionLogStore store(data.path(),"chat");QString error;QVERIFY(store.create(A,&error));QVERIFY(store.append(A,"12345",&error));QVERIFY(store.rawRead(logPath(A),4,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());std::atomic_bool canceled=true;QVERIFY(store.rawRead(logPath(A),10,&canceled,&error).isEmpty());QVERIFY(error.contains("canceled"));FileReadCapture capture;QVERIFY(!store.formattedSource()("ls","shells",{},&capture,&canceled,&error));QVERIFY(capture.entries.isEmpty());}
 void truncationHasOneVisibleMarker(){QTemporaryDir data;SessionLogStore store(data.path(),"chat");QString error;QVERIFY(store.create(A,&error));const QByteArray megabyte(1024*1024,'x');for(int i=0;i<50;++i)QVERIFY(store.append(A,megabyte,&error));QVERIFY(store.append(A,"overflow",&error));QVERIFY(store.append(A,"ignored",&error));const auto bytes=store.rawRead(logPath(A),SessionLogStore::MaxLogBytes+64,nullptr,&error);QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(bytes.endsWith("\n[log truncated: exceeded 50MB]\n"));QCOMPARE(bytes.count("[log truncated:"),1);QVERIFY(store.stats(A)["truncated"].toBool());QCOMPARE(store.stats(A)["bytesWritten"].toDouble(),double(bytes.size()));}
 void invalidUtf8StopsWriter(){QTemporaryDir data;SessionLogStore store(data.path(),"chat");QString error;QVERIFY(store.create(A,&error));QVERIFY(store.append(A,"good",&error));QVERIFY(!store.append(A,QByteArray::fromHex("e282"),&error));QVERIFY(!store.append(A,"bad",&error));QCOMPARE(store.rawRead(logPath(A),1024,nullptr,&error),QByteArray("good"));}
 void cloneAndDeleteAreOwnerScoped(){
    QTemporaryDir data;SessionLogStore source(data.path(),"source"),other(data.path(),"other");QString error;QVERIFY(source.create(A,&error));QVERIFY(source.append(A,"source",&error));QVERIFY(other.create(B,&error));QVERIFY(other.append(B,"other",&error));QVERIFY(source.cloneTo("fork",&error));SessionLogStore fork(data.path(),"fork");QCOMPARE(fork.rawRead(logPath(A),1024,nullptr,&error),QByteArray("source"));QVERIFY(!source.cloneTo("fork",&error));QVERIFY(source.append(A," later",&error));QCOMPARE(fork.rawRead(logPath(A),1024,nullptr,&error),QByteArray("source"));auto old=source.formattedSource();QVERIFY(SessionLogStore::removeAgent(data.path(),"source",&error));FileReadCapture capture;QVERIFY(!old("read",logPath(A),{},&capture,nullptr,&error));QCOMPARE(other.rawRead(logPath(B),1024,nullptr,&error),QByteArray("other"));QCOMPARE(fork.rawRead(logPath(A),1024,nullptr,&error),QByteArray("source"));SessionLogStore restarted(data.path(),"source");QVERIFY(restarted.list(&error).isEmpty());QVERIFY(error.isEmpty());
 }
 void unexpectedEntriesAndHardlinksFailClosed(){
    QTemporaryDir data,outside;SessionLogStore store(data.path(),"chat");QString error;const auto target=outside.filePath("secret.txt");QVERIFY(write(target,"outside"));QProcess process;process.start("cmd.exe",{"/c","mklink","/H",QDir::toNativeSeparators(store.rootDirectory()+"/"+A+".shell.log"),QDir::toNativeSeparators(target)});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);QVERIFY(store.rawRead(logPath(A),1024,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());QVERIFY(!SessionLogStore::removeAgent(data.path(),"chat",&error));QFile file(target);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("outside"));
 }
 void aliasProfileRejected(){QTemporaryDir fixture;QVERIFY(QDir().mkdir(fixture.filePath("actual")));QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(fixture.filePath("alias")),QDir::toNativeSeparators(fixture.filePath("actual"))});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);{SessionLogStore store(fixture.filePath("alias"),"chat");QVERIFY(store.rootDirectory().isEmpty());QString error;QVERIFY(!store.create(A,&error));}QVERIFY(!QFileInfo::exists(fixture.filePath("actual/shell-logs-v1")));QVERIFY(QDir().rmdir(fixture.filePath("alias")));}
};
QTEST_GUILESS_MAIN(SessionLogTests)
#include "test_session_logs.moc"
