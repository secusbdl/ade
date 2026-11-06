#include "core/AgentEngine.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
class Server:public QObject {
public:
    QTcpServer server;QList<QJsonObject> requests;
    Server(){server.listen(QHostAddress::LocalHost);connect(&server,&QTcpServer::newConnection,this,[this]{
        while(server.hasPendingConnections()){
            auto* socket=server.nextPendingConnection();connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                auto bytes=socket->property("input").toByteArray()+socket->readAll();socket->setProperty("input",bytes);
                if(socket->property("received").toBool())return;
                const auto end=bytes.indexOf("\r\n\r\n");if(end<0)return;int size=0;
                for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))size=line.mid(15).trimmed().toInt();
                if(bytes.size()<end+4+size)return;socket->setProperty("received",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,size)).object());
            });
        }
    });}
    ProviderConfig provider(const QString& protocol) const {return {"fixture","Prompt fixture",QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),"synthetic-local-only","fixture-model",protocol};}
};
QString instructions(const QJsonObject& body,const QString& protocol){
    if(protocol=="responses")return body["instructions"].toString();
    if(protocol=="anthropic")return body["system"].toString();
    if(protocol=="google")return body["systemInstruction"].toObject()["parts"].toArray().first().toObject()["text"].toString();
    return body["messages"].toArray().first().toObject()["content"].toString();
}
QStringList toolNames(const QJsonObject& body,const QString& protocol){
    QStringList names;auto tools=body["tools"].toArray();
    if(protocol=="google")tools=tools.isEmpty()?QJsonArray{}:tools.first().toObject()["functionDeclarations"].toArray();
    for(const auto& value:tools){auto tool=value.toObject();if(protocol=="openai")tool=tool["function"].toObject();names.append(tool["name"].toString());}
    return names;
}
QJsonObject tool(const QString& name){return {{"type","function"},{"function",QJsonObject{{"name",name},{"description","Synthetic registered tool"},{"parameters",QJsonObject{{"type","object"},{"properties",QJsonObject{}}}}}}};}
void protocols(){QTest::addColumn<QString>("protocol");for(const auto& p:{"openai","responses","anthropic","google"})QTest::newRow(p)<<QString(p);}
}
class AgentPromptTest:public QObject {
    Q_OBJECT
private slots:
    void unavailableToolsAreNotDescribedAsAvailable_data(){protocols();}
    void unavailableToolsAreNotDescribedAsAvailable(){
        QFETCH(QString,protocol);Server server;AgentEngine engine;engine.setProvider(server.provider(protocol));engine.submit("Describe only what you can actually do.");
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),1,3000);const auto body=server.requests.first();engine.cancel();
        QCOMPARE(toolNames(body,protocol),QStringList{});const auto prompt=instructions(body,protocol);
        QVERIFY2(!prompt.contains("readContext reads"),qPrintable(prompt));QVERIFY(!prompt.contains("API.openApp"));
    }
    void nativeFilesystemWorkflowReachesEveryProvider_data(){protocols();}
    void nativeFilesystemWorkflowReachesEveryProvider(){
        QFETCH(QString,protocol);QTemporaryDir workspace;Server server;AgentEngine engine;engine.setProvider(server.provider(protocol));engine.setWorkspace(workspace.path());engine.submit("Inspect and edit the existing file.");
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),1,3000);const auto body=server.requests.first();engine.cancel();const auto prompt=instructions(body,protocol);const auto names=toolNames(body,protocol);
        QVERIFY(names.contains("read"));QVERIFY(names.contains("multiEdit"));QVERIFY(names.contains("write"));
        QVERIFY2(prompt.contains("Prefer native file tools"),qPrintable(prompt));QVERIFY(prompt.contains("read before"));QVERIFY(prompt.contains("multiEdit"));QVERIFY(prompt.contains("not use shell or sandbox as a shortcut"));
    }
    void authoringMountDoesNotInventSandboxCapability_data(){protocols();}
    void authoringMountDoesNotInventSandboxCapability(){
        QFETCH(QString,protocol);QTemporaryDir apps;Server server;AgentEngine engine;engine.setProvider(server.provider(protocol));QVERIFY(engine.setAppsDirectory(apps.path()));engine.submit("Create a local app file.");
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),1,3000);const auto body=server.requests.first();engine.cancel();const auto prompt=instructions(body,protocol);
        QVERIFY(toolNames(body,protocol).contains("write"));QVERIFY(!toolNames(body,protocol).contains("executeSandboxJs"));QVERIFY2(!prompt.contains("API.openApp"),qPrintable(prompt));QVERIFY(prompt.contains("apps/"));
    }
    void suppliedContextAndToolsRemainExplicit_data(){protocols();}
    void suppliedContextAndToolsRemainExplicit(){
        QFETCH(QString,protocol);QTemporaryDir apps;Server server;AgentEngine engine;engine.setProvider(server.provider(protocol));QVERIFY(engine.setAppsDirectory(apps.path()));
        engine.setExternalTools({tool("readContext"),tool("executeSandboxJs")});engine.setSystemContext("<fixture-context>Exact approved context.</fixture-context>");engine.setDynamicSystemContext([]{return QString("<shells>Current session manifest.</shells>");});
        engine.submit("Use only available capabilities.");QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),1,3000);const auto body=server.requests.first();engine.cancel();const auto prompt=instructions(body,protocol);
        QVERIFY(toolNames(body,protocol).contains("readContext"));QVERIFY(toolNames(body,protocol).contains("executeSandboxJs"));
        QVERIFY(prompt.contains("readContext reads"));QVERIFY(prompt.contains("API.openApp"));QVERIFY(prompt.contains("<fixture-context>Exact approved context.</fixture-context>"));QVERIFY(prompt.contains("<shells>Current session manifest.</shells>"));
        QVERIFY(prompt.contains("do not assume a Node.js"));
    }
    void dynamicContextCancellationStillPreventsWireRequest(){
        Server server;AgentEngine engine;engine.setProvider(server.provider("openai"));engine.setDynamicSystemContext([&]{engine.cancel();return QString("Canceled context");});engine.submit("Do not send after cancellation.");
        QTest::qWait(100);QVERIFY(server.requests.isEmpty());QVERIFY(!engine.isBusy());
    }
};
QTEST_GUILESS_MAIN(AgentPromptTest)
#include "test_agent_prompt.moc"
