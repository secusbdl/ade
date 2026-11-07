#include "ui/ConversationView.h"
#include "ui/AppFonts.h"
#include <QtTest>
#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QWheelEvent>
#include <QTimer>
using namespace ade;
namespace {
QJsonObject part(QString id,QString state="done",QString type="text") {return {{"id",id},{"type",type},{"state",state},{"text","Some readable text. "}};}
QJsonObject row(QString id,QString role,QJsonArray parts){return {{"id",id},{"role",role},{"parts",parts}};}
QJsonArray history(){QJsonArray out;for(int i=0;i<30;++i){auto p=part("p"+QString::number(i));p["text"]=QString("History paragraph %1 with stable readable content. ").arg(i).repeated(8);out.append(row(QString::number(i),"assistant",{p}));}return out;}
QScrollBar* bar(ConversationView& v){return v.findChild<QScrollArea*>("transcriptScroll")->verticalScrollBar();}
void wheel(ConversationView& v){auto* viewport=v.findChild<QScrollArea*>("transcriptScroll")->viewport();QWheelEvent e(QPointF(40,40),viewport->mapToGlobal(QPoint(40,40)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(viewport,&e);}
void expose(ConversationView& v){v.resize(640,400);v.show();QTest::qWait(120);}
}
class StreamingFollowups final:public QObject{
 Q_OBJECT
private slots:
 void initTestCase(){AppFonts::load();}
 void independentLayoutPreservesReading_data(){QTest::addColumn<QString>("input");QTest::newRow("wheel")<<"wheel";QTest::newRow("page-up")<<"page-up";QTest::newRow("scrollbar-value")<<"scrollbar-value";}
 void independentLayoutPreservesReading(){
  QFETCH(QString,input);ConversationView v;v.setConversation("reading",history(),true);expose(v);auto* scroll=bar(v);
  QTRY_VERIFY(scroll->maximum()>2000);QTRY_COMPARE(scroll->value(),scroll->maximum());
  if(input=="wheel"){for(int i=0;i<5;++i)wheel(v);}
  else if(input=="page-up"){scroll->setFocus();QTest::keyClick(scroll,Qt::Key_PageUp);}
  else scroll->setValue(scroll->maximum()-500);
  QVERIFY(!v.followsOutput());const int before=scroll->value();QVERIFY(scroll->maximum()-before>100);
  auto* content=v.findChild<QWidget*>("transcriptContent");QVERIFY(content);QEvent layout(QEvent::LayoutRequest);QApplication::sendEvent(content,&layout);QTest::qWait(100);
  QVERIFY2(qAbs(scroll->value()-before)<=2,qPrintable(QString("reading=%1 restored=%2 maximum=%3").arg(before).arg(scroll->value()).arg(scroll->maximum())));
 }
 void userInputBetweenLayoutAndRestore_data(){QTest::addColumn<QString>("input");QTest::newRow("wheel")<<"wheel";QTest::newRow("page-up")<<"page-up";}
 void userInputBetweenLayoutAndRestore(){
  QFETCH(QString,input);ConversationView v;v.setConversation("queued-reading",history(),true);expose(v);auto* scroll=bar(v);
  QTRY_VERIFY(scroll->maximum()>2000);QTRY_COMPARE(scroll->value(),scroll->maximum());
  auto* content=v.findChild<QWidget*>("transcriptContent");QEvent layout(QEvent::LayoutRequest);QApplication::sendEvent(content,&layout);
  int intended=-1;QTimer::singleShot(0,&v,[&]{if(input=="wheel"){for(int i=0;i<5;++i)wheel(v);}else{scroll->setFocus();QTest::keyClick(scroll,Qt::Key_PageUp);}intended=scroll->value();});
  QTest::qWait(100);QVERIFY(intended>=0);QVERIFY(scroll->maximum()-intended>100);QVERIFY(!v.followsOutput());
  QVERIFY2(qAbs(scroll->value()-intended)<=2,qPrintable(QString("intent=%1 restored=%2").arg(intended).arg(scroll->value())));
 }
 void stallIndicatorMatchesUnfinishedAssistant_data(){
  QTest::addColumn<QString>("role");QTest::addColumn<QString>("type");QTest::addColumn<QString>("state");QTest::addColumn<bool>("expected");
  QTest::newRow("waiting-first-response")<<"user"<<"text"<<"done"<<false;
  QTest::newRow("between-tool-steps")<<"assistant"<<"tool"<<"output-available"<<false;
  QTest::newRow("running-tool")<<"assistant"<<"tool"<<"running"<<true;
  QTest::newRow("streaming-reasoning")<<"assistant"<<"reasoning"<<"streaming"<<true;
 }
 void stallIndicatorMatchesUnfinishedAssistant(){
  QFETCH(QString,role);QFETCH(QString,type);QFETCH(QString,state);QFETCH(bool,expected);
  auto p=part("part",state,type);if(type=="tool"){p["name"]="read";p["toolCallId"]="call";p["inputText"]="{}";}
  ConversationView v;v.setConversation("stall",{row("message",role,{p})},true);expose(v);auto* activity=v.findChild<QLabel*>("transcriptActivity");QVERIFY(activity);QTest::qWait(5300);
  QCOMPARE(activity->isVisible(),expected);if(expected)QCOMPARE(activity->text(),QString("Still on it…"));
 }
 void reasoningOnlyGroupHasThinkingLabel(){
  ConversationView v;auto a=part("reason-a","streaming","reasoning"),b=part("reason-b","streaming","reasoning");v.setConversation("reasoning",{row("reply","assistant",{a,b})},true);expose(v);
  auto* group=v.findChild<QWidget*>("explorationGroup");QVERIFY(group);auto* title=group->findChild<QToolButton*>();QVERIFY(title);QCOMPARE(title->text(),QString("Thinking..."));
  a["state"]="done";b["state"]="done";v.setConversation("reasoning",{row("reply","assistant",{a,b})},false);QTest::qWait(60);QCOMPARE(title->text(),QString("Thought"));
 }
};
QTEST_MAIN(StreamingFollowups)
#include "test_streaming_followups.moc"
