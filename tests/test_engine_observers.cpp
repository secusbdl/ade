#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "core/ProviderProtocol.h"
#include "core/ProviderService.h"
#include "core/SseParser.h"
#include "core/ToolExecutor.h"
#include "core/UtilityService.h"
#include "core/ContextLoader.h"

#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ade;

namespace {
struct Response { QByteArray body; int status = 200; bool hold = false; QByteArray type = "text/event-stream"; };
class FixtureServer : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    QList<QByteArray> targets;
    QList<QByteArray> headers;
    std::function<Response(int, const QJsonObject &)> respond;
    FixtureServer() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    const auto headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto &line : buffer.left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer.size() < headerEnd + 4 + length) return;
                    socket->setProperty("handled", true);
                    headers.append(buffer.left(headerEnd));
                    targets.append(buffer.left(buffer.indexOf('\r')).split(' ').value(1));
                    const auto object = QJsonDocument::fromJson(buffer.mid(headerEnd + 4, length)).object();
                    requests.append(object);
                    const auto response = respond ? respond(requests.size(), object) : Response{};
                    if (response.hold) return;
                    const auto header = "HTTP/1.1 " + QByteArray::number(response.status) + " Test\r\nContent-Type: " + response.type
                        + "\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n";
                    socket->write(header);
                    socket->write(response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ProviderConfig provider() const {
        return {"fixture", "Local fixture", QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()),
                "fixture-secret", "fixture-model", "openai"};
    }
};

