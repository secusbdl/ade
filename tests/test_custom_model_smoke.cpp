#include "core/ProviderStore.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CustomModelSmokeTest:public QObject {
 Q_OBJECT
private slots:
 void actualMainUsesSavedCustomDefinition_data(){
  QTest::addColumn<bool>("removed");
  QTest::newRow("exact-second-instance")<<false;
  QTest::newRow("removed-definition-refused")<<true;
 }
 void actualMainUsesSavedCustomDefinition(){
  QFETCH(bool,removed);QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
  QJsonArray bodies;QList<QByteArray> headers;
  connect(&server,&QTcpServer::newConnection,this,[&]{while(server.hasPendingConnections()){
   auto* socket=server.nextPendingConnection();connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
   connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
    const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
    const auto end=bytes.indexOf("\r\n\r\n");if(end<0||socket->property("done").toBool())return;
    int length=0;for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
    if(bytes.size()<end+4+length)return;
    socket->setProperty("done",true);
    headers.append(bytes.left(end));bodies.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());
    const QByteArray body="data: {\"choices\":[{\"delta\":{\"content\":\"CUSTOM_SMOKE_OK\"},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
   });
  }});
  QTemporaryDir profile,workspace;ProviderConfig first;first.id="one";first.name="Same label";first.model="legacy";first.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
  auto second=first;second.id="two";CustomModelConfig model;model.modelId="custom-smoke";model.providerInstanceId=second.id;model.displayName="Saved custom";model.contextWindowSize=32768;
  model.headers={{"x-custom-fixture","SYNTHETIC_HEADER"}};model.providerOptions={{"user","SYNTHETIC_OPTION"},{"maxCompletionTokens",33}};
  if(removed)second.retiredCustomModelIds={model.modelId};else second.customModels={model};
  QString error;QVERIFY2(ProviderStore::save(profile.filePath("providers.json"),{first,second},&error),qPrintable(error));
  const auto executable=QCoreApplication::applicationDirPath()+"/custom_model_smoke_app.exe";QVERIFY(QFile::exists(executable));
  QProcess process;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("QT_QPA_PLATFORM","offscreen");process.setProcessEnvironment(environment);
  process.start(executable,{"--smoke","--data-dir",profile.path(),"--workspace",workspace.path(),"--provider","two","--model",model.modelId,"--max-output","77","--prompt","Reply with the fixture acknowledgement.","--report",profile.filePath("report.json"),"--timeout","10"});
  QVERIFY(process.waitForStarted(5000));QTRY_COMPARE_WITH_TIMEOUT(process.state(),QProcess::NotRunning,15000);
  QCOMPARE(process.exitStatus(),QProcess::NormalExit);
  if(removed){QCOMPARE(process.exitCode(),2);QCOMPARE(bodies.size(),0);QVERIFY(process.readAllStandardError().contains("unavailable"));return;}
  QCOMPARE(process.exitCode(),0);QCOMPARE(bodies.size(),1);const auto body=bodies[0].toObject();
  QCOMPARE(body.value("model"),QJsonValue(model.modelId));QCOMPARE(body.value("user"),QJsonValue("SYNTHETIC_OPTION"));QCOMPARE(body.value("max_completion_tokens").toInt(),77);QVERIFY(headers[0].contains("SYNTHETIC_HEADER"));
  QFile report(profile.filePath("report.json"));QVERIFY(report.open(QIODevice::ReadOnly));const auto bytes=report.readAll();const auto evidence=QJsonDocument::fromJson(bytes).object();
  QCOMPARE(evidence.value("provider").toObject().value("id"),QJsonValue("two"));QCOMPARE(evidence.value("provider").toObject().value("contextWindowTokens").toInt(),32768);
  QVERIFY(!bytes.contains("SYNTHETIC_HEADER"));QVERIFY(!bytes.contains("SYNTHETIC_OPTION"));
 }
};
QTEST_GUILESS_MAIN(CustomModelSmokeTest)
#include "test_custom_model_smoke.moc"
