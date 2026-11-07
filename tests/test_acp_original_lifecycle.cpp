#include "harness/AcpHarnessSession.h"
#include "harness/AcpPromptPayload.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
using namespace ade;
namespace {
QString executable(){return QCoreApplication::applicationDirPath()+"/acp_original_fixture.exe";}
HarnessAttachmentCapture image(const QString &ref,qint64 size){const QByteArray bytes(size,'i');return {ref,"fixture.png",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()),bytes};}
AcpHarnessOptions options(const QString &root,const HarnessAttachmentLease &lease){AcpHarnessOptions o;o.executable=executable();o.providerId="owned";o.workspace=root;o.promptTimeoutMs=5000;o.attachments.current=lease;o.attachments.acpOriginalImages=true;return o;}
QJsonArray input(){return {QJsonObject{{"type","text"},{"text","inspect"}}};}
int prompts(const QString &log){QFile f(log);int count=0;if(f.open(QIODevice::ReadOnly))for(const auto &line:f.readAll().split('\n'))count+=QJsonDocument::fromJson(line).object()["method"]=="session/prompt";return count;}
bool alive(qint64 pid){
#ifdef Q_OS_WIN
 auto h=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));if(!h)return false;bool live=WaitForSingleObject(h,0)==WAIT_TIMEOUT;CloseHandle(h);return live;
#else
 Q_UNUSED(pid)return false;
#endif
}
}
class OriginalLifecycleTest:public QObject{
 Q_OBJECT
private slots:
 void cleanup(){qunsetenv("ADE_ORIGINAL_LOG");qunsetenv("ADE_ORIGINAL_FIXTURE");QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));}
 void actualCapabilityControlsMapping_data(){QTest::addColumn<bool>("supports");QTest::newRow("supported")<<true;QTest::newRow("unsupported")<<false;}
 void actualCapabilityControlsMapping(){
  QFETCH(bool,supports);QTemporaryDir temp;auto log=temp.filePath("summary");qputenv("ADE_ORIGINAL_LOG",log.toUtf8());if(!supports)qputenv("ADE_ORIGINAL_FIXTURE","no-images");
  HarnessAttachmentStore store(temp.path(),"owner");QString error;auto lease=store.prepare({image("att/a",1024)},nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));
  AcpHarnessSession session;QSignalSpy done(&session,&AcpHarnessSession::finished);QVERIFY(session.startTurn(options(temp.path(),lease),input()));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QCOMPARE(done[0][0].toJsonObject()["status"].toString(),QString("completed"));
  QFile f(log);QVERIFY(f.open(QIODevice::ReadOnly));QJsonArray parts;for(const auto &line:f.readAll().split('\n')){auto row=QJsonDocument::fromJson(line).object();if(row["method"]=="session/prompt")parts=row["result"].toObject()["parts"].toArray();}
  QCOMPARE(parts.size(),2);QCOMPARE(parts.last().toObject()["type"].toString(),supports?QString("image"):QString("resource_link"));
 }
 void cancelWhilePreparingThenReplacement(){
  QTemporaryDir temp;auto log=temp.filePath("summary");qputenv("ADE_ORIGINAL_LOG",log.toUtf8());HarnessAttachmentStore store(temp.path(),"owner");QString error;auto lease=store.prepare({image("att/a",20*1024*1024)},nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));
  AcpHarnessSession session;QSignalSpy done(&session,&AcpHarnessSession::finished);bool observed=false;QTimer observer;observer.setInterval(0);connect(&observer,&QTimer::timeout,this,[&]{if(!session.findChildren<QFutureWatcherBase*>().isEmpty()){observed=true;observer.stop();session.interrupt();}});observer.start();
  QVERIFY(session.startTurn(options(temp.path(),lease),input()));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(observed);QCOMPARE(done[0][0].toJsonObject()["status"].toString(),QString("interrupted"));QCOMPARE(prompts(log),0);
  auto next=options(temp.path(),{});QVERIFY(session.startTurn(next,input()));QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,10000);QCOMPARE(done[1][0].toJsonObject()["status"].toString(),QString("completed"));QCOMPARE(prompts(log),1);
 }
 void ownerDeletionDuringPreparationStopsChild(){
  QTemporaryDir temp;auto log=temp.filePath("summary");qputenv("ADE_ORIGINAL_LOG",log.toUtf8());HarnessAttachmentStore store(temp.path(),"owner");QString error;auto lease=store.prepare({image("att/a",20*1024*1024)},nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));
  QPointer<AcpHarnessSession> session=new AcpHarnessSession;bool observed=false;qint64 pid=0;QTimer observer;observer.setInterval(0);connect(&observer,&QTimer::timeout,this,[&]{if(session&&!session->findChildren<QFutureWatcherBase*>().isEmpty()){observed=true;observer.stop();const auto child=session->findChild<QProcess*>();if(child)pid=child->processId();delete session.data();}});observer.start();
  QVERIFY(session->startTurn(options(temp.path(),lease),input()));QTRY_VERIFY_WITH_TIMEOUT(!session,10000);QVERIFY(observed);QVERIFY(pid>0);QTRY_VERIFY_WITH_TIMEOUT(!alive(pid),2000);QVERIFY(QThreadPool::globalInstance()->waitForDone(10000));QCOMPARE(prompts(log),0);
 }
 void preparedPayloadRevocationAndWriterDeadline(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"owner");QString error;auto lease=store.prepare({image("att/a",20*1024*1024)},nullptr,&error);auto o=options(temp.path(),lease);auto payload=prepareAcpPrompt("session",{},input(),o.attachments,true,true,nullptr,&error);QVERIFY2(payload.valid(),qPrintable(error));store.revoke();QVERIFY(!payload.valid());
  JsonRpcProcess process;QSignalSpy started(&process,&JsonRpcProcess::started);QVERIFY(process.start(executable(),{},temp.path(),JsonRpcProcess::managedEnvironment()));QTRY_COMPARE(started.size(),1);QVERIFY(process.requestAcpPrompt(payload,1000).isEmpty());
 }
 void aggregateMemoryRemainsBounded(){
  QTemporaryDir temp;HarnessAttachmentStore store(temp.path(),"owner");QString error;QList<HarnessAttachmentCapture> files;for(int i=0;i<3;++i)files.append(image("att/"+QString::number(i),20*1024*1024));auto lease=store.prepare(files,nullptr,&error);QVERIFY2(lease.valid(),qPrintable(error));files.clear();auto o=options(temp.path(),lease);
  auto payload=prepareAcpPrompt("session",{},input(),o.attachments,true,true,nullptr,&error);QVERIFY2(payload.valid(),qPrintable(error));QVERIFY(payload.byteSize()<64*1024*1024);
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);QVERIFY(GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)));qInfo()<<"fixture process peak working set bytes"<<qulonglong(counters.PeakWorkingSetSize)<<"serialized payload bytes"<<payload.byteSize();QVERIFY(counters.PeakWorkingSetSize<512ULL*1024*1024);
#endif
 }
};
QTEST_GUILESS_MAIN(OriginalLifecycleTest)
#include "test_acp_original_lifecycle.moc"
