#include "shell/SessionLogStore.h"
#include "core/ToolExecutor.h"
#include "attachments/FileReadSnapshotStore.h"
#include "attachments/AttachmentPipeline.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace { const QString Id="11111111-1111-1111-1111-111111111111",Path="shells/"+Id+".shell.log"; }
class ShellLogMountTests:public QObject{
 Q_OBJECT
private slots:
 void rawReadsAreExactAndReadOnly(){
    QTemporaryDir data;SessionLogStore logs(data.path(),"chat");QString error;QVERIFY(logs.create(Id,&error));const QByteArray bytes="\xef\xbb\xbfline\r\n";QVERIFY(logs.append(Id,bytes,&error));ToolExecutor tools;tools.setShellReadSource(logs.formattedSource());QSignalSpy done(&tools,&ToolExecutor::completed);
    tools.execute("bytes","readBytes",{{"path",Path}});QTRY_COMPARE(done.size(),1);QVERIFY(!done[0][2].toBool());QCOMPARE(QByteArray::fromBase64(done[0][1].toString().toLatin1()),bytes);
    tools.execute("text","read",{{"path",Path},{"raw",true}});QTRY_COMPARE(done.size(),2);QVERIFY(!done[1][2].toBool());QCOMPARE(done[1][1].toString().toUtf8(),bytes);
    tools.execute("list","readDirectory",{{"path","shells"},{"recursive",true}});QTRY_COMPARE(done.size(),3);QVERIFY(!done[2][2].toBool());QCOMPARE(QJsonDocument::fromJson(done[2][1].toString().toUtf8()).array(),QJsonArray{Id+".shell.log"});
    tools.execute("write","write",{{"path",Path},{"content","forbidden"}});QTRY_COMPARE(done.size(),4);QVERIFY(done[3][2].toBool());QCOMPARE(logs.rawRead(Path,100,nullptr,&error),bytes);
 }
 void formattedSnapshotsAreFixedAndOwnerBound(){
    QTemporaryDir data;SessionLogStore logs(data.path(),"chat");QString error;QVERIFY(logs.create(Id,&error));QVERIFY(logs.append(Id,"one\ntwo\nthree\n",&error));FileReadSnapshotStore snapshots(data.path());const auto lease=snapshots.acquire("chat");ToolExecutor tools;tools.setShellReadSource(logs.formattedSource());tools.setFileReadCommitter([lease](const auto& capture,const auto* cancel,QString* error){return FileReadSnapshotStore::commit(lease,capture,cancel,error);});QSignalSpy done(&tools,&ToolExecutor::completed);
    const QJsonObject args{{"path",Path},{"start_line",2},{"end_line",2}};tools.execute("read","read",args);QTRY_COMPARE(done.size(),1);QVERIFY2(!done[0][2].toBool(),qPrintable(done[0][1].toString()));const auto meta=done[0][3].toJsonObject();FileReadCapture captured;QVERIFY(FileReadSnapshotStore::read(lease,meta,&captured,&error));QCOMPARE(captured.bytes,QByteArray("one\ntwo\nthree\n"));QCOMPARE(captured.params.startLine.value(),2);QVERIFY(logs.append(Id,"four\n",&error));QVERIFY(FileReadSnapshotStore::read(lease,meta,&captured,&error));QCOMPARE(captured.bytes,QByteArray("one\ntwo\nthree\n"));
    const auto other=snapshots.acquire("other");QVERIFY(!FileReadSnapshotStore::read(other,meta,&captured,&error));
    AttachmentPipeline pipeline{AttachmentStore(data.path())};pipeline.setFileReadLease(lease);const QJsonObject function{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))}};
    const QJsonArray history{QJsonObject{{"role","assistant"},{"tool_calls",QJsonArray{QJsonObject{{"id","read"},{"type","function"},{"function",function}}}}},QJsonObject{{"role","tool"},{"tool_call_id","read"},{"content",done[0][1].toString()},{"metadata",meta}}};
    const auto prepared=pipeline.prepare(history,"chat",{}, {}, {},&error);QVERIFY2(error.isEmpty(),qPrintable(error));const auto encoded=QJsonDocument(prepared).toJson();QVERIFY(encoded.contains("two"));QVERIFY(!encoded.contains("four"));QVERIFY(!encoded.contains("Original read could not be included"));
 }
 void largerLogsDoNotRelaxWorkspaceCaptureBound(){
    QTemporaryDir data;SessionLogStore logs(data.path(),"chat");QString error;QVERIFY(logs.create(Id,&error));QVERIFY(logs.append(Id,QByteArray(5*1024*1024,'x'),&error));FileReadCapture capture;QVERIFY(logs.formattedSource()("read",Path,{},&capture,nullptr,&error));FileReadSnapshotStore snapshots(data.path());const auto lease=snapshots.acquire("chat");const auto meta=FileReadSnapshotStore::commit(lease,capture,nullptr,&error);QVERIFY2(!meta.isEmpty(),qPrintable(error));FileReadCapture restored;QVERIFY(FileReadSnapshotStore::read(lease,meta,&restored,&error));QCOMPARE(restored.bytes.size(),5*1024*1024);
    capture.descriptor.mountedPath="ws1/log.txt";QVERIFY(FileReadSnapshotStore::commit(lease,capture,nullptr,&error).isEmpty());QVERIFY(!error.isEmpty());
    ToolExecutor tools;tools.setShellReadSource(logs.formattedSource());QSignalSpy done(&tools,&ToolExecutor::completed);tools.execute("raw","readBytes",{{"path",Path}});QTRY_COMPARE(done.size(),1);QVERIFY(done[0][2].toBool());QVERIFY(done[0][1].toString().contains("4MiB"));
 }
 void revokedCallbackAndCanceledRequestCannotDeliver(){
    QTemporaryDir data;SessionLogStore logs(data.path(),"chat");QString error;QVERIFY(logs.create(Id,&error));QVERIFY(logs.append(Id,"keep",&error));ToolExecutor tools;tools.setShellReadSource(logs.formattedSource());QSignalSpy done(&tools,&ToolExecutor::completed);tools.execute("canceled","read",{{"path",Path}});tools.cancel();QCOMPARE(done.size(),1);QVERIFY(done[0][2].toBool());QTest::qWait(50);QCOMPARE(done.size(),1);logs.revoke();tools.execute("revoked","ls",{{"path","shells"}});QTRY_COMPARE(done.size(),2);QVERIFY(done[1][2].toBool());
 }
};
QTEST_GUILESS_MAIN(ShellLogMountTests)
#include "test_shell_log_mount.moc"
