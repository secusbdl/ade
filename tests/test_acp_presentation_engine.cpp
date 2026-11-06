#include "core/AgentEngine.h"
#include "harness/AcpToolPresentation.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(){ProviderConfig p;p.id="fixture";p.name="Fixture";p.kind="opencode";p.protocol="opencode-acp";p.model="model-b";p.harnessExecutable=QDir(QCoreApplication::applicationDirPath()).filePath("acp_fixture.exe");return p;}
void configure(AgentEngine &e,const QString &path){e.setProvider(provider());e.setHarnessContext("owner",path);}
QJsonArray tools(const QJsonArray &rows){QJsonArray out;for(const auto &value:rows)for(const auto &item:value.toObject()["parts"].toArray())if(item.toObject()["type"]=="tool")out.append(item);return out;}
}
class AcpPresentationEngineTest:public QObject {
 Q_OBJECT
private slots:
 void cleanup(){qunsetenv("ADE_ACP_SCENARIO");qunsetenv("ADE_ACP_LOG");}
 void actualProcessMapsWithoutChangingRawOwnership(){qputenv("ADE_ACP_SCENARIO","presentation");QTemporaryDir dir;AgentEngine e;configure(e,dir.path());QSignalSpy finished(&e,&AgentEngine::finished);QVERIFY(e.submitWithAttachments("map",{}));QTRY_COMPARE(finished.size(),1);QVERIFY(e.lastTurnSucceeded());const auto parts=tools(e.displayMessages());QCOMPARE(parts.size(),5);const auto read=parts[0].toObject();QCOMPARE(read["name"],QJsonValue("acp.vendor-read"));QVERIFY(read["toolCallId"].toString().endsWith("/read-id"));QCOMPARE(read["input"].toObject()["rawSentinel"],QJsonValue("retain me"));const auto mapped=read["acpPresentation"].toObject();QVERIFY(AcpToolPresentation::valid(mapped));QCOMPARE(mapped["entries"].toArray()[0].toObject()["path"],QJsonValue("ws1/probe.txt"));QCOMPARE(parts[4].toObject()["acpPresentation"].toObject()["entries"].toArray().size(),2);QVERIFY(!QJsonDocument(e.messages()).toJson().contains("mutationId"));}
 void restartPreservesMetadataAndReusedRawIdsStayDistinct(){qputenv("ADE_ACP_SCENARIO","presentation");QTemporaryDir dir;AgentEngine e;configure(e,dir.path());QSignalSpy done(&e,&AgentEngine::finished);QVERIFY(e.submitWithAttachments("first",{}));QTRY_COMPARE(done.size(),1);const auto saved=QJsonDocument::fromJson(QJsonDocument(e.messages()).toJson()).array();const auto projected=e.displayMessages();AgentEngine next;configure(next,dir.path());next.setMessages(saved);QCOMPARE(next.displayMessages(),projected);QSignalSpy again(&next,&AgentEngine::finished);QVERIFY(next.submitWithAttachments("second",{}));QTRY_COMPARE(again.size(),1);const auto parts=tools(next.displayMessages());QCOMPARE(parts.size(),10);QVERIFY(parts[0].toObject()["toolCallId"]!=parts[5].toObject()["toolCallId"]);QCOMPARE(parts[0].toObject()["acpPresentation"],parts[5].toObject()["acpPresentation"]);QCOMPARE(e.messages(),saved);}
 void approvalBeforeUpdateKeepsOpaqueDecision_data(){QTest::addColumn<bool>("allow");QTest::newRow("allow")<<true;QTest::newRow("deny")<<false;}
 void approvalBeforeUpdateKeepsOpaqueDecision(){QFETCH(bool,allow);qputenv("ADE_ACP_SCENARIO","presentation-approval-first");QTemporaryDir dir;const auto log=dir.filePath("rpc.jsonl");qputenv("ADE_ACP_LOG",log.toUtf8());AgentEngine e;configure(e,dir.path());QSignalSpy approval(&e,&AgentEngine::approvalRequired),done(&e,&AgentEngine::finished);QVERIFY(e.submitWithAttachments("permission",{}));QTRY_COMPARE(approval.size(),1);QTRY_VERIFY(tools(e.displayMessages())[0].toObject()["output"].toString().contains("partial"));const auto before=tools(e.displayMessages())[0].toObject();QCOMPARE(before["state"],QJsonValue("approval-requested"));QCOMPARE(before["name"],QJsonValue("acp.vendor-read"));QVERIFY(AcpToolPresentation::valid(before["acpPresentation"].toObject()));e.approveTool("tool-1",allow);QVERIFY(e.isBusy());e.approveTool(approval[0][0].toString(),allow);QTRY_COMPARE(done.size(),1);QCOMPARE(approval.size(),1);QFile file(log);QVERIFY(file.open(QIODevice::ReadOnly));QJsonArray responses;for(const auto &line:file.readAll().split('\n')){const auto row=QJsonDocument::fromJson(line).object();if(row["id"]=="approval-1")responses.append(row);}QCOMPARE(responses.size(),1);QCOMPARE(responses[0].toObject()["result"].toObject()["outcome"].toObject()["optionId"],QJsonValue(allow?"yes":"no"));QCOMPARE(tools(e.displayMessages())[0].toObject()["id"],before["id"]);}
 void displayObserverCanCancel(){qputenv("ADE_ACP_SCENARIO","presentation");QTemporaryDir dir;AgentEngine e;configure(e,dir.path());bool canceled=false;connect(&e,&AgentEngine::displayChanged,&e,[&]{if(!canceled&&!tools(e.displayMessages()).isEmpty()){canceled=true;e.cancel();}});QSignalSpy done(&e,&AgentEngine::finished);QVERIFY(e.submitWithAttachments("cancel",{}));QTRY_COMPARE(done.size(),1);QVERIFY(canceled);QVERIFY(!e.isBusy());}
 void displayObserverCanDestroyOwner(){qputenv("ADE_ACP_SCENARIO","presentation");QTemporaryDir dir;QPointer<AgentEngine> e=new AgentEngine;configure(*e,dir.path());connect(e,&AgentEngine::displayChanged,e,[&]{if(e&&!tools(e->displayMessages()).isEmpty())delete e.data();});QVERIFY(e->submitWithAttachments("delete",{}));QTRY_VERIFY(!e);}
};
QTEST_GUILESS_MAIN(AcpPresentationEngineTest)
#include "test_acp_presentation_engine.moc"
