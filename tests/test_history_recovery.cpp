#include "core/AgentEngine.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray sseEvent(const QJsonObject& object){return "data: "+QJsonDocument(object).toJson(QJsonDocument::Compact)+"\n\n";}
QJsonObject call(const QString& id,const QString& name="write"){
    return {{"id",id},{"type","function"},{"function",QJsonObject{{"name",name},{"arguments","{\"path\":\"must-not-exist.txt\",\"content\":\"DO_NOT_EXECUTE\"}"}}}};
}
QJsonObject assistant(const QJsonArray& calls){return {{"role","assistant"},{"content","Inherited plan"},{"tool_calls",calls}};}
QJsonObject result(const QString& id,const QString& content=""){
    return {{"role","tool"},{"tool_call_id",id},{"content",content}};
}
QByteArray response(const QString& protocol){
    if(protocol=="responses")return sseEvent({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",QJsonArray{QJsonObject{{"type","message"},{"role","assistant"},{"content",QJsonArray{QJsonObject{{"type","output_text"},{"text","DONE"}}}}}}}}}});
    if(protocol=="google")return sseEvent({{"candidates",QJsonArray{QJsonObject{{"content",QJsonObject{{"role","model"},{"parts",QJsonArray{QJsonObject{{"text","DONE"}}}}}},{"finishReason","STOP"}}}}});
    if(protocol=="anthropic")return sseEvent({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","text"},{"text","DONE"}}}})+sseEvent({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","end_turn"}}}})+sseEvent({{"type","message_stop"}});
    return sseEvent({{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"content","DONE"}}},{"finish_reason","stop"}}}}})+"data: [DONE]\n\n";
}
class Server:public QObject {
public:
    QTcpServer server;QString protocol="openai";QList<QJsonObject> requests;QList<QByteArray> replies;
    Server(){
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
                const auto end=bytes.indexOf("\r\n\r\n");if(end<0)return;int length=0;
                for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<end+4+length)return;
                socket->setProperty("handled",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());
                const auto body=replies.isEmpty()?response(protocol):replies.takeFirst();
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();
            });
        }});
    }
    ProviderConfig provider()const{return {"fixture","Fixture",QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),"fixture-only","fixture-model",protocol};}
};
QJsonArray results(const QJsonArray& history){QJsonArray out;for(const auto& value:history)if(value.toObject()["role"]=="tool")out.append(value);return out;}
QJsonObject nativeAssistant(const QString& protocol,bool googleIds=true){
    const QJsonArray calls{call("complete","read"),call("pending-A"),call("pending-B")};auto message=assistant(calls);
    QJsonArray content;
    if(protocol=="anthropic")content.append(QJsonObject{{"type","thinking"},{"thinking","preserved thought"},{"signature","SIGNED_NATIVE_FIXTURE"}});
    if(protocol=="google")content.append(QJsonObject{{"text","preserved thought"},{"thought",true},{"thoughtSignature","SIGNED_NATIVE_FIXTURE"}});
    if(protocol=="responses")content.append(QJsonObject{{"type","reasoning"},{"id","reasoning-1"},{"encrypted_content","SIGNED_NATIVE_FIXTURE"},{"summary",QJsonArray{}}});
    for(const auto& value:calls){const auto c=value.toObject(),fn=c["function"].toObject();const auto args=QJsonDocument::fromJson(fn["arguments"].toString().toUtf8()).object();
        if(protocol=="anthropic")content.append(QJsonObject{{"type","tool_use"},{"id",c["id"]},{"name",fn["name"]},{"input",args}});
        if(protocol=="google"){QJsonObject native{{"name",fn["name"]},{"args",args}};if(googleIds)native["id"]=c["id"];content.append(QJsonObject{{"functionCall",native}});}
        if(protocol=="responses")content.append(QJsonObject{{"type","function_call"},{"id","item-"+c["id"].toString()},{"call_id",c["id"]},{"name",fn["name"]},{"arguments",fn["arguments"]}});
    }
    if(!content.isEmpty())message["ade_native"]=QJsonObject{{"protocol",protocol},{"providerId","fixture"},{"model","fixture-model"},{"content",content}};
    else {message["reasoning_content"]="preserved thought";message["ade_reasoning_source"]=QJsonObject{{"providerId","fixture"},{"model","fixture-model"}};}
    return message;
}
}
class HistoryRecoveryTest:public QObject {
    Q_OBJECT
private slots:
    void repairsInheritedCallsWithoutExecution_data(){
        QTest::addColumn<QString>("protocol");QTest::addColumn<bool>("googleIds");
        for(const auto& name:QStringList{"openai","anthropic","google","responses"})QTest::newRow(qPrintable(name))<<name<<true;
        QTest::newRow("google-without-native-ids")<<QString("google")<<false;
    }
    void repairsInheritedCallsWithoutExecution(){
        QFETCH(QString,protocol);QFETCH(bool,googleIds);Server server;server.protocol=protocol;QTemporaryDir workspace;
        auto complete=result("complete");complete["metadata"]=QJsonObject{{"fileReads",QJsonArray{QJsonObject{{"sentinel","completed-metadata"}}}},{"attachments",QJsonArray{QJsonObject{{"path","att/original.txt"}}}}};
        const QJsonArray input{QJsonObject{{"role","user"},{"content","Original"}},nativeAssistant(protocol,googleIds),complete};const auto originalBytes=QJsonDocument(input).toJson(QJsonDocument::Compact);
        AgentEngine source,target;source.setMessages(input);target.setMessages(QJsonDocument::fromJson(originalBytes).array());target.setProvider(server.provider());target.setWorkspace(workspace.path());target.setAlwaysAllow(true);
        QJsonArray prepared;target.setMessagePreparer([&](const QJsonArray& history,const ProviderConfig&,QString*){prepared=history;return history;});
        QSignalSpy done(&target,&AgentEngine::finished),failed(&target,&AgentEngine::failed),started(&target,&AgentEngine::toolStarted),approval(&target,&AgentEngine::approvalRequired),external(&target,&AgentEngine::externalToolRequested);
        target.submit("Continue safely");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(failed.isEmpty());QVERIFY(target.lastTurnSucceeded());QCOMPARE(server.requests.size(),1);QVERIFY(started.isEmpty()&&approval.isEmpty()&&external.isEmpty());QVERIFY(!QFileInfo::exists(workspace.filePath("must-not-exist.txt")));
        QCOMPARE(source.messages(),input);QCOMPARE(QJsonDocument(input).toJson(QJsonDocument::Compact),originalBytes);QCOMPARE(prepared.size(),6);QCOMPARE(prepared[1],input[1]);QCOMPARE(prepared[2],input[2]);
        for(int i=0;i<2;++i){const auto item=prepared[3+i].toObject();QCOMPARE(item["role"].toString(),"tool");QCOMPARE(item["tool_call_id"].toString(),i?QString("pending-B"):QString("pending-A"));QVERIFY(item["content"].toString().startsWith("Error: Tool execution interrupted"));QCOMPARE(item.size(),3);}
        QCOMPARE(prepared[5].toObject()["role"].toString(),"user");const auto body=server.requests.first();
        if(protocol=="openai"){
            const auto messages=body["messages"].toArray();QCOMPARE(messages[2].toObject()["tool_calls"],input[1].toObject()["tool_calls"]);QCOMPARE(messages[2].toObject()["reasoning_content"].toString(),"preserved thought");QCOMPARE(messages[4].toObject()["tool_call_id"].toString(),"pending-A");QCOMPARE(messages[5].toObject()["tool_call_id"].toString(),"pending-B");
        }else if(protocol=="responses"){
            const auto values=body["input"].toArray();QCOMPARE(values[1],input[1].toObject()["ade_native"].toObject()["content"].toArray()[0]);QCOMPARE(values[5].toObject()["call_id"].toString(),"complete");QCOMPARE(values[6].toObject()["call_id"].toString(),"pending-A");QCOMPARE(values[7].toObject()["call_id"].toString(),"pending-B");QCOMPARE(values[8].toObject()["role"].toString(),"user");
        }else if(protocol=="anthropic"){
            const auto messages=body["messages"].toArray();QCOMPARE(messages[1].toObject()["content"],input[1].toObject()["ade_native"].toObject()["content"]);const auto blocks=messages[2].toObject()["content"].toArray();QCOMPARE(blocks.size(),4);QVERIFY(!blocks[0].toObject()["is_error"].toBool());QVERIFY(blocks[1].toObject()["is_error"].toBool());QVERIFY(blocks[2].toObject()["is_error"].toBool());QCOMPARE(blocks[1].toObject()["tool_use_id"].toString(),"pending-A");
        }else{
            const auto messages=body["contents"].toArray();QCOMPARE(messages[1].toObject()["parts"],input[1].toObject()["ade_native"].toObject()["content"]);const auto parts=messages[2].toObject()["parts"].toArray();QCOMPARE(parts.size(),4);for(int i=0;i<3;++i){const auto output=parts[i].toObject()["functionResponse"].toObject();QCOMPARE(output.contains("id"),googleIds);QCOMPARE(output["name"].toString(),i?QString("write"):QString("read"));}
        }
        target.submit("Continue again");QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,5000);QCOMPARE(results(target.messages()).size(),3);
    }
    void repairsOlderBatchesAndAllowsReusedIds(){
        Server server;AgentEngine engine;engine.setProvider(server.provider());const QJsonArray input{assistant({call("same")}),QJsonObject{{"role","user"},{"content","Persisted later turn"}},assistant({call("same")}),result("same"),assistant({call("last")})};engine.setMessages(input);
        QSignalSpy done(&engine,&AgentEngine::finished);engine.submit("Continue");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(engine.lastTurnSucceeded());const auto history=engine.messages();QCOMPARE(history[0],input[0]);QCOMPARE(history[1].toObject()["tool_call_id"].toString(),"same");QCOMPARE(history[2],input[1]);QCOMPARE(history[4],input[3]);QCOMPARE(history[6].toObject()["tool_call_id"].toString(),"last");QCOMPARE(results(history).size(),3);
    }
    void malformedHistoryRejectsAtomically_data(){
        QTest::addColumn<QJsonArray>("history");
        QTest::newRow("duplicate-call-id")<<QJsonArray{assistant({call("same"),call("same")})};
        QTest::newRow("duplicate-result-id")<<QJsonArray{assistant({call("same")}),result("same"),result("same")};
        QTest::newRow("orphan-result")<<QJsonArray{result("orphan")};
        QTest::newRow("wrong-case-result")<<QJsonArray{assistant({call("Case")}),result("case")};
        QTest::newRow("result-after-user-boundary")<<QJsonArray{assistant({call("same")}),QJsonObject{{"role","user"},{"content","later"}},result("same")};
        auto missing=call("id");missing.remove("id");QTest::newRow("missing-id")<<QJsonArray{assistant({missing})};
        auto numeric=call("id");numeric["id"]=7;QTest::newRow("numeric-id")<<QJsonArray{assistant({numeric})};
        QTest::newRow("non-array-calls")<<QJsonArray{QJsonObject{{"role","assistant"},{"content",""},{"tool_calls","bad"}}};
        QTest::newRow("non-assistant-calls")<<QJsonArray{QJsonObject{{"role","user"},{"content",""},{"tool_calls",QJsonArray{call("id")}}}};
        auto badArgs=call("id");auto fn=badArgs["function"].toObject();fn["arguments"]=QJsonObject{};badArgs["function"]=fn;QTest::newRow("non-string-arguments")<<QJsonArray{assistant({badArgs})};
        for(const auto& protocol:QStringList{"anthropic","google","responses"}){
            auto message=nativeAssistant(protocol);auto meta=message["ade_native"].toObject();auto blocks=meta["content"].toArray();auto block=blocks[1].toObject();
            if(protocol=="google"){auto native=block["functionCall"].toObject();native["id"]="different";block["functionCall"]=native;}else block[protocol=="responses"?"call_id":"id"]="different";
            blocks[1]=block;meta["content"]=blocks;message["ade_native"]=meta;QTest::newRow(qPrintable(protocol+"-native-mismatch"))<<QJsonArray{message};
            message=nativeAssistant(protocol);meta=message["ade_native"].toObject();blocks=meta["content"].toArray();blocks.removeLast();meta["content"]=blocks;message["ade_native"]=meta;QTest::newRow(qPrintable(protocol+"-native-missing"))<<QJsonArray{message};
        }
    }
    void malformedHistoryRejectsAtomically(){
        QFETCH(QJsonArray,history);Server server;AgentEngine engine;engine.setProvider(server.provider());engine.setMessages(history);QSignalSpy done(&engine,&AgentEngine::finished),failed(&engine,&AgentEngine::failed),busy(&engine,&AgentEngine::busyChanged);
        QVERIFY(!engine.submitWithAttachments("Do not accept ambiguous history",{}));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QCOMPARE(failed.size(),1);QVERIFY(failed[0][0].toString().contains("history",Qt::CaseInsensitive));QVERIFY(!engine.isBusy());QVERIFY(busy.isEmpty());QCOMPARE(engine.messages(),history);QVERIFY(server.requests.isEmpty());
    }
    void rejectedConfigurationDoesNotPublishRecovery(){
        AgentEngine engine;const QJsonArray history{assistant({call("pending")})};engine.setMessages(history);QSignalSpy failed(&engine,&AgentEngine::failed),done(&engine,&AgentEngine::finished);QVERIFY(!engine.submitWithAttachments("Continue",{}));QCOMPARE(failed.size(),1);QCOMPARE(done.size(),1);QCOMPARE(engine.messages(),history);
    }
    void partialArgumentsAreInterruptedWithoutRewritingOriginalCall(){
        Server server;AgentEngine engine;engine.setProvider(server.provider());auto partial=call("partial");auto function=partial["function"].toObject();function["arguments"]="{\"path\":";partial["function"]=function;
        const QJsonArray input{assistant({partial})};engine.setMessages(input);QSignalSpy done(&engine,&AgentEngine::finished),started(&engine,&AgentEngine::toolStarted);engine.submit("Continue");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(engine.lastTurnSucceeded());QVERIFY(started.isEmpty());QCOMPARE(engine.messages().first(),input.first());QCOMPARE(results(engine.messages()).size(),1);
    }
    void cancellationAfterRecoveryKeepsHistoryPaired(){
        Server server;AgentEngine engine;engine.setProvider(server.provider());const QJsonArray input{assistant({call("pending")})};engine.setMessages(input);
        connect(&engine,&AgentEngine::busyChanged,&engine,[&](bool busy){if(busy)engine.cancel();});QSignalSpy done(&engine,&AgentEngine::finished),started(&engine,&AgentEngine::toolStarted);QVERIFY(engine.submitWithAttachments("Continue",{}));QCOMPARE(done.size(),1);QVERIFY(!engine.isBusy());QVERIFY(!engine.lastTurnSucceeded());QVERIFY(started.isEmpty());QVERIFY(server.requests.isEmpty());QCOMPARE(engine.messages().size(),3);QCOMPARE(engine.messages().first(),input.first());QCOMPARE(results(engine.messages()).size(),1);
    }
    void busySourceForkRecoversOnlyTarget(){
        Server server;auto streamed=call("source-pending");streamed["index"]=0;server.replies={sseEvent({{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"tool_calls",QJsonArray{streamed}}}},{"finish_reason","tool_calls"}}}}})+"data: [DONE]\n\n"};QTemporaryDir workspace;
        AgentEngine source;source.setProvider(server.provider());source.setWorkspace(workspace.path());QSignalSpy approval(&source,&AgentEngine::approvalRequired);source.submit("Original operation");QTRY_COMPARE_WITH_TIMEOUT(approval.size(),1,5000);QVERIFY(source.isBusy());const auto sourceHistory=source.messages();
        AgentEngine fork;fork.setProvider(server.provider());fork.setWorkspace(workspace.path());fork.setAlwaysAllow(true);fork.setMessages(sourceHistory);QSignalSpy done(&fork,&AgentEngine::finished),tools(&fork,&AgentEngine::toolStarted);fork.submit("Do something else");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(fork.lastTurnSucceeded());QVERIFY(tools.isEmpty());QVERIFY(!QFileInfo::exists(workspace.filePath("must-not-exist.txt")));QCOMPARE(source.messages(),sourceHistory);QVERIFY(source.isBusy());QCOMPARE(approval.size(),1);QCOMPARE(results(fork.messages()).size(),1);source.cancel();
    }
};
QTEST_GUILESS_MAIN(HistoryRecoveryTest)
#include "test_history_recovery.moc"
