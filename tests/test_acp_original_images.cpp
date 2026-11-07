#include "harness/AcpPromptPayload.h"
#include "harness/AcpHarnessSession.h"
#include "harness/JsonRpcProcess.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QString sha(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
QString exe(){return QCoreApplication::applicationDirPath()+"/acp_original_fixture.exe";}
HarnessAttachmentCapture capture(QString path,QString name,const QByteArray &bytes){return {path,name,sha(bytes),bytes};}
HarnessAttachments policy(const HarnessAttachmentLease &lease){HarnessAttachments result;result.current=lease;result.acpOriginalImages=true;return result;}
AcpPromptPayload build(const HarnessAttachments &attachments,bool images=true,QString *error=nullptr,const QJsonArray &prefix={}) {return prepareAcpPrompt("owned-session",prefix,{QJsonObject{{"type","text"},{"text","inspect"}}},attachments,images,true,nullptr,error);}
}
class OriginalImageTest:public QObject{
 Q_OBJECT
private slots:
 void cleanup(){qunsetenv("ADE_ORIGINAL_FIXTURE");qunsetenv("ADE_ORIGINAL_LOG");}
 void exactAndFallback_data(){
  QTest::addColumn<int>("size");QTest::addColumn<QString>("name");QTest::addColumn<bool>("images");QTest::addColumn<bool>("inlineExpected");
  QTest::newRow("small-original")<<17<<"odd.png"<<true<<true;
  QTest::newRow("twenty-MiB")<<20*1024*1024<<"Large.PNG"<<true<<true;
  QTest::newRow("above-twenty")<<20*1024*1024+1<<"large.png"<<true<<false;
  QTest::newRow("actual-no-images")<<1024<<"a.webp"<<false<<false;
  QTest::newRow("unknown-extension")<<1024<<"a.bin"<<true<<false;
  QTest::newRow("leading-only-dot")<<1024<<".png"<<true<<false;
 }
 void exactAndFallback(){
  QFETCH(int,size);QFETCH(QString,name);QFETCH(bool,images);QFETCH(bool,inlineExpected);
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"owner");QString error;const QByteArray bytes(size,'z');
  auto lease=store.prepare({capture("att/file",name,bytes)},nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));auto payload=build(policy(lease),images,&error);QVERIFY2(payload.valid(),qPrintable(error));
  JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started),response(&process,&JsonRpcProcess::response),failed(&process,&JsonRpcProcess::failed);
  QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  auto id=process.requestAcpPrompt(payload,15000);QVERIFY(!id.isEmpty());
  QTRY_COMPARE_WITH_TIMEOUT(response.size(),1,20000);QVERIFY(failed.isEmpty());QCOMPARE(response[0][0].toString(),id);
  auto result=response[0][1].value<QJsonValue>().toObject();auto parts=result["parts"].toArray();QCOMPARE(parts.size(),2);auto part=parts[1].toObject();
  QCOMPARE(part["type"].toString(),inlineExpected?QString("image"):QString("resource_link"));
  if(inlineExpected){QCOMPARE(part["sha256"].toString(),sha(bytes));QCOMPARE(part["size"].toInt(),size);}
  QVERIFY(result["frameBytes"].toInteger()<=AcpPromptPayload::MaxFrameBytes);QVERIFY(process.peakQueuedWriteBytes()<=1024*1024);
 }
 void aggregatePreservesOrderedFileFallback(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"owner");QString error;QByteArray bytes(20*1024*1024,'a');QList<HarnessAttachmentCapture> files;
  for(int i=0;i<3;++i)files.append(capture("att/"+QString::number(i),QString::number(i)+".png",bytes));
  auto lease=store.prepare(files,nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));auto payload=build(policy(lease),true,&error);QVERIFY2(payload.valid(),qPrintable(error));
  QVERIFY(payload.byteSize()>50*1024*1024);QVERIFY(payload.byteSize()<64*1024*1024);
  JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started),response(&process,&JsonRpcProcess::response);QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  const auto id=process.requestAcpPrompt(payload,20000);QVERIFY(!id.isEmpty());const auto after=process.request("after",{},20000);QVERIFY(!after.isEmpty());
  QTRY_COMPARE_WITH_TIMEOUT(response.size(),2,25000);QCOMPARE(response[0][0].toString(),id);QCOMPARE(response[1][0].toString(),after);
  auto parts=response[0][1].value<QJsonValue>().toObject()["parts"].toArray();QCOMPARE(parts.size(),4);QCOMPARE(parts[1].toObject()["type"].toString(),QString("image"));QCOMPARE(parts[2].toObject()["type"].toString(),QString("image"));QCOMPARE(parts[3].toObject()["type"].toString(),QString("resource_link"));QVERIFY(process.peakQueuedWriteBytes()<=1024*1024);
 }
 void strictEnvelopeAndAuthority(){
  QTemporaryDir temp;HarnessAttachmentStore a(temp.path(),"a"),b(temp.path(),"b");QString error;auto first=a.prepare({capture("att/a","a.png","bytes")},nullptr,&error);auto second=b.prepare({capture("att/b","b.png","bytes")},nullptr,&error);
  auto attachments=policy(first);attachments.history=second;QVERIFY(!build(attachments,true,&error).valid());QVERIFY(error.contains("different chats"));
  attachments=policy(first);auto text=QJsonArray{QJsonObject{{"type","text"},{"text",QString(4*1024*1024,'x')}}};QVERIFY(!build(attachments,true,&error,text).valid());QVERIFY(error.contains("4 MiB"));
  attachments.acpOriginalImages=false;QVERIFY(!build(attachments,true,&error).valid());
  attachments=policy(first);a.revoke();QVERIFY(!build(attachments,true,&error).valid());
 }
 void historyNeverInlined(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto old=store.prepare({capture("att/old","old.png","original")},nullptr,&error);HarnessAttachments input;input.history=old;input.acpOriginalImages=true;auto payload=build(input,true,&error);QVERIFY2(payload.valid(),qPrintable(error));QVERIFY(payload.byteSize()<4096);
 }
 void cancellationBeforeWriteLeavesOwnedProcessReusable(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png",QByteArray(1024,'x'))},nullptr,&error);auto payload=build(policy(lease),true,&error);
  JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started),response(&process,&JsonRpcProcess::response);QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  const auto id=process.requestAcpPrompt(payload,2000);QVERIFY(!id.isEmpty());QCOMPARE(process.cancelAcpPromptWrite(id),JsonRpcProcess::PromptWriteCancel::RemovedBeforeWrite);QVERIFY(process.isRunning());const auto next=process.request("replacement",{},2000);QVERIFY(!next.isEmpty());QTRY_COMPARE(response.size(),1);QCOMPARE(response[0][0].toString(),next);
 }
 void partialWriteCancellationStopsOnlyOwnedProcess(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png",QByteArray(20*1024*1024,'x'))},nullptr,&error);auto payload=build(policy(lease),true,&error);
  qputenv("ADE_ORIGINAL_FIXTURE","hold");JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started);QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  const auto id=process.requestAcpPrompt(payload,10000);QVERIFY(!id.isEmpty());QTRY_VERIFY(process.peakQueuedWriteBytes()>0);QCOMPARE(process.cancelAcpPromptWrite(id),JsonRpcProcess::PromptWriteCancel::StoppedPartial);QVERIFY(!process.isRunning());
  qunsetenv("ADE_ORIGINAL_FIXTURE");QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),2);QSignalSpy response(&process,&JsonRpcProcess::response);const auto next=process.request("new-generation",{},2000);QTRY_COMPARE(response.size(),1);QCOMPARE(response[0][0].toString(),next);
 }
 void controlAndInboundLimitsStaySmall(){
  QTemporaryDir temp;JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started),failed(&process,&JsonRpcProcess::failed);
  qputenv("ADE_ORIGINAL_FIXTURE","oversized-output");QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  QVERIFY(process.request("control",{{"text",QString(8*1024*1024,'x')}},1000).isEmpty());QVERIFY(!process.request("small",{},2000).isEmpty());QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,5000);QVERIFY(!process.isRunning());
 }
 void finalQueuedBytesRetainCapturedLease(){
  QTemporaryDir temp;QString error;AcpPromptPayload payload;
  {HarnessAttachmentStore store(temp.path(),"a");auto lease=store.prepare({capture("att/a","a.png",QByteArray(512*1024,'x'))},nullptr,&error);payload=build(policy(lease),true,&error);QVERIFY2(payload.valid(),qPrintable(error));}
  qputenv("ADE_ORIGINAL_FIXTURE","hold");JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started);QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);QVERIFY(!process.requestAcpPrompt(payload,10000).isEmpty());payload={};QTRY_VERIFY(process.peakQueuedWriteBytes()>0);
  const auto child=process.findChild<QProcess*>();QVERIFY(child);QVERIFY(child->bytesToWrite()>0);
  QVERIFY2(!HarnessAttachmentStore::removeAgent(temp.path(),"a",&error),"The final queued frame must retain its captured lease until the pipe drains.");
  process.stop();QVERIFY2(HarnessAttachmentStore::removeAgent(temp.path(),"a",&error),qPrintable(error));
 }
 void revokedPartialWriteStopsOnlyItsProcess_data(){QTest::addColumn<int>("size");QTest::newRow("multi-frame-buffer")<<20*1024*1024;QTest::newRow("final-buffer-only")<<512*1024;}
 void revokedPartialWriteStopsOnlyItsProcess(){
  QFETCH(int,size);
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png",QByteArray(size,'x'))},nullptr,&error);auto payload=build(policy(lease),true,&error);QVERIFY2(payload.valid(),qPrintable(error));
  qputenv("ADE_ORIGINAL_FIXTURE","hold");JsonRpcProcess process,sentinel;QSignalSpy started(&process,&JsonRpcProcess::started),otherStarted(&sentinel,&JsonRpcProcess::started),failed(&process,&JsonRpcProcess::failed);
  QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QVERIFY(sentinel.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);QTRY_COMPARE(otherStarted.size(),1);
  QVERIFY(!process.requestAcpPrompt(payload,10000).isEmpty());QTRY_VERIFY(process.peakQueuedWriteBytes()>0);const auto pipe=process.findChild<QProcess*>();qInfo()<<"pending before revoke"<<(pipe?pipe->bytesToWrite():-1)<<"original bytes"<<size;store.revoke();QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,2000);QVERIFY(!process.isRunning());QVERIFY(sentinel.isRunning());
 }
 void partialWriteTimeoutStopsUnsafeReuse(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png",QByteArray(20*1024*1024,'x'))},nullptr,&error);auto payload=build(policy(lease),true,&error);QVERIFY2(payload.valid(),qPrintable(error));
  qputenv("ADE_ORIGINAL_FIXTURE","hold");JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started),response(&process,&JsonRpcProcess::response);QVERIFY(process.start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);
  const auto id=process.requestAcpPrompt(payload,100);QVERIFY(!id.isEmpty());QTRY_COMPARE_WITH_TIMEOUT(response.size(),1,3000);QCOMPARE(response[0][0].toString(),id);QCOMPARE(response[0][2].toJsonObject()["code"].toInt(),-32001);QVERIFY(!process.isRunning());
 }
 void timeoutObserverCanDeleteOrReplace_data(){QTest::addColumn<bool>("destroy");QTest::newRow("delete")<<true;QTest::newRow("replace")<<false;}
 void timeoutObserverCanDeleteOrReplace(){
  QFETCH(bool,destroy);QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png",QByteArray(20*1024*1024,'x'))},nullptr,&error);auto payload=build(policy(lease),true,&error);QVERIFY2(payload.valid(),qPrintable(error));
  qputenv("ADE_ORIGINAL_FIXTURE","hold");QPointer<JsonRpcProcess> process=new JsonRpcProcess;QSignalSpy started(process,&JsonRpcProcess::started),responses(process,&JsonRpcProcess::response),failed(process,&JsonRpcProcess::failed);QString original;bool observed=false;
  connect(process,&JsonRpcProcess::response,this,[&](const QString &id,const QJsonValue &,const QJsonObject &e){if(id!=original||e["code"].toInt()!=-32001)return;observed=true;if(destroy){delete process.data();return;}qunsetenv("ADE_ORIGINAL_FIXTURE");process->start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment());});
  QVERIFY(process->start(exe(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);original=process->requestAcpPrompt(payload,100);QVERIFY(!original.isEmpty());QTRY_VERIFY_WITH_TIMEOUT(observed,3000);
  if(destroy)QVERIFY(!process);else{QTRY_COMPARE(started.size(),2);QVERIFY(failed.isEmpty());auto id=process->request("replacement",{},2000);QVERIFY(!id.isEmpty());QTRY_COMPARE(responses.size(),2);QCOMPARE(responses.last()[0].toString(),id);delete process.data();}
 }
 void cancelledPreparation(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"a");QString error;auto lease=store.prepare({capture("att/a","a.png","bytes")},nullptr,&error);auto attachments=policy(lease);std::atomic_bool stopped(true);auto payload=prepareAcpPrompt("session",{}, {},attachments,true,true,&stopped,&error);QVERIFY(!payload.valid());QVERIFY(error.contains("cancelled"));
 }
};
QTEST_GUILESS_MAIN(OriginalImageTest)
#include "test_acp_original_images.moc"
