#include "runtime/AgentToolHost.h"
#include "core/AgentEngine.h"
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray event(const QJsonObject& value){return "data: "+QJsonDocument(value).toJson(QJsonDocument::Compact)+"\n\n";}
QByteArray reply(const QJsonObject& arguments={},const QString& protocol="openai"){
 if(protocol=="responses")return event({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",QJsonArray{QJsonObject{{"type","message"},{"id","response"},{"role","assistant"},{"content",QJsonArray{QJsonObject{{"type","output_text"},{"text","BUDGET_DONE"}}}}}}}}}});
 if(protocol=="anthropic")return event({{"type","message_start"},{"message",QJsonObject{{"role","assistant"}}}})+event({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","text"},{"text","BUDGET_DONE"}}}})+event({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","end_turn"}}}})+event({{"type","message_stop"}});
 if(protocol=="google")return event({{"candidates",QJsonArray{QJsonObject{{"content",QJsonObject{{"role","model"},{"parts",QJsonArray{QJsonObject{{"text","BUDGET_DONE"}}}}}},{"finishReason","STOP"}}}}});
 QJsonObject delta;
 if(arguments.isEmpty())delta["content"]="BUDGET_DONE";
 else delta["tool_calls"]=QJsonArray{QJsonObject{{"index",0},{"id","read-budget"},{"type","function"},{"function",QJsonObject{{"name","read"},{"arguments",QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}}}}};
 return event({{"choices",QJsonArray{QJsonObject{{"delta",delta},{"finish_reason",arguments.isEmpty()?"stop":"tool_calls"}}}}})+"data: [DONE]\n\n";
}
class Server:public QObject {
public:
 QTcpServer tcp;QString protocol="openai";QJsonArray requests;QList<QByteArray> replies;bool failFirst=false;
 Server(){tcp.listen(QHostAddress::LocalHost);connect(&tcp,&QTcpServer::newConnection,this,[this]{while(auto* socket=tcp.nextPendingConnection()){
  connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
  connect(socket,&QTcpSocket::readyRead,socket,[this,socket]{
   auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
   const auto end=bytes.indexOf("\r\n\r\n");if(end<0||socket->property("done").toBool())return;int length=0;
   for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
   if(bytes.size()<end+4+length)return;
   socket->setProperty("done",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());
   const bool failed=failFirst&&requests.size()==1;const auto body=failed?QByteArray("{\"error\":\"fixture overload\"}"):(replies.isEmpty()?reply({},protocol):replies.takeFirst());
   socket->write(QByteArray(failed?"HTTP/1.1 503 Service Unavailable\r\n":"HTTP/1.1 200 OK\r\n")+"Content-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();
  });
 }});}
 ProviderConfig provider(QString model="custom-budget",int context=10000)const{ProviderConfig p;p.id="fixture";p.name="Budget fixture";p.model=model;p.contextWindowTokens=context;p.protocol=protocol;p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(tcp.serverPort());return p;}
};
QByteArray lines(){QByteArray data;for(int i=1;i<=6000;++i)data+=QString("BUDGET_%1_").arg(i,5,10,QChar('0')).leftJustified(99,'x').toUtf8()+'\n';return data;}
QByteArray marker(int line){return QString("BUDGET_%1_").arg(line,5,10,QChar('0')).toUtf8();}
QByteArray serialized(const QJsonValue& value){return QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);}
bool write(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
ProviderConfig custom(ProviderConfig p){CustomModelConfig m;m.modelId=p.model;m.providerInstanceId=p.id;m.displayName="Bounded custom";m.contextWindowSize=p.contextWindowTokens;p.customModels={m};p.customRequest={{"providerInstanceId",p.id},{"modelId",p.model},{"providerOptions",m.providerOptions},{"headers",m.headers},{"capabilities",m.capabilities}};return p;}
}
class ContextBudgetHostTest:public QObject {
 Q_OBJECT
private slots:
 void actualAttachmentUsesSelectedContext_data(){
  QTest::addColumn<QString>("model");QTest::addColumn<int>("context");QTest::addColumn<int>("lastLine");QTest::addColumn<bool>("isCustom");QTest::addColumn<QString>("protocol");
  QTest::newRow("small-custom")<<QString("custom-budget")<<10000<<40<<true<<QString("openai");
  QTest::newRow("custom32768")<<QString("custom-budget")<<32768<<131<<true<<QString("openai");
  QTest::newRow("builtin128k")<<QString("qwen3-32b")<<0<<512<<false<<QString("openai");
  QTest::newRow("builtin1m")<<QString("gpt-6.1-sol")<<0<<4200<<false<<QString("openai");
  QTest::newRow("unknown-legacy")<<QString("unknown-fixture")<<0<<400<<false<<QString("openai");
  for(const auto& protocol:QStringList{"responses","anthropic","google"})QTest::newRow(qPrintable(protocol))<<QString("custom-budget")<<10000<<40<<true<<protocol;
 }
 void actualAttachmentUsesSelectedContext(){
  QFETCH(QString,model);QFETCH(int,context);QFETCH(int,lastLine);QFETCH(bool,isCustom);QFETCH(QString,protocol);
  Server server;server.protocol=protocol;QTemporaryDir data;auto provider=server.provider(model,context);if(isCustom)provider=custom(provider);
  AgentEngine engine;engine.setProvider(provider);AgentToolHost host(&engine,nullptr,"budget-chat",data.path());AttachmentStore store(data.path());QString error;
  const auto ref=store.write("budget-chat","budget.txt",lines(),&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));const auto capture=host.captureAttachments({ref.toJson()},&error);QVERIFY2(error.isEmpty(),qPrintable(error));
  QSignalSpy done(&engine,&AgentEngine::finished);QVERIFY(engine.submitWithAttachments("Use this captured text.",capture["attachments"].toArray(),capture["pathReferences"].toObject(),capture["pathMetadata"].toObject()));
  QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(server.requests.size(),1);const auto request=serialized(server.requests[0]);
  QVERIFY2(request.contains(marker(lastLine)),qPrintable(QString("Last allowed line %1 missing").arg(lastLine)));QVERIFY2(!request.contains(marker(lastLine+1)),qPrintable(QString("Line beyond selected context %1 leaked").arg(lastLine+1)));
  QVERIFY(!QJsonDocument(engine.messages()).toJson().contains(marker(1)));QCOMPARE(store.read("budget-chat",ref,{},&error),lines());
 }
 void fallbackRepreparesForExactActiveInstance(){
  Server server;server.failFirst=true;QTemporaryDir data;auto a=server.provider("same-id",10000),b=a;b.id="second-instance";b.contextWindowTokens=100000;
  AgentEngine engine;engine.setModelChain({{custom(a)},{custom(b)}},"budget-fallback");AgentToolHost host(&engine,nullptr,"budget-chat",data.path());QString error;const auto ref=AttachmentStore(data.path()).write("budget-chat","budget.txt",lines(),&error);const auto metadata=host.captureAttachments({ref.toJson()},&error);QVERIFY(error.isEmpty());
  QSignalSpy done(&engine,&AgentEngine::finished);engine.submitWithAttachments("Use context.",metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject());QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(server.requests.size(),2);
  const auto first=serialized(server.requests[0]),second=serialized(server.requests[1]);QVERIFY(first.contains(marker(40)));QVERIFY(!first.contains(marker(41)));QVERIFY(second.contains(marker(400)));QVERIFY(!second.contains(marker(401)));QCOMPARE(engine.provider().id,QString("second-instance"));
 }
 void originalRangeReplaysWithNewBudgetAfterRestart(){
  Server server;QTemporaryDir data,workspace;const auto path=workspace.filePath("budget.txt");QVERIFY(write(path,lines()));QJsonArray saved;
  {AgentEngine engine;engine.setProvider(custom(server.provider()));engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"budget-chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));server.replies={reply({{"path","ws1/budget.txt"},{"start_line",501},{"end_line",1500}})};QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Read the precise range.");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());const auto body=serialized(server.requests.last());QVERIFY(body.contains(marker(501)));QVERIFY(body.contains(marker(540)));QVERIFY(!body.contains(marker(541)));saved=engine.messages();QVERIFY(!QJsonDocument(saved).toJson().contains(marker(501)));}
  QVERIFY(write(path,"REPLACEMENT_MUST_NOT_APPEAR"));AgentEngine reopened;reopened.setProvider(custom(server.provider("custom-budget",100000)));reopened.setMessages(saved);AgentToolHost host(&reopened,nullptr,"budget-chat",data.path());QSignalSpy done(&reopened,&AgentEngine::finished);reopened.submit("Continue from the saved capture.");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(reopened.lastTurnSucceeded());const auto body=serialized(server.requests.last());QVERIFY(body.contains(marker(501)));QVERIFY(body.contains(marker(900)));QVERIFY(!body.contains(marker(901)));QVERIFY(!body.contains(marker(500)));QVERIFY(!body.contains("REPLACEMENT_MUST_NOT_APPEAR"));AgentEngine foreign;foreign.setProvider(server.provider("gpt-6.1-sol",0));foreign.setMessages(saved);AgentToolHost foreignHost(&foreign,nullptr,"other-chat",data.path());QSignalSpy foreignDone(&foreign,&AgentEngine::finished);foreign.submit("Untrusted replay.");QTRY_COMPARE_WITH_TIMEOUT(foreignDone.size(),1,10000);const auto denied=serialized(server.requests.last());QVERIFY(!denied.contains(marker(501)));QVERIFY(denied.contains("Original read could not be included"));
 }
 void previewRetainsThirtyLinePolicy(){Server server;QTemporaryDir data,workspace;QVERIFY(write(workspace.filePath("budget.txt"),lines()));AgentEngine engine;engine.setProvider(custom(server.provider("custom-budget",10000)));engine.setWorkspace(workspace.path());AgentToolHost host(&engine,nullptr,"budget-chat",data.path());QVERIFY(host.setWorkspace(workspace.path()));server.replies={reply({{"path","budget.txt"},{"preview",true}})};QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Preview.");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(engine.lastTurnSucceeded());const auto body=serialized(server.requests.last());QVERIFY(body.contains(marker(30)));QVERIFY(!body.contains(marker(31)));}
};
QTEST_GUILESS_MAIN(ContextBudgetHostTest)
#include "test_context_budget_host.moc"
