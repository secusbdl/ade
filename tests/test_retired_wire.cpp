#include "SandboxHost.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <iostream>
using namespace ade;
class WireTests:public QObject{
 Q_OBJECT
private slots:
 void retiredWireCannotBecomeBackgroundRpc(){
  QTemporaryDir data;const auto exe=data.filePath("sandbox-wire-fixture.exe");QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),exe));SandboxHost host;host.setWorkerExecutable(exe);host.setWorkspace(data.path());QSignalSpy requests(&host,&SandboxHost::fileRequest),done(&host,&SandboxHost::completed);host.execute("old","executeSandboxJs",{{"script","fixture"},{"explanation","Fixture"}});QTRY_COMPARE(done.size(),1);
  const auto background=[&]{for(const auto& row:requests)if(row[0].toString()=="background-request")return true;return false;};QTRY_VERIFY(background());QTest::qWait(20);bool stale=false;for(const auto& row:requests)if(row[0].toString()=="stale-request")stale=true;QVERIFY2(!stale,"Retired nonempty wire ID reached the native file broker");QCOMPARE(requests.size(),1);
 }
};
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 if(QCoreApplication::applicationFilePath().endsWith("sandbox-wire-fixture.exe")){
  std::string line;while(std::getline(std::cin,line)){const auto input=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();if(input["type"]!="execute")continue;
   const auto send=[](const QJsonObject& row){const auto bytes=QJsonDocument(row).toJson(QJsonDocument::Compact);std::cout<<bytes.constData()<<std::endl;};
   send({{"type","result"},{"id",input["id"]},{"output","done"},{"error",false}});
   QJsonObject rpc{{"type","rpc"},{"id",input["id"]},{"agentId",input["agentId"]},{"contextId",input["contextId"]},{"requestId","stale-request"},{"kind","writeFile"},{"arguments",QJsonObject{{"path","ws1/stale.txt"},{"content","stale"}}}};send(rpc);rpc["id"]="";rpc["requestId"]="background-request";send(rpc);
  }return 0;
 }
 WireTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "test_retired_wire.moc"
