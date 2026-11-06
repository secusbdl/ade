#include "core/AgentEngine.h"
#include "core/AgentPresentation.h"
#include "core/ProviderProtocol.h"
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
using namespace ade;
namespace {
QByteArray event(const QJsonObject& object){return "data: "+QJsonDocument(object).toJson(QJsonDocument::Compact)+"\n\n";}
QByteArray chat(const QJsonObject& delta,const QJsonValue& finish=QJsonValue::Null){return event({{"choices",QJsonArray{QJsonObject{{"delta",delta},{"finish_reason",finish}}}}});}
QJsonObject call(const QString& id="same-call",const QString& arguments="{\"x\":1}"){
    return {{"index",0},{"id",id},{"type","function"},{"function",QJsonObject{{"name","inspectFixture"},{"arguments",arguments}}}};
}
QJsonObject responseItem(const QString& type,const QString& text){
    if(type=="reasoning")return {{"id","reason-1"},{"type","reasoning"},{"encrypted_content","SIGNED_FIXTURE"},{"summary",QJsonArray{QJsonObject{{"type","summary_text"},{"text",text}}}}};
    return {{"id","message-1"},{"type","message"},{"role","assistant"},{"content",QJsonArray{QJsonObject{{"type","output_text"},{"text",text}}}}};
}
QJsonObject responseCall(){return {{"id","item-1"},{"type","function_call"},{"call_id","same-call"},{"name","inspectFixture"},{"arguments","{\"x\":1}"}};}
QList<QByteArray> round(const QString& protocol,bool tool=true){
    if(protocol=="openai"){
        if(!tool)return {chat({{"content","Final"}},"stop"),"data: [DONE]\n\n"};
        auto partial=call("same-call","{\"x\":");auto rest=call("","1}");rest.remove("id");auto fn=rest["function"].toObject();fn.remove("name");rest["function"]=fn;
        return {chat({{"reasoning_content","Think"}}),chat({{"content","Before"}}),chat({{"tool_calls",QJsonArray{partial}}}),chat({{"tool_calls",QJsonArray{rest}}},"tool_calls"),"data: [DONE]\n\n"};
    }
    if(protocol=="anthropic"){
        if(!tool)return {event({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","text"},{"text","Final"}}}}),event({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","end_turn"}}}}),event({{"type","message_stop"}})};
        return {event({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","thinking"},{"thinking","Think"},{"signature","SIGNED_FIXTURE"}}}}),
            event({{"type","content_block_stop"},{"index",0}}),
            event({{"type","content_block_start"},{"index",1},{"content_block",QJsonObject{{"type","text"},{"text","Before"}}}}),
            event({{"type","content_block_stop"},{"index",1}}),
            event({{"type","content_block_start"},{"index",2},{"content_block",QJsonObject{{"type","tool_use"},{"id","same-call"},{"name","inspectFixture"},{"input",QJsonObject{}}}}}),
            event({{"type","content_block_delta"},{"index",2},{"delta",QJsonObject{{"type","input_json_delta"},{"partial_json","{\"x\":"}}}}),
            event({{"type","content_block_delta"},{"index",2},{"delta",QJsonObject{{"type","input_json_delta"},{"partial_json","1}"}}}}),
            event({{"type","content_block_stop"},{"index",2}}),event({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","tool_use"}}}}),event({{"type","message_stop"}})};
    }
    if(protocol=="google"){
        auto chunk=[](const QJsonArray& parts,bool done=false){QJsonObject candidate{{"content",QJsonObject{{"role","model"},{"parts",parts}}}};if(done)candidate["finishReason"]="STOP";return event({{"candidates",QJsonArray{candidate}}});};
        if(!tool)return {chunk(QJsonArray{QJsonObject{{"text","Final"}}},true)};
        return {chunk(QJsonArray{QJsonObject{{"text","Think"},{"thought",true},{"thoughtSignature","SIGNED_FIXTURE"}}}),chunk(QJsonArray{QJsonObject{{"text","Before"}}}),chunk(QJsonArray{QJsonObject{{"functionCall",QJsonObject{{"id","same-call"},{"name","inspectFixture"},{"args",QJsonObject{{"x",1}}}}}}},true)};
    }
    if(!tool)return {event({{"type","response.output_text.delta"},{"output_index",0},{"content_index",0},{"delta","Final"}}),event({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",QJsonArray{responseItem("message","Final")}}}}})};
    auto started=responseCall();started["arguments"]="";
    return {event({{"type","response.reasoning_summary_text.delta"},{"output_index",0},{"summary_index",0},{"delta","Think"}}),
        event({{"type","response.reasoning_summary_text.done"},{"output_index",0},{"summary_index",0},{"text","Think"}}),
        event({{"type","response.output_text.delta"},{"output_index",1},{"content_index",0},{"delta","Before"}}),
        event({{"type","response.output_item.added"},{"output_index",2},{"item",started}}),
        event({{"type","response.function_call_arguments.delta"},{"output_index",2},{"delta","{\"x\":"}}),
        event({{"type","response.function_call_arguments.delta"},{"output_index",2},{"delta","1}"}}),
        event({{"type","response.output_item.done"},{"output_index",2},{"item",responseCall()}}),
        event({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",QJsonArray{responseItem("reasoning","Think"),responseItem("message","Before"),responseCall()}}}}})};
}
struct Reply{QList<QByteArray> chunks;bool hold=false;int status=200;};
class Server:public QObject{
public:
    QTcpServer server;QString protocol="openai";QList<Reply> replies;QList<QJsonObject> requests;
    Server(){server.listen(QHostAddress::LocalHost);connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
            if(socket->property("handled").toBool()){socket->readAll();return;}
            auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
            const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;int length=0;
            for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
            if(bytes.size()<end+4+length)return;
            socket->setProperty("handled",true);
            requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());auto reply=replies.isEmpty()?Reply{round(protocol,false)}:replies.takeFirst();
            int total=0;for(const auto& chunk:reply.chunks)total+=chunk.size();if(reply.hold)total+=1000000;
            socket->write("HTTP/1.1 "+QByteArray::number(reply.status)+" Test\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(total)+"\r\nConnection: close\r\n\r\n");
            const QPointer<QTcpSocket> safe(socket);int delay=0;
            for(const auto& chunk:reply.chunks){delay+=15;QTimer::singleShot(delay,socket,[safe,chunk]{if(safe&&safe->state()==QAbstractSocket::ConnectedState)safe->write(chunk);});}
            if(!reply.hold)QTimer::singleShot(delay+15,socket,[safe]{if(safe)safe->disconnectFromHost();});
        });
    }});}
    ProviderConfig provider()const{return {"fixture","Fixture",QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),"fake-token-only","fixture-model",protocol};}
};
QJsonArray tools(){return {QJsonObject{{"type","function"},{"function",QJsonObject{{"name","inspectFixture"},{"description","Fixture"},{"parameters",QJsonObject{{"type","object"},{"properties",QJsonObject{{"x",QJsonObject{{"type","integer"}}}}}}}}}}};}
QJsonArray assistantRows(const QJsonArray& rows){QJsonArray out;for(const auto& row:rows)if(row.toObject()["role"]=="assistant")out.append(row);return out;}
QJsonObject findTool(const QJsonArray& rows){for(const auto& row:rows)for(const auto& part:row.toObject()["parts"].toArray())if(part.toObject()["type"]=="tool")return part.toObject();return {};}
QStringList partTypes(const QJsonObject& row){QStringList out;for(const auto& part:row["parts"].toArray())out.append(part.toObject()["type"].toString());return out;}
void protocols(){QTest::addColumn<QString>("protocol");for(const auto& p:{"openai","anthropic","google","responses"})QTest::newRow(p)<<QString(p);}
QJsonObject textMessage(const QString& text){return {{"role","assistant"},{"content",text}};}
}
class PresentationTest:public QObject{
    Q_OBJECT
private slots:
    void multiRound_data(){protocols();}
    void multiRound(){
        QFETCH(QString,protocol);Server server;server.protocol=protocol;server.replies={{round(protocol)}};
        AgentEngine engine;engine.setProvider(server.provider());engine.setExternalTools(tools());
        QSignalSpy finished(&engine,&AgentEngine::finished);QSignalSpy errors(&engine,&AgentEngine::failed);QList<QJsonArray> snapshots;
        connect(&engine,&AgentEngine::displayChanged,&engine,[&]{const auto original=engine.messages();snapshots.append(engine.displayMessages());QCOMPARE(engine.messages(),original);});
        connect(&engine,&AgentEngine::approvalRequired,&engine,[&](const QString& id){QTimer::singleShot(10,&engine,[&,id]{engine.approveTool(id,true);});});
        connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString& id){QTimer::singleShot(15,&engine,[&,id]{engine.completeExternalToolWithAttachments(id,"OUTPUT",false,QJsonArray{QJsonObject{{"path","att/fake.txt"}}});});});
        engine.submit("Question");QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,4000);QCOMPARE(errors.size(),0);QCOMPARE(server.requests.size(),2);
        const auto rows=engine.displayMessages(), assistants=assistantRows(rows);QCOMPARE(rows.size(),3);QCOMPARE(assistants.size(),2);
        const auto first=assistants[0].toObject();QCOMPARE(partTypes(first),QStringList({"reasoning","text","tool"}));
        const auto parts=first["parts"].toArray();QCOMPARE(parts[0].toObject()["text"].toString(),"Think");QCOMPARE(parts[1].toObject()["text"].toString(),"Before");
        QVERIFY(parts[0].toObject().contains("durationMs"));QVERIFY(parts[0].toObject()["durationMs"].toDouble()>=0);
        QCOMPARE(parts[2].toObject()["state"].toString(),"output-available");QCOMPARE(parts[2].toObject()["output"].toString(),"OUTPUT");QCOMPARE(parts[2].toObject()["input"].toObject(),QJsonObject({{"x",1}}));
        QCOMPARE(first["attachments"].toArray().size(),1);QCOMPARE(assistants[1].toObject()["parts"].toArray()[0].toObject()["text"].toString(),"Final");
        QSet<QString> states;bool provisional=false;for(const auto& snapshot:snapshots){QSet<QString> ids;int provisionalCount=0;for(const auto& row:snapshot){const auto object=row.toObject();QVERIFY(!ids.contains(object["id"].toString()));ids.insert(object["id"].toString());if(object["provisional"].toBool())++provisionalCount;
                if(object["id"]==first["id"]&&object["provisional"].toBool()){provisional=true;for(int i=0;i<object["parts"].toArray().size();++i)QCOMPARE(object["parts"].toArray()[i].toObject()["id"],parts[i].toObject()["id"]);}}
            QVERIFY(provisionalCount<=1);const auto tool=findTool(snapshot);if(!tool.isEmpty())states.insert(tool["state"].toString());}
        QVERIFY(provisional);for(const auto& state:{"input-streaming","input-available","approval-requested","running","output-available"})QVERIFY2(states.contains(state),state);
        const auto retained=engine.messages();AgentEngine restarted;restarted.setMessages(retained);QCOMPARE(restarted.displayMessages(),rows);QCOMPARE(AgentEngine::displayHistory(retained),rows);
        auto undecorated=retained;for(int i=0;i<undecorated.size();++i){auto message=undecorated[i].toObject();auto metadata=message["metadata"].toObject();metadata.remove("display");if(metadata.isEmpty())message.remove("metadata");else message["metadata"]=metadata;undecorated[i]=message;}
        QCOMPARE(ProviderProtocol::prepare(server.provider(),retained,"same",tools()).body,ProviderProtocol::prepare(server.provider(),undecorated,"same",tools()).body);
        const auto legacy=assistantRows(AgentEngine::displayHistory(undecorated));QCOMPARE(partTypes(legacy[0].toObject()),QStringList({"reasoning","text","tool"}));
        QCOMPARE(engine.messages(),retained);
        const auto wire=QJsonDocument(server.requests[1]).toJson(QJsonDocument::Compact);QVERIFY(!wire.contains("durationMs"));QVERIFY(!wire.contains("approval-requested"));QVERIFY(!wire.contains("metadata"));
        if(protocol!="openai")QVERIFY(wire.contains("SIGNED_FIXTURE"));
    }
    void states_data(){QTest::addColumn<QString>("terminal");for(const auto& state:{"output-denied","output-error","canceled"})QTest::newRow(state)<<QString(state);}
    void states(){
        QFETCH(QString,terminal);Server server;server.replies={{round("openai")}};AgentEngine engine;engine.setProvider(server.provider());engine.setExternalTools(tools());QSignalSpy done(&engine,&AgentEngine::finished);
        connect(&engine,&AgentEngine::approvalRequired,&engine,[&](const QString& id){engine.approveTool(id,terminal!="output-denied");});
        connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString& id){if(terminal=="canceled")engine.cancel();else engine.completeExternalTool(id,"BAD",true);});
        engine.submit("Q");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,4000);QCOMPARE(findTool(engine.displayMessages())["state"].toString(),terminal);
    }
    void displayObserverCanDecideApproval_data(){QTest::addColumn<bool>("allow");QTest::newRow("allow")<<true;QTest::newRow("deny")<<false;}
    void displayObserverCanDecideApproval(){
        QFETCH(bool,allow);Server server;server.replies={{round("openai")}};AgentEngine engine;engine.setProvider(server.provider());engine.setExternalTools(tools());QSignalSpy done(&engine,&AgentEngine::finished),approvals(&engine,&AgentEngine::approvalRequired);bool decided=false;
        connect(&engine,&AgentEngine::displayChanged,&engine,[&]{const auto tool=findTool(engine.displayMessages());if(!decided&&tool["state"]=="approval-requested"){decided=true;engine.approveTool(tool["toolCallId"].toString(),allow);}});
        connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString& id){engine.completeExternalTool(id,"DONE",false);});
        engine.submit("Q");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,4000);QVERIFY(decided);QCOMPARE(approvals.size(),0);QCOMPARE(findTool(engine.displayMessages())["state"].toString(),allow?"output-available":"output-denied");
    }
    void reasoningOnly_data(){QTest::addColumn<QString>("protocol");QTest::addColumn<bool>("failure");for(const auto& p:{"openai","anthropic","google","responses"})for(bool fail:{false,true})QTest::newRow(qPrintable(QString(p)+(fail?"-fail":"-cancel")))<<QString(p)<<fail;}
    void reasoningOnly(){
        QFETCH(QString,protocol);QFETCH(bool,failure);Server server;server.protocol=protocol;auto chunks=round(protocol);server.replies={{QList<QByteArray>{chunks.first()},!failure}};
        AgentEngine engine;engine.setProvider(server.provider());QSignalSpy done(&engine,&AgentEngine::finished);
        if(!failure)connect(&engine,&AgentEngine::displayChanged,&engine,[&]{const auto rows=assistantRows(engine.displayMessages());if(engine.isBusy()&&!rows.isEmpty()&&!rows.last().toObject()["parts"].toArray().isEmpty())engine.cancel();});
        engine.submit("Q");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,4000);QCOMPARE(engine.messages().size(),2);const auto message=engine.messages().last().toObject();QVERIFY(AgentPresentation::isUiOnly(message));QVERIFY(!message.contains("ade_native"));
        const auto rows=engine.displayMessages();const auto parts=rows.last().toObject()["parts"].toArray();QCOMPARE(parts[0].toObject()["type"].toString(),"reasoning");QCOMPARE(parts[0].toObject()["text"].toString(),"Think");QCOMPARE(parts[0].toObject()["state"].toString(),failure?"error":"canceled");
        QCOMPARE(AgentEngine::displayHistory(engine.messages()),rows);const auto wire=ProviderProtocol::prepare(server.provider(),engine.messages(),"",{}).body;QVERIFY(!QJsonDocument(wire).toJson().contains("Think"));
    }
    void wireUnchanged_data(){protocols();}
    void wireUnchanged(){
        QFETCH(QString,protocol);Server server;server.protocol=protocol;QJsonArray messages{QJsonObject{{"role","user"},{"content","Q"}},textMessage("REAL")};auto decorated=messages;
        AgentPresentation display;display.begin();display.text("text","REAL");display.finish();auto message=messages.last().toObject();message["metadata"]=QJsonObject{{"attachments",QJsonArray{QJsonObject{{"path","att/blob"}}}},{"fileReads",QJsonArray{QJsonObject{{"snapshotId","fake"}}}}};
        decorated[1]=AgentPresentation::withMetadata(message,display.metadata());messages[1]=message;
        QCOMPARE(ProviderProtocol::prepare(server.provider(),decorated,"system",tools()).body,ProviderProtocol::prepare(server.provider(),messages,"system",tools()).body);
        display.begin();display.text("reasoning","PRIVATE_PARTIAL");display.finish("canceled");auto ui=AgentPresentation::withMetadata(textMessage(""),display.metadata(true));QVERIFY(AgentPresentation::isUiOnly(ui));
        auto added=messages;added.append(ui);QCOMPARE(ProviderProtocol::prepare(server.provider(),added,"system",{}).body,ProviderProtocol::prepare(server.provider(),messages,"system",{}).body);
        for(const auto& key:{"content","tool_calls","ade_native","reasoning_content"}){auto malformed=ui;if(QString(key)=="content")malformed[key]="REAL";else malformed[key]=QJsonArray{};QVERIFY(!AgentPresentation::isUiOnly(malformed));}
        auto actualContent=ui;actualContent["content"]="CANNOT_HIDE_REAL_CONTENT";QVERIFY(QJsonDocument(ProviderProtocol::prepare(server.provider(),QJsonArray{actualContent},"",{}).body).toJson().contains("CANNOT_HIDE_REAL_CONTENT"));
        auto malformed=ui;auto meta=malformed["metadata"].toObject();meta["attachments"]=QJsonArray{QJsonObject{{"path","att/a"}}};malformed["metadata"]=meta;QVERIFY(!AgentPresentation::isUiOnly(malformed));
    }
    void reusedIdsAndLegacy(){
        const QJsonObject assistant{{"role","assistant"},{"content",""},{"tool_calls",QJsonArray{call()}}};const QJsonObject result{{"role","tool"},{"tool_call_id","same-call"},{"content","ONE"}};
        auto second=result;second["content"]="TWO";const QJsonArray history{assistant,result,assistant,second};const auto rows=AgentEngine::displayHistory(history);QCOMPARE(rows.size(),2);
        QVERIFY(rows[0].toObject()["id"]!=rows[1].toObject()["id"]);QCOMPARE(findTool(QJsonArray{rows[0]})["output"].toString(),"ONE");QCOMPARE(findTool(QJsonArray{rows[1]})["output"].toString(),"TWO");QCOMPARE(AgentEngine::displayHistory(history),rows);
        const auto thought=AgentEngine::displayHistory(QJsonArray{QJsonObject{{"role","assistant"},{"content","Text"},{"reasoning_content","Thought"}}});QCOMPARE(partTypes(thought[0].toObject()),QStringList({"reasoning","text"}));QVERIFY(!thought[0].toObject()["parts"].toArray()[0].toObject().contains("durationMs"));
    }
    void malformedDisplayCannotHideCanonicalContent(){
        AgentPresentation display;display.begin();display.text("text","FORGED");display.finish();const auto message=AgentPresentation::withMetadata(textMessage("REAL"),display.metadata());
        const auto rows=AgentEngine::displayHistory(QJsonArray{message});QCOMPARE(rows[0].toObject()["parts"].toArray()[0].toObject()["text"].toString(),"REAL");
        auto valid=AgentPresentation::withMetadata(textMessage("FORGED"),display.metadata());const auto repeated=AgentEngine::displayHistory(QJsonArray{valid,valid});QVERIFY(repeated[0].toObject()["id"]!=repeated[1].toObject()["id"]);
        auto invalid=display.metadata();auto parts=invalid["parts"].toArray();auto part=parts[0].toObject();part["state"]="unknown";parts[0]=part;invalid["parts"]=parts;
        const auto fallback=AgentEngine::displayHistory(QJsonArray{AgentPresentation::withMetadata(textMessage("FORGED"),invalid)});QCOMPARE(fallback[0].toObject()["parts"].toArray()[0].toObject()["state"].toString(),"done");
        const QJsonObject metadata{{"attachments",QJsonArray{QJsonObject{{"path","att/a"}}}},{"fileReads",QJsonArray{QJsonObject{{"snapshotId","snapshot"}}}},{"pathReferences",QJsonObject{{"att/a",QJsonObject{{"hash","fixture"}}}}}};
        auto withContext=textMessage("FORGED");withContext["metadata"]=metadata;const auto preserved=AgentPresentation::withMetadata(withContext,display.metadata())["metadata"].toObject();for(auto it=metadata.begin();it!=metadata.end();++it)QCOMPARE(preserved[it.key()],it.value());
    }
    void uiOnlyCannotHideOtherReplayFields_data(){QTest::addColumn<QString>("field");for(const auto& field:{"function_call","refusal","audio"})QTest::newRow(field)<<QString(field);}
    void uiOnlyCannotHideOtherReplayFields(){
        QFETCH(QString,field);Server server;AgentPresentation display;display.begin();display.text("reasoning","partial");display.finish("canceled");auto message=AgentPresentation::withMetadata(textMessage(""),display.metadata(true));
        if(field=="function_call")message[field]=QJsonObject{{"name","legacy"},{"arguments","{\"marker\":\"REPLAY_PAYLOAD\"}"}};
        else if(field=="audio")message[field]=QJsonObject{{"id","REPLAY_PAYLOAD"}};
        else message[field]="REPLAY_PAYLOAD";
        const auto wire=ProviderProtocol::prepare(server.provider(),QJsonArray{message},"",{}).body;QVERIFY(QJsonDocument(wire).toJson().contains("REPLAY_PAYLOAD"));QVERIFY(!AgentPresentation::isUiOnly(message));
    }
    void savedIdsCannotCollideWithLegacyIds(){
        const auto legacy=textMessage("LEGACY_VISIBLE");const auto id=AgentEngine::displayHistory(QJsonArray{legacy})[0].toObject()["id"].toString();AgentPresentation display;display.begin();display.text("text","SAVED_VISIBLE");display.finish();auto metadata=display.metadata();metadata["id"]=id;
        const auto saved=AgentPresentation::withMetadata(textMessage("SAVED_VISIBLE"),metadata);const auto rows=AgentEngine::displayHistory(QJsonArray{saved,legacy});QCOMPARE(rows.size(),2);QVERIFY(rows[0].toObject()["id"]!=rows[1].toObject()["id"]);QCOMPARE(rows[1].toObject()["parts"].toArray()[0].toObject()["text"].toString(),"LEGACY_VISIBLE");
    }
    void emptyPartFloodIsBounded(){
        Server server;server.protocol="responses";QByteArray body;for(int i=0;i<=20000;++i)body+=::event({{"type","response.content_part.added"},{"output_index",i/20000},{"content_index",i%20000},{"part",QJsonObject{{"type","output_text"},{"text",""}}}});
        server.replies={{QList<QByteArray>{body}}};AgentEngine engine;engine.setProvider(server.provider());QSignalSpy errors(&engine,&AgentEngine::failed);engine.submit("Q");QTRY_COMPARE_WITH_TIMEOUT(errors.size(),1,10000);QVERIFY(errors[0][0].toString().contains("presentation"));
        const auto rows=assistantRows(engine.displayMessages());QVERIFY(!rows.isEmpty());QVERIFY(rows.last().toObject()["parts"].toArray().size()<=20000);
    }
    void allPartKindsShareOneRuntimeBound(){
        AgentPresentation display;display.begin();for(int i=0;i<AgentPresentation::MaximumParts;++i){if(i%3==0)display.text("text","x",QString::number(i));else if(i%3==1)display.tool(i,QString::number(i),"fixture","{}");else display.error("fixture");}
        QVERIFY(!display.limitExceeded());display.text("reasoning","overflow","extra");display.tool(30000,"extra","fixture","{}");display.error("overflow");QVERIFY(display.limitExceeded());QCOMPARE(display.row(false)["parts"].toArray().size(),AgentPresentation::MaximumParts);
    }
    void durationIsMeasuredAndFreezes(){
        AgentPresentation display;display.begin();display.text("reasoning","Thought");QTest::qWait(25);const auto live=display.row(true)["parts"].toArray()[0].toObject()["durationMs"].toDouble();QVERIFY(live>=20);
        display.finish();const auto frozen=display.metadata();QTest::qWait(25);QCOMPARE(display.metadata(),frozen);QVERIFY(frozen["parts"].toArray()[0].toObject()["durationMs"].toDouble()>=live);
    }
    void emptyNativePartStarts_data(){QTest::addColumn<QString>("protocol");QTest::newRow("anthropic")<<QString("anthropic");QTest::newRow("responses")<<QString("responses");}
    void emptyNativePartStarts(){
        QFETCH(QString,protocol);Server server;server.protocol=protocol;auto chunks=round(protocol);if(protocol=="anthropic"){
            chunks.removeFirst();chunks.prepend(::event({{"type","content_block_delta"},{"index",0},{"delta",QJsonObject{{"type","thinking_delta"},{"thinking","Think"}}}}));
            chunks.prepend(::event({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","thinking"},{"thinking",""},{"signature","SIGNED_FIXTURE"}}}}));
        }else chunks.prepend(::event({{"type","response.reasoning_summary_part.added"},{"output_index",0},{"summary_index",0},{"part",QJsonObject{{"type","summary_text"},{"text",""}}}}));
        server.replies={{chunks}};AgentEngine engine;engine.setProvider(server.provider());engine.setExternalTools(tools());engine.setAlwaysAllow(true);QSignalSpy done(&engine,&AgentEngine::finished);QString partId;bool emptyStart=false;
        connect(&engine,&AgentEngine::displayChanged,&engine,[&]{const auto rows=assistantRows(engine.displayMessages());if(rows.isEmpty())return;const auto parts=rows[0].toObject()["parts"].toArray();if(parts.isEmpty()||parts[0].toObject()["type"]!="reasoning")return;const auto part=parts[0].toObject();if(partId.isEmpty())partId=part["id"].toString();QCOMPARE(part["id"].toString(),partId);if(part["text"]==""&&part["state"]=="streaming")emptyStart=true;});
        connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString& id){engine.completeExternalTool(id,"OK",false);});engine.submit("Q");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,4000);QVERIFY(emptyStart);QCOMPARE(assistantRows(engine.displayMessages())[0].toObject()["parts"].toArray()[0].toObject()["id"].toString(),partId);
    }
    void observedToolOrderSurvivesRetention(){
        AgentPresentation display;display.begin();display.tool(7,"second","inspectFixture","{}");display.tool(2,"first","inspectFixture","{}");display.finish();auto first=call("first","{}"),second=call("second","{}");
        const QJsonObject message{{"role","assistant"},{"content",""},{"tool_calls",QJsonArray{first,second}}};const auto saved=AgentPresentation::withMetadata(message,display.metadata());const auto rows=AgentEngine::displayHistory(QJsonArray{saved});
        QCOMPARE(rows[0].toObject()["id"],display.metadata()["id"]);QCOMPARE(rows[0].toObject()["parts"].toArray()[0].toObject()["toolCallId"].toString(),"second");
    }
    void runningOutputIsBoundedAndReplaced_data(){QTest::addColumn<bool>("unicode");QTest::newRow("ascii")<<false;QTest::newRow("unicode")<<true;}
    void runningOutputIsBoundedAndReplaced(){
        QFETCH(bool,unicode);
        Server server;server.replies={{round("openai")}};AgentEngine engine;engine.setProvider(server.provider());engine.setExternalTools(tools());engine.setAlwaysAllow(true);QSignalSpy done(&engine,&AgentEngine::finished);QString token;
        connect(&engine,&AgentEngine::externalToolRequested,&engine,[&](const QString& id){token=id;});engine.submit("Q");QTRY_VERIFY_WITH_TIMEOUT(!token.isEmpty(),3000);
        const auto beforeWire=ProviderProtocol::prepare(server.provider(),engine.messages(),"same",tools()).body;
        QVERIFY(!engine.appendExternalToolOutput("wrong-token","REJECT"));QVERIFY(engine.appendExternalToolOutput(token,"prefix fake-"));QVERIFY(engine.appendExternalToolOutput(token,"token-only suffix"));
        auto tool=findTool(engine.displayMessages());QCOMPARE(tool["state"].toString(),"running");QCOMPARE(tool["output"].toString(),"prefix [REDACTED] suffix");
        QCOMPARE(ProviderProtocol::prepare(server.provider(),engine.messages(),"same",tools()).body,beforeWire);
        QVERIFY(engine.appendExternalToolOutput(token,QString(100000,unicode?QChar(0x03bb):QChar('x'))));tool=findTool(engine.displayMessages());QVERIFY(tool["output"].toString().toUtf8().size()<=65536);QVERIFY(!tool["output"].toString().contains(QChar::ReplacementCharacter));QVERIFY(tool["output"].toString().endsWith("[Output truncated at 64 KiB.]"));QVERIFY(!engine.appendExternalToolOutput(token,"MORE"));
        engine.completeExternalTool(token,"FINAL_RESULT",false);QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,4000);QCOMPARE(findTool(engine.displayMessages())["output"].toString(),"FINAL_RESULT");QVERIFY(!engine.appendExternalToolOutput(token,"STALE"));
    }
    void outputObserver_data(){QTest::addColumn<QString>("action");for(const auto& action:{"cancel","replace","destroy"})QTest::newRow(action)<<QString(action);}
    void outputObserver(){
        QFETCH(QString,action);Server server;server.replies={{round("openai")}};auto* engine=new AgentEngine;QPointer<AgentEngine> safe(engine);engine->setProvider(server.provider());engine->setExternalTools(tools());engine->setAlwaysAllow(true);QString token;
        connect(engine,&AgentEngine::externalToolRequested,this,[&](const QString& id){token=id;});engine->submit("Q");QTRY_VERIFY_WITH_TIMEOUT(!token.isEmpty(),3000);bool acted=false;
        connect(engine,&AgentEngine::displayChanged,this,[&]{if(acted||!safe||findTool(safe->displayMessages())["output"]!="LIVE")return;acted=true;if(action=="destroy"){delete safe;return;}safe->cancel();if(action=="replace")safe->submit("Next");});
        QVERIFY(engine->appendExternalToolOutput(token,"LIVE"));QVERIFY(acted);if(safe){QVERIFY(!safe->appendExternalToolOutput(token,"STALE"));QTRY_VERIFY_WITH_TIMEOUT(!safe->isBusy(),4000);delete safe;}
    }
    void observer_data(){QTest::addColumn<QString>("action");QTest::addColumn<QString>("phase");QTest::addColumn<QString>("protocol");for(const auto& action:{"cancel","replace","destroy"})for(const auto& phase:{"accepted","round","stream","running"})for(const auto& protocol:{"openai","anthropic","google","responses"})QTest::newRow(qPrintable(QString(action)+"-"+phase+"-"+protocol))<<QString(action)<<QString(phase)<<QString(protocol);}
    void observer(){
        QFETCH(QString,action);QFETCH(QString,phase);QFETCH(QString,protocol);Server server;server.protocol=protocol;server.replies={{round(protocol)}};auto* engine=new AgentEngine;QPointer<AgentEngine> safe(engine);engine->setProvider(server.provider());engine->setExternalTools(tools());engine->setAlwaysAllow(true);bool acted=false;int toolRequests=0;
        connect(engine,&AgentEngine::externalToolRequested,this,[&]{++toolRequests;});
        connect(engine,&AgentEngine::displayChanged,this,[&]{if(acted||!safe||!safe->isBusy())return;const auto rows=assistantRows(safe->displayMessages());bool ready=phase=="accepted"&&rows.isEmpty();
            if(!rows.isEmpty()){const auto parts=rows.last().toObject()["parts"].toArray();ready|=phase=="round"&&parts.isEmpty();ready|=phase=="stream"&&!parts.isEmpty();ready|=phase=="running"&&findTool(safe->displayMessages())["state"]=="running";}
            if(!ready)return;
            acted=true;if(action=="destroy"){delete safe;return;}safe->cancel();if(action=="replace"){server.replies.clear();auto p=server.provider();p.id="replacement";safe->setProvider(p);safe->submit("Replacement");}});
        engine->submit("Q");QTRY_VERIFY_WITH_TIMEOUT(acted,4000);if(safe)QTRY_VERIFY_WITH_TIMEOUT(!safe->isBusy(),4000);QTest::qWait(80);QCOMPARE(toolRequests,0);if(safe){if(action=="replace")QCOMPARE(safe->messages().last().toObject()["content"].toString(),"Final");delete safe;}
    }
    void acceptedCancelDoesNotRepeatPreviousRound(){
        Server server;AgentEngine engine;engine.setProvider(server.provider());QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("First");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,2000);const auto prior=engine.messages();
        bool canceled=false;connect(&engine,&AgentEngine::displayChanged,&engine,[&]{if(!canceled&&engine.isBusy()){canceled=true;engine.cancel();}});QVERIFY(engine.submitWithAttachments("Second",{}));QCOMPARE(engine.messages().size(),prior.size()+1);QCOMPARE(engine.messages()[1],prior[1]);QCOMPARE(server.requests.size(),1);
    }
};
QTEST_GUILESS_MAIN(PresentationTest)
#include "test_presentation.moc"
