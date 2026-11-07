#include "harness/CodexHarnessSession.h"
#include "harness/AcpHarnessSession.h"
#include "harness/ClaudeHarnessSession.h"
#include "harness/HarnessAttachmentStore.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
using namespace ade;
namespace {
QString digest(const QByteArray &b){return QString::fromLatin1(QCryptographicHash::hash(b,QCryptographicHash::Sha256).toHex());}
QList<QJsonObject> rows(const QString &p){
  QFile f(p);QList<QJsonObject> result;
  if(f.open(QIODevice::ReadOnly))for(const auto &line:f.readAll().split('\n'))
    if(!line.trimmed().isEmpty())result.append(QJsonDocument::fromJson(line).object());
  return result;
}
}
class HarnessAttachmentWireTest:public QObject {
 Q_OBJECT
 void run(const QString &protocol,const QString &root,const HarnessAttachments &attachments,
          const QJsonArray &input,QJsonObject *out,QStringList *errors){
  const auto executable=QDir(QCoreApplication::applicationDirPath()).filePath(
    protocol=="codex"?"attachment_codex_fixture.exe":protocol=="acp"?"attachment_acp_fixture.exe":"attachment_claude_fixture.exe");
  if(protocol=="codex"){
    CodexHarnessSession s;CodexHarnessOptions o;o.executable=executable;o.workspace=root;o.providerId="fixture";o.model="model-a";o.attachments=attachments;
    connect(&s,&CodexHarnessSession::failed,this,[&](const QString &e){errors->append(e);});
    connect(&s,&CodexHarnessSession::finished,this,[&](const QJsonObject &v){*out=v;});
    QVERIFY(s.startTurn(o,input));QTRY_VERIFY_WITH_TIMEOUT(!out->isEmpty(),10000);s.shutdown();
  }else if(protocol=="acp"){
    AcpHarnessSession s;AcpHarnessOptions o;o.executable=executable;o.workspace=root;o.providerId="fixture";o.model="model-a";o.attachments=attachments;
    connect(&s,&AcpHarnessSession::failed,this,[&](const QString &e){errors->append(e);});
    connect(&s,&AcpHarnessSession::finished,this,[&](const QJsonObject &v){*out=v;});
    QVERIFY(s.startTurn(o,input));QTRY_VERIFY_WITH_TIMEOUT(!out->isEmpty(),10000);s.shutdown();
  }else{
    ClaudeHarnessSession s;ClaudeHarnessOptions o;o.executable=executable;o.workspace=root;o.providerId="fixture";o.model="fixture-model";o.attachments=attachments;
    connect(&s,&ClaudeHarnessSession::failed,this,[&](const QString &e){errors->append(e);});
    connect(&s,&ClaudeHarnessSession::finished,this,[&](const QJsonObject &v){*out=v;});
    QVERIFY(s.startTurn(o,input));QTRY_VERIFY_WITH_TIMEOUT(!out->isEmpty(),10000);s.shutdown();
  }
 }
private slots:
 void cleanup(){
  qunsetenv("ADE_ATTACHMENT_LOG");qunsetenv("ADE_ATTACHMENT_ROOT");qunsetenv("ADE_CLAUDE_ARGS_LOG");
 }
 void exactByteFilesReachActualNativeProcess_data(){
  QTest::addColumn<QString>("protocol");QTest::addColumn<QByteArray>("bytes");
  for(const auto &p:QStringList{"codex","acp","claude"}){
    QTest::newRow(qPrintable(p+"-binary"))<<p<<QByteArray("A\0B\xff",4);
    QTest::newRow(qPrintable(p+"-empty"))<<p<<QByteArray("");
    QTest::newRow(qPrintable(p+"-bom"))<<p<<QByteArray::fromHex("efbbbf610d0a");
  }
 }
 void exactByteFilesReachActualNativeProcess(){
  QFETCH(QString,protocol);QFETCH(QByteArray,bytes);QTemporaryDir temp;
  const auto root=temp.path()+"/profile # Unicode-é";QVERIFY(QDir().mkdir(root));const auto log=temp.filePath("wire.jsonl");
  qputenv("ADE_ATTACHMENT_LOG",log.toUtf8());qputenv("ADE_ATTACHMENT_ROOT",root.toUtf8());
  qputenv("ADE_CLAUDE_ARGS_LOG",temp.filePath("args.jsonl").toUtf8());
  HarnessAttachmentStore store(root,"chat-a");QString error;
  auto lease=store.prepare({{"att/original.bin","Original %.bin",digest(bytes),bytes}},nullptr,&error);
  QVERIFY2(lease.valid(),qPrintable(error));QJsonObject outcome;QStringList errors;
  run(protocol,root,{lease,store.retained(),{}},{QJsonObject{{"type","text"},{"text","Read the attached file."}}},&outcome,&errors);
  QCOMPARE(errors,QStringList{});QCOMPARE(outcome["status"].toString(),"completed");
  QJsonArray observed;bool permissionSeen=false;
  for(const auto &row:rows(log)){
    for(const auto &file:row["files"].toArray())observed.append(file);
    const auto frame=row["frame"].toObject();
    if(protocol=="codex"&&frame["method"]=="thread/start"){
      const auto params=frame["params"].toObject();QCOMPARE(params["cwd"].toString(),root);
      QCOMPARE(params["approvalPolicy"].toString(),"on-request");QCOMPARE(params["sandbox"].toString(),"read-only");permissionSeen=true;
    }
    if(protocol=="acp"&&frame["method"]=="session/new"){
      QCOMPARE(frame["params"].toObject()["cwd"].toString(),root);
      QVERIFY(!frame["params"].toObject().contains("additionalDirectories"));permissionSeen=true;
    }
  }
  QCOMPARE(observed.size(),1);const auto file=observed.first().toObject();
  QVERIFY(!file.contains("error"));QCOMPARE(file["sha256"].toString(),digest(bytes));QCOMPARE(file["size"].toInteger(),bytes.size());
  if(protocol=="claude"){
    QFile args(temp.filePath("args.jsonl"));QVERIFY(args.open(QIODevice::ReadOnly));const auto wire=args.readAll();
    QVERIFY(wire.contains("--permission-mode"));QVERIFY(wire.contains("default"));QVERIFY(!wire.contains("--add-dir"));QVERIFY(!wire.contains("bypassPermissions"));
  }else QVERIFY(permissionSeen);
 }
 void acpUnadvertisedImageFallsBackToOriginalFile(){
  QTemporaryDir temp;const auto log=temp.filePath("wire.jsonl");
  qputenv("ADE_ATTACHMENT_LOG",log.toUtf8());qputenv("ADE_ATTACHMENT_ROOT",temp.path().toUtf8());
  HarnessAttachmentStore store(temp.path(),"a");const QByteArray bytes("exact-original-image");
  auto lease=store.prepare({{"att/image.png","image.png",digest(bytes),bytes}},nullptr);QVERIFY(lease.valid());
  QJsonObject result;QStringList errors;
  run("acp",temp.path(),{lease,store.retained(),{"att/image.png"}},
      {QJsonObject{{"type","text"},{"text","image"}},QJsonObject{{"type","image"},{"data","YQ=="},{"mimeType","image/png"}}},&result,&errors);
  QCOMPARE(errors,QStringList{});QCOMPARE(result["status"].toString(),"completed");
  int found=0;
  for(const auto &row:rows(log))for(const auto &v:row["files"].toArray()){
    QCOMPARE(v.toObject()["sha256"].toString(),digest(bytes));++found;
  }
  QCOMPARE(found,1);
 }
};
QTEST_GUILESS_MAIN(HarnessAttachmentWireTest)
#include "test_harness_attachment_wire.moc"

