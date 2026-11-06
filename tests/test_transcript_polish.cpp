#include "ui/ConversationView.h"
#include "ui/MarkdownParts.h"
#include "ui/AppFonts.h"
#include <QApplication>
#include <QFontInfo>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextFragment>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject text(const QString& id,const QString& value,const QString& state="done"){return {{"id",id},{"type","text"},{"text",value},{"state",state}};}
QJsonObject message(const QString& id,const QString& role,const QJsonArray& parts){return {{"id",id},{"role",role},{"parts",parts}};}
void settle(){for(int i=0;i<8;++i){QApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QApplication::processEvents();QTest::qWait(20);}}
}
class PolishTests:public QObject{
 Q_OBJECT
private slots:
 void inlineCodeUsesApplicationMono(){
  TranscriptText view;view.resize(600,100);view.setSource("Marker: `ADE_LIVE_RENDER_OK` and ordinary prose.");view.show();settle();bool found=false;
  for(auto block=view.document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){const auto fragment=it.fragment();if(fragment.text().contains("ADE_LIVE_RENDER_OK")){found=true;const auto format=fragment.charFormat();qInfo()<<"inline code font"<<format.fontFamilies()<<QFontInfo(format.font()).family()<<"fixed"<<format.fontFixedPitch();QCOMPARE(QFontInfo(format.font()).family(),QString("Roboto Mono"));}}
  QVERIFY(found);
 }
 void completedShortTurnKeepsFollowingAfterLayout_data(){QTest::addColumn<int>("pixels");QTest::newRow("normal")<<14;QTest::newRow("large")<<18;}
 void completedShortTurnKeepsFollowingAfterLayout(){
  QFETCH(int,pixels);ConversationView view;auto font=view.font();font.setPixelSize(pixels);view.setFont(font);view.resize(1500,1200);view.show();
  const auto user=message("user","user",{text("u","Use the read tool exactly once to read ws1/probe.txt. Then answer in Markdown with a heading Native render check, one bullet with the exact marker read, and a fenced cpp block containing only int answer = 42;. Do not call other tools or change files. Keep the response below70 words.")});
  QJsonObject tool{{"id","read"},{"type","tool"},{"toolCallId","call"},{"name","read"},{"inputText","{\"path\":\"ws1/probe.txt\"}"},{"state","output-available"},{"output","ADE_LIVE_RENDER_OK"}};
  auto rows=QJsonArray{user,message("assistant","assistant",{tool,text("reason","Checked file", "done")})};view.setConversation("chat",rows,true);settle();rows[1]=message("assistant","assistant",{tool,text("answer","# Native render check\n- Marker: `ADE_LIVE_RENDER_OK`\n\n```cpp\nint answer = 42;\n```\n")});view.setConversation("chat",rows,false);settle();
  auto* scroll=view.findChild<QScrollArea*>("transcriptScroll");auto* bar=scroll->verticalScrollBar();auto* jump=view.findChild<QToolButton*>("jumpToBottom");qInfo()<<"scroll"<<bar->value()<<bar->maximum()<<"jump"<<jump->isVisible()<<"viewport"<<scroll->viewport()->size();
  const auto capture=qEnvironmentVariable("ADE_POLISH_CAPTURE_DIR");if(!capture.isEmpty())view.grab().save(capture+QString("/short-%1.png").arg(pixels));QCOMPARE(bar->value(),bar->maximum());QVERIFY(view.followsOutput());QVERIFY(!jump->isVisible());
 }
 void jumpUsesSourceBottomThreshold(){
  ConversationView view;view.resize(640,480);QJsonArray rows;for(int i=0;i<20;++i)rows.append(message(QString::number(i),"assistant",{text("text"+QString::number(i),QString("Readable prose paragraph. ").repeated(12))}));view.setConversation("chat",rows,false);view.show();settle();auto* bar=view.findChild<QScrollArea*>("transcriptScroll")->verticalScrollBar();auto* jump=view.findChild<QToolButton*>("jumpToBottom");QVERIFY(bar->maximum()>150);
  bar->setValue(bar->maximum()-50);QVERIFY(!jump->isVisible());bar->setValue(bar->maximum()-100);QVERIFY(!jump->isVisible());bar->setValue(bar->maximum()-101);QVERIFY(jump->isVisible());jump->click();settle();QCOMPARE(bar->value(),bar->maximum());QVERIFY(!jump->isVisible());
 }
};
int main(int argc,char** argv){QApplication app(argc,argv);AppFonts::load();QFont font("Geist");font.setPixelSize(14);app.setFont(font);PolishTests test;return QTest::qExec(&test,argc,argv);}
#include "test_transcript_polish.moc"
