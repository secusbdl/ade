#include "core/AgentPresentation.h"
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QtTest>

using namespace ade;
namespace {
QJsonObject message(const QString& role,const QString& content){return {{"role",role},{"content",content}};}
QJsonObject call(const QString& id="reused"){
    const QJsonObject function{{"name","readFile"},{"arguments","{\"path\":\"example.txt\"}"}};
    const QJsonObject tool{{"id",id},{"type","function"},{"function",function}};
    return {{"role","assistant"},{"content","Inspect"},{"tool_calls",QJsonArray{tool}}};
}
QJsonObject result(const QString& text,const QString& id="reused"){
    return {{"role","tool"},{"tool_call_id",id},{"content",text}};
}
QJsonArray history(int pairs){
    QJsonArray rows;for(int i=0;i<pairs;++i){rows.append(message("user",QString("Question %1").arg(i)));rows.append(message("assistant",QString("Answer %1").arg(i)));}return rows;
}
}
class HistoryProjectionTest final:public QObject {
    Q_OBJECT
private slots:
    void unchangedHistoryDoesNoProjectionWork(){
        AgentHistoryProjection cache;const auto rows=history(1000),before=rows;
        const auto projected=cache.project(rows);QCOMPARE(projected,AgentPresentation::history(rows));
        const auto processed=cache.processedMessages();QCOMPARE(processed,quint64(2000));
        for(int i=0;i<300;++i)QCOMPARE(cache.project(rows),projected);
        QCOMPARE(cache.processedMessages(),processed);QCOMPARE(rows,before);
    }
    void changingToolTailDoesNotReprojectSettledRows(){
        AgentHistoryProjection cache;auto rows=history(500);rows.append(call());
        cache.project(rows);const auto first=cache.processedMessages();
        for(int i=0;i<80;++i){
            // Same assistant input, changing tool result, as happens during a
            // running shell. Input and all prior messages remain untouched.
            if(rows.size()==1002)rows.removeLast();
            rows.append(result(QString::number(i)));
            const auto actual=cache.project(rows);QCOMPARE(actual,AgentPresentation::history(rows));
            QCOMPARE(actual.last().toObject()["parts"].toArray().last().toObject()["output"].toString(),QString::number(i));
        }
        QCOMPARE(cache.processedMessages()-first,quint64(160));
    }
    void toolIdsAttachmentsAndDuplicateRowsStayBatchLocal(){
        AgentHistoryProjection cache;QJsonArray rows{message("user","same"),call()};
        cache.project(rows);auto output=result("first");
        const QJsonArray attachments{QJsonObject{{"path","att/first.txt"}}};
        output["metadata"]=QJsonObject{{"attachments",attachments}};
        rows.append(output);cache.project(rows);
        rows.append(message("user","same"));rows.append(call());rows.append(result("second"));
        auto actual=cache.project(rows);QCOMPARE(actual,AgentPresentation::history(rows));QCOMPARE(actual.size(),4);
        const auto first=actual.at(1).toObject(),second=actual.at(3).toObject();
        QVERIFY(first["id"]!=second["id"]);QCOMPARE(first["attachments"].toArray().size(),1);QVERIFY(!second.contains("attachments"));
        QCOMPARE(first["parts"].toArray().last().toObject()["output"].toString(),QString("first"));
        QCOMPARE(second["parts"].toArray().last().toObject()["output"].toString(),QString("second"));
    }
    void replacementTruncationAndClearCannotReuseStalePrefix(){
        AgentHistoryProjection cache;auto rows=history(6);cache.project(rows);
        rows[0]=message("user","Changed first turn");QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        rows[4]=message("system","hidden");QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        while(rows.size()>3)rows.removeLast();
        QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        QCOMPARE(cache.project({}),QJsonArray());
        rows=QJsonArray{message("user","only user")};QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        rows.append(result("orphan"));QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        rows.append(call());QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        rows[0]=message("assistant","replacement");QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
    }
    void retainedOutputSnapshotsAreImmutable(){
        AgentHistoryProjection cache;QJsonArray rows{message("user","Question"),call(),result("old")};
        const auto original=cache.project(rows);const auto bytes=QJsonDocument(original).toJson();
        rows[2]=result("new");const auto next=cache.project(rows);QVERIFY(next!=original);
        QCOMPARE(QJsonDocument(original).toJson(),bytes);
        auto editable=next;editable[0]=QJsonObject{{"role","changed by consumer"}};
        QCOMPARE(cache.project(rows),next);
    }
    void mutationReferencesBelongOnlyToFirstMatchingResult(){
        const auto mutation=[](const QString& id){return QJsonObject{{"mutationId",id},{"agentId","owner"},{"committed",true},{"recoveryRequired",false}};};
        auto first=result("first");first["metadata"]=QJsonObject{{"mutations",QJsonArray{mutation("first"),mutation("first")}}};
        auto duplicate=result("duplicate");duplicate["metadata"]=QJsonObject{{"mutations",QJsonArray{mutation("untrusted-duplicate")}}};
        auto unmatched=result("unmatched","unknown-call");unmatched["metadata"]=QJsonObject{{"mutations",QJsonArray{mutation("unmatched")}}};
        AgentHistoryProjection cache;QJsonArray rows{call(),first,duplicate,unmatched};
        const auto projected=cache.project(rows);QCOMPARE(projected.size(),1);
        QCOMPARE(projected.first().toObject()["mutations"].toArray(),QJsonArray{mutation("first")});
        auto second=result("second");second["metadata"]=QJsonObject{{"mutations",QJsonArray{mutation("second")}}};
        rows.append(call());rows.append(second);const auto next=cache.project(rows);
        QCOMPARE(next.last().toObject()["mutations"].toArray(),QJsonArray{mutation("second")});
        QCOMPARE(next,AgentPresentation::history(rows));
    }
    void malformedAndExcessMutationReferencesAreBounded(){
        QJsonArray records;
        records.append(QJsonObject{{"mutationId","../other"},{"agentId","owner"},{"committed",true},{"recoveryRequired",false}});
        records.append(QJsonObject{{"mutationId","wrong-type"},{"agentId","owner"},{"committed","true"},{"recoveryRequired",false}});
        for(int i=0;i<513;++i)records.append(QJsonObject{{"mutationId",QString("valid-%1").arg(i)},{"agentId","owner"},{"committed",true},{"recoveryRequired",false},{"ignoredExtra","not an authority field"}});
        auto output=result("done");output["metadata"]=QJsonObject{{"mutations",records}};
        AgentHistoryProjection cache;const auto projected=cache.project(QJsonArray{call(),output}).first().toObject();
        QCOMPARE(projected["mutations"].toArray().size(),512);QCOMPARE(projected["mutationsIncomplete"],QJsonValue(true));
        for(const auto& entry:projected["mutations"].toArray())QCOMPARE(entry.toObject().size(),4);
    }
    void savedIdCollisionsAndMalformedMetadataMatchFreshProjection(){
        AgentHistoryProjection cache;AgentPresentation display;display.begin();display.text("text","same");display.finish();
        auto saved=AgentPresentation::withMetadata(message("assistant","same"),display.metadata());
        QJsonArray rows{saved,saved,message("user","same")};cache.project(rows);
        rows.append(saved);QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        auto changed=saved;changed["content"]="does not match saved presentation";rows[0]=changed;
        QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
        rows[0]=saved;QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
    }
    void deterministicMixedEditsMatchFreshProjection(){
        AgentHistoryProjection cache;QRandomGenerator random(418);QJsonArray rows;
        for(int i=0;i<400;++i){
            const int action=random.bounded(6);const auto next=message(i%2?"assistant":"user",QString::number(random.bounded(8)));
            if(rows.isEmpty()||action<2)rows.append(next);
            else if(action==2)rows[random.bounded(int(rows.size()))]=next;
            else if(action==3)rows.removeAt(random.bounded(int(rows.size())));
            else if(action==4){rows.append(call());rows.append(result(QString::number(i)));}
            else rows.insert(random.bounded(int(rows.size())),message("system","hidden"));
            const auto input=QJsonDocument(rows).toJson();QCOMPARE(cache.project(rows),AgentPresentation::history(rows));
            QCOMPARE(QJsonDocument(rows).toJson(),input);
        }
    }
};
QTEST_GUILESS_MAIN(HistoryProjectionTest)
#include "test_history_projection.moc"
