#include "ui/ConversationView.h"
#include "ui/MarkdownParts.h"
#include "editor/CodeEditor.h"
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
using namespace ade;
namespace {
QJsonObject part(QString id,QString value){return {{"id",id},{"type","text"},{"text",value},{"state","done"}};}
QJsonArray rows(QString text){return {QJsonObject{{"id","row"},{"role","assistant"},{"parts",QJsonArray{part("part",text)}}}};}
void show(ConversationView &v){v.resize(600,500);v.show();}
QImage sample(){QImage i(120,80,QImage::Format_ARGB32_Premultiplied);i.fill(QColor("#00b8c8"));QPainter p(&i);p.fillRect(4,4,20,20,QColor("#d42335"));return i;}
quint64 id(QSignalSpy &spy,int index=0){return spy.at(index).at(0).toULongLong();}
}
class RichTranscriptTest:public QObject {
    Q_OBJECT
private slots:
    void trustedImageIsRenderedWithoutResourceLoading(){
        ConversationView v;show(v);QSignalSpy spy(&v,&ConversationView::imageRequested);v.setConversation("chat",rows("Before\n\n![diagram](path:att/diagram.png)\n\nAfter"),false);QTRY_COMPARE(spy.size(),1);
        QCOMPARE(spy[0][1].toString(),QString("chat"));QCOMPARE(spy[0][2].toString(),QString("row"));QCOMPARE(spy[0][3].toString(),QString("part"));QVERIFY(v.provideImage(id(spy),sample()));QTest::qWait(40);
        auto *text=v.findChild<TranscriptText*>();QVERIFY(text);QCOMPARE(text->document()->property("trustedImageCount").toInt(),1);
        const auto image=v.grab().toImage();int pixels=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==QColor("#00b8c8"))++pixels;QVERIFY(pixels>1000);
        QVERIFY(text->document()->resource(QTextDocument::ImageResource,QUrl("path:att/diagram.png")).isNull());QVERIFY(!v.provideImage(id(spy),sample()));
    }
    void unsafeSourcesNeverRequest(){
        ConversationView v;show(v);QSignalSpy spy(&v,&ConversationView::imageRequested);v.setConversation("chat",rows("![remote](https://example.com/x.png) ![local](file:///C:/private.png) ![data](data:image/png;base64,AAAA)"),false);QTest::qWait(40);QCOMPARE(spy.size(),0);
    }
    void relativeMountBecomesScopedPath(){ConversationView v;QSignalSpy s(&v,&ConversationView::imageRequested);v.setConversation("c",rows("![x](att/a.png)"),false);QTRY_COMPARE(s.size(),1);QCOMPARE(s[0][4].toUrl(),QUrl("path:att/a.png"));}
    void staleImagesDoNotCrossChats(){ConversationView v;QSignalSpy s(&v,&ConversationView::imageRequested);v.setConversation("old",rows("![x](path:att/a.png)"),false);QTRY_COMPARE(s.size(),1);v.setConversation("new",rows("![x](path:att/a.png)"),false);QTRY_COMPARE(s.size(),2);QVERIFY(!v.provideImage(id(s),sample()));QVERIFY(v.provideImage(id(s,1),sample()));}
    void changedOrRemovedPartRejectsImage(){ConversationView v;QSignalSpy s(&v,&ConversationView::imageRequested);v.setConversation("c",rows("![x](path:att/a.png)"),false);QTRY_COMPARE(s.size(),1);v.setConversation("c",rows("![x](path:att/b.png)"),false);QTRY_COMPARE(s.size(),2);QVERIFY(!v.provideImage(id(s),sample()));v.setConversation("c",{},false);QVERIFY(!v.provideImage(id(s,1),sample()));}
    void imageRequestMayDeleteView(){QPointer<ConversationView> v=new ConversationView;connect(v,&ConversationView::imageRequested,v,[&]{delete v.data();});v->setConversation("c",rows("![x](path:att/a.png)"),false);QTRY_VERIFY(!v);}
    void synchronousImageReplyMaySwitchChat(){ConversationView v;connect(&v,&ConversationView::imageRequested,&v,[&](quint64 n){v.provideImage(n,sample());v.setConversation("replacement",rows("Replacement"),false);});v.setConversation("c",rows("![x](path:att/a.png)"),false);QTRY_COMPARE(v.conversationId(),QString("replacement"));QCOMPARE(v.toPlainText(),QString("Replacement"));}
    void imageRequestsAreBoundedAndUnchangedRowsDoNotRepeat(){ConversationView v;QSignalSpy s(&v,&ConversationView::imageRequested);QString text;for(int i=0;i<100;++i)text+=QString("![x](path:att/%1.png)\n").arg(i);v.setConversation("c",rows(text),true);QTRY_COMPARE(s.size(),32);v.setConversation("c",rows(text+"\nMore"),true);QTest::qWait(30);QCOMPARE(s.size(),32);}
    void oversizeAndErrorRepliesRemainPlaceholders(){ConversationView v;QSignalSpy s(&v,&ConversationView::imageRequested);v.setConversation("c",rows("![x](path:att/a.png) ![y](path:att/b.png)"),false);QTRY_COMPARE(s.size(),2);QImage huge(16385,1,QImage::Format_ARGB32);huge.fill(Qt::red);QVERIFY(!v.provideImage(id(s),huge));QVERIFY(!v.provideImage(id(s,1),{},"Unavailable"));QCOMPARE(v.findChild<TranscriptText*>()->document()->property("trustedImageCount").toInt(),0);}
    void everyFenceSplitConvergesAndKeepsClosedBlock(){
        MarkdownParts md;const QString source="before\n```cpp\nconst int n = 1;\n```\nafter\n~~~python\nprint('done')\n~~~\n";
        for(int split=0;split<=source.size();++split){md.updateText(source.left(split),true);QCoreApplication::processEvents();}
        md.updateText(source,false);auto codes=md.findChildren<CodeEditor*>();QCOMPARE(codes.size(),2);QCOMPARE(codes[0]->sourceText(),QString("const int n = 1;\n"));QCOMPARE(codes[1]->sourceText(),QString("print('done')\n"));QPointer<CodeEditor> first=codes[0];auto cursor=first->textCursor();cursor.setPosition(0);cursor.setPosition(5,QTextCursor::KeepAnchor);first->setTextCursor(cursor);
        md.updateText(source+"stream tail",true);QCOMPARE(md.findChildren<CodeEditor*>()[0],first.data());QCOMPARE(first->textCursor().selectedText(),QString("const"));
    }
    void toolsHaveSeparateCopyableBoundedDisclosures(){
        ConversationView v;show(v);const QString output=QString("a").repeated(70000)+"TAIL";const QJsonObject tool{{"id","tool"},{"type","tool"},{"name","executeSandboxJs"},{"toolCallId","call"},{"state","running"},{"input",QJsonObject{{"script","return 1 + 2;"},{"explanation","Compute"}}},{"output",output}};
        QJsonArray history{QJsonObject{{"id","r"},{"role","assistant"},{"parts",QJsonArray{tool}}}};v.setConversation("c",history,true);QTest::qWait(40);
        auto *args=v.findChild<QToolButton*>("toolArgumentsToggle");auto *result=v.findChild<QToolButton*>("toolResultToggle");QVERIFY(args&&result);QVERIFY(!args->isChecked());QVERIFY(result->isChecked());QCOMPARE(v.findChild<CodeEditor*>("toolCodePreview")->sourceText(),QString("return 1 + 2;"));
        auto *text=v.findChild<TranscriptText*>("toolOutput");QCOMPARE(text->source().size(),65536);QVERIFY(text->source().endsWith("TAIL"));auto *copy=v.findChild<QPushButton*>("copyToolResult");copy->click();QCOMPARE(QApplication::clipboard()->text(),output);
        result->click();QVERIFY(!result->isChecked());auto updated=tool;updated["state"]="output-available";updated["output"]="3";history[0]=QJsonObject{{"id","r"},{"role","assistant"},{"parts",QJsonArray{updated}}};v.setConversation("c",history,false);QVERIFY(!result->isChecked());QCOMPARE(text->source(),QString("3"));
        v.setConversation("other",rows("Other"),false);v.setConversation("c",history,false);QVERIFY(!v.findChild<QToolButton*>("toolResultToggle")->isChecked());
    }
};
QTEST_MAIN(RichTranscriptTest)
#include "test_transcript_rich.moc"
