#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <cstdio>
#include <iostream>
static void send(const QJsonObject &o) { auto b=QJsonDocument(o).toJson(QJsonDocument::Compact)+'\n'; fwrite(b.constData(),1,b.size(),stdout);fflush(stdout); }
int main(int argc,char **argv) {
 QCoreApplication app(argc,argv);
 const auto mode=qEnvironmentVariable("ADE_ORIGINAL_FIXTURE");
 if(mode=="hold") { QThread::sleep(30);return 0; }
 std::string line;
 while(std::getline(std::cin,line)) {
  if(line.size()>65*1024*1024) return 20;
  QJsonParseError error;const auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(line),&error);
  if(error.error!=QJsonParseError::NoError||!doc.isObject())return 21;
  auto o=doc.object();const auto method=o["method"].toString();const auto params=o["params"].toObject();
  QJsonObject result;
  if(method=="initialize") result={{"protocolVersion",1},{"agentCapabilities",QJsonObject{{"promptCapabilities",QJsonObject{{"image",mode!="no-images"}}}}}};
  else if(method=="session/new"||method=="session/resume") result={{"sessionId","image-fixture"}};
  else if(method=="session/prompt") {
   QJsonArray parts;
   for(const auto &v:params["prompt"].toArray()) {
    auto part=v.toObject();
    if(part["type"]=="image") {auto bytes=QByteArray::fromBase64(part["data"].toString().toLatin1());parts.append(QJsonObject{{"type","image"},{"size",bytes.size()},{"mimeType",part["mimeType"]},{"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});}
    else if(part["type"]=="resource_link") parts.append(part);
    else parts.append(QJsonObject{{"type",part["type"]}});
   }
   result={{"stopReason","end_turn"},{"parts",parts},{"frameBytes",qint64(line.size())}};
  } else result={{"method",method}};
  QFile log(qEnvironmentVariable("ADE_ORIGINAL_LOG"));if(!log.fileName().isEmpty()&&log.open(QIODevice::Append))log.write(QJsonDocument(QJsonObject{{"method",method},{"result",result}}).toJson(QJsonDocument::Compact)+'\n');
  if(mode=="oversized-output") {QByteArray huge(8*1024*1024+1,'x');fwrite(huge.constData(),1,huge.size(),stdout);fflush(stdout);QThread::sleep(2);return 0;}
  if(o.contains("id")) send({{"jsonrpc","2.0"},{"id",o["id"]},{"result",result}});
 }
 return 0;
}