QByteArray sseEvent(const QJsonObject &object) { return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n"; }
QByteArray textResponse(const QString &text) {
    const QJsonObject delta{{"content", text}};
    const QJsonObject choice{{"delta", delta}, {"finish_reason", "stop"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
}

namespace {
Response observerResponse(const QString &protocol, bool streaming, bool thought) {
    const QJsonObject call{{"id","call-stale"},{"index",0},{"type","function"},
        {"function",QJsonObject{{"name","write"},{"arguments","{\"path\":\"stale.txt\",\"content\":\"STALE\"}"}}}};
    QJsonObject payload; QByteArray body;
    if (protocol == "openai") {
        QJsonObject delta{{"content",thought?"STALE":"FIRST"},{"tool_calls",QJsonArray{call}}};
        if (thought) delta["reasoning_content"]="FIRST";
        payload={{"choices",QJsonArray{QJsonObject{{streaming?"delta":"message",delta},{"finish_reason","tool_calls"}}}}};
        body=sseEvent(payload)+textResponse("STALE");
    } else if (protocol == "anthropic") {
        QJsonArray content;
        content.append(thought?QJsonObject{{"type","thinking"},{"thinking","FIRST"},{"signature","fixture"}}
                               :QJsonObject{{"type","text"},{"text","FIRST"}});
        content.append(QJsonObject{{"type","text"},{"text","STALE"}});
        content.append(QJsonObject{{"type","tool_use"},{"id","call-stale"},{"name","write"},{"input",QJsonObject{{"path","stale.txt"},{"content","STALE"}}}});
        payload={{"content",content},{"stop_reason","tool_use"}};
        int i=0; for(const auto &item:content) {
            body+=sseEvent({{"type","content_block_start"},{"index",i},{"content_block",item}});
            body+=sseEvent({{"type","content_block_stop"},{"index",i++}});
        }
        body+=sseEvent({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","tool_use"}}}});
        body+=sseEvent({{"type","message_stop"}});
    } else if (protocol == "google") {
        QJsonArray parts{QJsonObject{{"text","FIRST"},{"thought",thought}},QJsonObject{{"text","STALE"}},
            QJsonObject{{"functionCall",QJsonObject{{"name","write"},{"args",QJsonObject{{"path","stale.txt"},{"content","STALE"}}}}}}};
        payload={{"candidates",QJsonArray{QJsonObject{{"content",QJsonObject{{"role","model"},{"parts",parts}}},{"finishReason","STOP"}}}}};
        body=sseEvent(payload);
    } else {
        QJsonArray output{QJsonObject{{"type","message"},{"role","assistant"},
            {"content",QJsonArray{QJsonObject{{"type","output_text"},{"text",thought?"":"FIRST"}}}}},
            QJsonObject{{"type","reasoning"},{"summary",QJsonArray{QJsonObject{{"type","summary_text"},{"text",thought?"FIRST":"STALE"}}}}},
            QJsonObject{{"type","function_call"},{"call_id","call-stale"},{"name","write"},{"arguments","{\"path\":\"stale.txt\",\"content\":\"STALE\"}"}}};
        payload={{"status","completed"},{"output",output}};
        body=sseEvent({{"type","response.completed"},{"response",payload}});
    }
    return streaming?Response{body}:Response{QJsonDocument(payload).toJson(QJsonDocument::Compact),200,false,"application/json"};
}
void observerRows(bool replacementRows) {
    QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("streaming"); QTest::addColumn<bool>("thought");
    if(replacementRows) QTest::addColumn<bool>("replacement");
    for(const auto &protocol:QStringList{"openai","anthropic","google","responses"})
        for(bool streaming:{false,true}) for(bool thought:{false,true}) {
            const auto name=protocol+(streaming?"-sse":"-json")+(thought?"-reasoning":"-text");
            if(replacementRows) for(bool replacement:{false,true})
                QTest::newRow(qPrintable(name+(replacement?"-replace":"-cancel")))<<protocol<<streaming<<thought<<replacement;
            else QTest::newRow(qPrintable(name))<<protocol<<streaming<<thought;
        }
}
}
class StreamObserverTest:public QObject {
    Q_OBJECT
private slots:
    void cancellation_data(){observerRows(true);}
    void cancellation(){
        QFETCH(QString,protocol); QFETCH(bool,streaming); QFETCH(bool,thought); QFETCH(bool,replacement);
        QTemporaryDir workspace; FixtureServer server;
        server.respond=[&](int n,const QJsonObject&){return n==1?observerResponse(protocol,streaming,thought):Response{textResponse("REPLACEMENT")};};
        auto provider=server.provider(); provider.protocol=protocol;
        AgentEngine engine;engine.setProvider(provider);engine.setWorkspace(workspace.path());engine.setAlwaysAllow(true);
        QSignalSpy done(&engine,&AgentEngine::finished); QString observed; bool acted=false;
        connect(&engine,&AgentEngine::textDelta,this,[&](const QString&s){observed+=s;});
        connect(&engine,&AgentEngine::reasoningDelta,this,[&](const QString&s){observed+=s;});
        auto action=[&]{if(acted)return;acted=true;engine.cancel();if(replacement){engine.setProvider(server.provider());engine.submit("Replacement");}};
        if(thought)connect(&engine,&AgentEngine::reasoningDelta,this,action);
        else connect(&engine,&AgentEngine::textDelta,this,action);
        engine.submit("Initial");QTRY_VERIFY_WITH_TIMEOUT(acted,3000);
        if(replacement)QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),4000);
        QTest::qWait(40);
        QCOMPARE(done.size(),replacement?2:1);QCOMPARE(observed,replacement?QString("FIRSTREPLACEMENT"):QString("FIRST"));
        QVERIFY(!QFile::exists(workspace.filePath("stale.txt")));QCOMPARE(server.requests.size(),replacement?2:1);
        QCOMPARE(engine.lastTurnSucceeded(),replacement);
        if(replacement)QCOMPARE(engine.messages().last().toObject()["content"].toString(),QString("REPLACEMENT"));
    }
    void deletion_data(){observerRows(false);}
    void deletion(){
        QFETCH(QString,protocol);QFETCH(bool,streaming);QFETCH(bool,thought);
        QTemporaryDir workspace;FixtureServer server;
        server.respond=[&](int,const QJsonObject&){return observerResponse(protocol,streaming,thought);};
        SYSTEM_INFO info{};GetSystemInfo(&info);const SIZE_T bytes=((sizeof(AgentEngine)+info.dwPageSize-1)/info.dwPageSize)*info.dwPageSize;
        void* storage=VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);QVERIFY(storage);
        auto* engine=new(storage)AgentEngine;QPointer<AgentEngine> alive(engine);bool protectedStorage=false;
        auto cleanup=qScopeGuard([&]{if(alive)engine->~AgentEngine();VirtualFree(storage,0,MEM_RELEASE);});
        auto provider=server.provider();provider.protocol=protocol;engine->setProvider(provider);engine->setWorkspace(workspace.path());engine->setAlwaysAllow(true);
        auto destroy=[&]{if(!alive)return;engine->~AgentEngine();DWORD old=0;protectedStorage=VirtualProtect(storage,bytes,PAGE_NOACCESS,&old);};
        if(thought)connect(engine,&AgentEngine::reasoningDelta,this,destroy);else connect(engine,&AgentEngine::textDelta,this,destroy);
        engine->submit("Destroy on response");QTRY_VERIFY_WITH_TIMEOUT(!alive,3000);QVERIFY(protectedStorage);QTest::qWait(40);
        QVERIFY(!QFile::exists(workspace.filePath("stale.txt")));QCOMPARE(server.requests.size(),1);
    }
    void setupCallback_data(){
        QTest::addColumn<bool>("fallback");QTest::addColumn<int>("action");
        for(bool fallback:{false,true})for(int action=0;action<3;++action)
            QTest::newRow(qPrintable(QString(fallback?"fallback":"preparer")+QString::number(action)))<<fallback<<action;
    }
    void setupCallback(){
        QFETCH(bool,fallback);QFETCH(int,action);FixtureServer server;
        server.respond=[&](int n,const QJsonObject&){
            if(fallback&&n==1)return Response{"{}",503,false,"application/json"};
            return Response{textResponse("REPLACEMENT")};
        };
        SYSTEM_INFO info{};GetSystemInfo(&info);const SIZE_T bytes=((sizeof(AgentEngine)+info.dwPageSize-1)/info.dwPageSize)*info.dwPageSize;
        void* storage=VirtualAlloc(nullptr,bytes,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);QVERIFY(storage);
        auto* engine=new(storage)AgentEngine;QPointer<AgentEngine> alive(engine);bool acted=false,protectedStorage=false;
        auto cleanup=qScopeGuard([&]{if(alive)engine->~AgentEngine();VirtualFree(storage,0,MEM_RELEASE);});
        auto provider=server.provider();engine->setProvider(provider);QSignalSpy done(engine,&AgentEngine::finished);
        auto callback=[&]{
            if(acted)return;
            acted=true;
            if(action==2){engine->~AgentEngine();DWORD old=0;protectedStorage=VirtualProtect(storage,bytes,PAGE_NOACCESS,&old);return;}
            engine->cancel();
            if(action==1){engine->setMessagePreparer({});engine->setProvider(provider);engine->submit("Replacement");}
        };
        if(fallback){
            auto secondary=provider;secondary.id="secondary";secondary.model="secondary";
            engine->setModelChain({{provider},{secondary}},"fixture");
            connect(engine,&AgentEngine::effectiveProviderChanged,this,[&](const QString&,const QString&,int index){if(index==1)callback();});
        }else engine->setMessagePreparer([&,sentinel=QString("captured preparer stays alive")](const QJsonArray&messages,const ProviderConfig&config,QString*){
            const auto expectedConfig=config;callback();
            if(config.id!=expectedConfig.id||sentinel!="captured preparer stays alive")qFatal("Preparer arguments or capture were invalidated");
            return messages;
        });
        engine->submit("Initial");QTRY_VERIFY_WITH_TIMEOUT(acted,3000);
        if(action==2){QVERIFY(!alive);QVERIFY(protectedStorage);}
        else {QTRY_VERIFY_WITH_TIMEOUT(!engine->isBusy(),4000);QCOMPARE(done.size(),action==1?2:1);QCOMPARE(engine->lastTurnSucceeded(),action==1);}
        QTest::qWait(40);QCOMPARE(server.requests.size(),(fallback?1:0)+(action==1?1:0));
    }
};
QTEST_GUILESS_MAIN(StreamObserverTest)
#include "test_engine_observers.moc"
