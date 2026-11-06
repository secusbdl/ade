#include "ConversationView.h"
#include "MarkdownParts.h"
#include "editor/CodeEditor.h"
#include "ui/AppFonts.h"
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QJsonObject>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QToolButton>
#include <QWheelEvent>

using namespace ade;
namespace {
QJsonObject text(QString id,QString value,QString state="done",QString type="text"){return {{"id",id},{"type",type},{"text",value},{"state",state}};}
QJsonObject message(QString id,QString role,QJsonArray parts){return {{"id",id},{"role",role},{"provisional",false},{"parts",parts}};}
QJsonObject tool(QString id,QString name,QString state,QString output={}){return {{"id",id},{"type","tool"},{"toolCallId","call-"+id},{"name",name},{"inputText","{\"path\":\"src/main.cpp\"}"},{"state",state},{"output",output}};}
void expose(ConversationView &view,int width=640,int height=480){view.resize(width,height);view.show();QTest::qWait(30);}
QWidget *part(ConversationView &view,QString id){for(auto *w:view.findChildren<QWidget*>())if(w->property("partId")==id)return w;return nullptr;}
QToolButton *disclosure(ConversationView &view,QString id){auto *p=part(view,id);return p?p->findChild<QToolButton*>():nullptr;}
QScrollBar *bar(ConversationView &view){return view.findChild<QScrollArea*>("transcriptScroll")->verticalScrollBar();}
QJsonArray longHistory(){QJsonArray rows;for(int i=0;i<20;++i)rows.append(message("m"+QString::number(i),"assistant",{text("p"+QString::number(i),QString("Paragraph %1. ").arg(i)+QString("This is a long line of readable content. ").repeated(5))}));return rows;}
void wheelUp(ConversationView &view){auto *target=view.findChild<QScrollArea*>("transcriptScroll")->viewport();QWheelEvent event(QPointF(40,40),target->mapToGlobal(QPoint(40,40)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(target,&event);}
}

class TranscriptTest final:public QObject {
    Q_OBJECT
private slots:
    void orderedPartsRetainWidgetsAndSelection();
    void reasoningDefaultsAndManualState();
    void codeCopyPreservesBytesAndStreamingState();
    void unfinishedFenceStaysCode();
    void noImplicitImagesOrUnsafeLinks();
    void toolStates_data();
    void toolStates();
    void inlineApprovalIsOneDecisionForExactCall();
    void approvalCallbackMayDeleteView();
    void scrollIntentAnchorAndJump();
    void conversationSwitchRestoresDisclosure();
    void provisionalRetentionDoesNotDuplicate();
    void removedRowsAreDestroyed();
    void attachmentChipsAndPlainText();
    void themeChangesCodePalette();
    void largeHistoryRetainsUnchangedDocuments();
    void replacementOfAdjacentFenceKinds();
    void changedPartRetainsSelectedText();
    void scrollEndNearBottomRestartsFollowing();
    void scrollStateSurvivesChatSwitch();
    void scrollObserverCanDeleteView();
    void scrollObserverCanSwitchConversation();
    void fontScaleIsAppliedOnce();
    void emptyReasoningHeaderAppearsBeforeText();
    void completedReadBeforeReadyIsCompact();
    void queuedApprovalKeepsOriginalIdentity();
    void retiredApprovalCannotEmitAgain();
};

void TranscriptTest::orderedPartsRetainWidgetsAndSelection(){
    ConversationView view;QJsonArray rows{message("user","user",{text("u","Inspect **this**")}),message("assistant","assistant",{text("r","Consider the inputs.","streaming","reasoning"),text("a","First answer."),tool("t","write","running")})};
    view.setConversation("chat",rows,true);expose(view);
    auto *first=part(view,"a");QVERIFY(first);auto *browser=first->findChild<TranscriptText*>();QVERIFY(browser);
    QTextCursor selection(browser->document());selection.setPosition(0);selection.setPosition(5,QTextCursor::KeepAnchor);browser->setTextCursor(selection);
    const int updates=browser->updateCount();QPointer<QWidget> stable=first;
    rows[1]=message("assistant","assistant",{text("r","Consider the inputs.","done","reasoning"),text("a","First answer."),tool("t","write","output-available","Saved"),text("b","Second **answer**.","streaming")});
    view.setConversation("chat",rows,true);QTest::qWait(30);
    QCOMPARE(part(view,"a"),stable.data());QCOMPARE(browser->updateCount(),updates);QCOMPARE(browser->textCursor().selectedText(),QString("First"));
    QVERIFY(part(view,"r")->mapTo(&view,QPoint(0,0)).y()<first->mapTo(&view,QPoint(0,0)).y());
    QVERIFY(first->mapTo(&view,QPoint(0,0)).y()<part(view,"t")->mapTo(&view,QPoint(0,0)).y());
    QVERIFY(part(view,"t")->mapTo(&view,QPoint(0,0)).y()<part(view,"b")->mapTo(&view,QPoint(0,0)).y());
}
void TranscriptTest::reasoningDefaultsAndManualState(){
    ConversationView view;auto reasoning=text("r","Reasoning text","streaming","reasoning");
    view.setConversation("chat",{message("a","assistant",{reasoning})},true);expose(view);
    auto *toggle=disclosure(view,"r");QVERIFY(toggle);QVERIFY(toggle->isChecked());QCOMPARE(toggle->text(),QString("Thinking..."));
    reasoning["state"]="done";reasoning["durationMs"]=1499;
    view.setConversation("chat",{message("a","assistant",{reasoning,text("answer","Answer")})},false);QTest::qWait(20);
    QVERIFY(!toggle->isChecked());QCOMPARE(toggle->text(),QString("Thought for 1s"));
    QTest::mouseClick(toggle,Qt::LeftButton);QVERIFY(toggle->isChecked());
    view.setConversation("chat",{message("a","assistant",{reasoning,text("answer","Answer updated")})},false);QVERIFY(toggle->isChecked());
    QTest::mouseClick(toggle,Qt::LeftButton);QVERIFY(!toggle->isChecked());
    view.setConversation("chat",{message("a","assistant",{reasoning})},false);QVERIFY(!toggle->isChecked());
}
void TranscriptTest::codeCopyPreservesBytesAndStreamingState(){
    ConversationView view;const QString source=QString::fromUtf8("const café = \"a\u00a0b\";\n\t// العربية\n");
    auto content=text("p","```cpp\n"+source+"```","streaming");view.setConversation("chat",{message("a","assistant",{content})},true);expose(view);
    auto *copy=view.findChild<QPushButton*>("copyCode");auto *code=view.findChild<CodeEditor*>("codeContent");QVERIFY(copy&&code);QVERIFY(!copy->isVisible());QPointer<CodeEditor> stable=code;
    content["state"]="done";view.setConversation("chat",{message("a","assistant",{content})},false);QTest::qWait(20);QCOMPARE(view.findChild<CodeEditor*>("codeContent"),stable.data());QVERIFY(copy->isVisible());
    QTest::mouseClick(copy,Qt::LeftButton);QCOMPARE(QApplication::clipboard()->text(),source);QCOMPARE(copy->text(),QString("Copied"));
}
void TranscriptTest::unfinishedFenceStaysCode(){
    ConversationView view;auto p=text("p","Before\n\n```typescript\nconst x = ","streaming");view.setConversation("chat",{message("a","assistant",{p})},true);expose(view);
    QPointer<CodeEditor> code=view.findChild<CodeEditor*>("codeContent");QVERIFY(code);QCOMPARE(code->sourceText(),QString("const x = "));
    p["text"]="Before\n\n```typescript\nconst x = 2;\n```\nAfter";view.setConversation("chat",{message("a","assistant",{p})},true);QTest::qWait(20);QCOMPARE(view.findChild<CodeEditor*>("codeContent"),code.data());QVERIFY(view.toPlainText().contains("After"));
}
void TranscriptTest::noImplicitImagesOrUnsafeLinks(){
    QTemporaryDir temp;QVERIFY(temp.isValid());QImage image(2,2,QImage::Format_RGB32);image.fill(Qt::red);const auto imagePath=temp.filePath("private.png");QVERIFY(image.save(imagePath));const auto imageUrl=QUrl::fromLocalFile(imagePath);
    ConversationView view;view.setConversation("chat",{message("a","assistant",{text("p","![secret]("+imageUrl.toString()+")\n![remote](https://example.invalid/image.png)\n<script>alert(1)</script>\n[ok](https://example.invalid)")})},false);expose(view);
    QTextDocument defaultDocument;QVERIFY(defaultDocument.resource(QTextDocument::ImageResource,imageUrl).isValid());
    auto *browser=view.findChild<TranscriptText*>();QVERIFY(browser);QVERIFY(!browser->document()->resource(QTextDocument::ImageResource,imageUrl).isValid());QVERIFY(browser->document()->property("blockedResourceLoads").toInt()>0);
    QSignalSpy links(&view,&ConversationView::linkActivated);
    QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl("file:///C:/private/secret.png"))));QCOMPARE(links.size(),0);
    QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl("javascript:alert(1)"))));QCOMPARE(links.size(),0);
    QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl("https://example.invalid"))));QCOMPARE(links.size(),1);
    QVERIFY(QMetaObject::invokeMethod(browser,"anchorClicked",Q_ARG(QUrl,QUrl("path:w1/src/main.cpp"))));QCOMPARE(links.size(),2);
}
void TranscriptTest::toolStates_data(){QTest::addColumn<QString>("state");QTest::addColumn<QString>("prefix");QTest::newRow("ok")<<"output-available"<<"✓ ";QTest::newRow("error")<<"output-error"<<"× ";QTest::newRow("denied")<<"output-denied"<<"Skipped";QTest::newRow("canceled")<<"canceled"<<"Canceled";QTest::newRow("partial-input")<<"input-streaming"<<"Preparing";QTest::newRow("running")<<"running"<<"Running";}
void TranscriptTest::toolStates(){QFETCH(QString,state);QFETCH(QString,prefix);ConversationView view;view.setConversation("chat",{message("a","assistant",{tool("t","write",state,"output content")})},true);expose(view);auto *toggle=disclosure(view,"t");QVERIFY(toggle);QVERIFY(toggle->text().startsWith(prefix));if(state!="output-available")QVERIFY(!toggle->text().contains("✓"));QVERIFY(view.toPlainText().contains("output content"));}
void TranscriptTest::inlineApprovalIsOneDecisionForExactCall(){
    ConversationView view;view.setConversation("chat",{message("a","assistant",{tool("one","write","approval-requested"),tool("two","delete","approval-requested")})},true);expose(view);
    QSignalSpy decisions(&view,&ConversationView::approvalDecided);auto *target=part(view,"two")->findChild<QPushButton*>("approveTool");QVERIFY(target);QTest::mouseClick(target,Qt::LeftButton);QCOMPARE(decisions.size(),1);QCOMPARE(decisions[0][0].toString(),QString("chat"));QCOMPARE(decisions[0][1].toString(),QString("a"));QCOMPARE(decisions[0][2].toString(),QString("two"));QCOMPARE(decisions[0][3].toString(),QString("call-two"));QVERIFY(decisions[0][4].toBool());QVERIFY(!target->isEnabled());QTest::mouseClick(target,Qt::LeftButton);QCOMPARE(decisions.size(),1);
    auto *deny=part(view,"one")->findChild<QPushButton*>("denyTool");QTest::mouseClick(deny,Qt::LeftButton);QCOMPARE(decisions.size(),2);QCOMPARE(decisions[1][3].toString(),QString("call-one"));QVERIFY(!decisions[1][4].toBool());
}
void TranscriptTest::approvalCallbackMayDeleteView(){auto *view=new ConversationView;view->setConversation("chat",{message("a","assistant",{tool("one","write","approval-requested")})},true);expose(*view);QPointer<ConversationView> guarded=view;connect(view,&ConversationView::approvalDecided,view,[view]{delete view;});view->findChild<QPushButton*>("approveTool")->click();QVERIFY(guarded.isNull());}
void TranscriptTest::scrollIntentAnchorAndJump(){
    ConversationView view;auto rows=longHistory();view.setConversation("chat",rows,true);expose(view,600,360);QTRY_VERIFY(bar(view)->maximum()>1000);QTRY_COMPARE(bar(view)->value(),bar(view)->maximum());
    wheelUp(view);QVERIFY(!view.followsOutput());const int before=bar(view)->value();rows.append(message("tail","assistant",{text("tail","New paragraph.\n"+QString("More text. ").repeated(100))}));view.setConversation("chat",rows,true);QTest::qWait(60);QVERIFY(!view.followsOutput());QVERIFY(qAbs(bar(view)->value()-before)<=2);
    auto *jump=view.findChild<QToolButton*>("jumpToBottom");QVERIFY(jump->isVisible());jump->click();QTRY_COMPARE(bar(view)->value(),bar(view)->maximum());QVERIFY(view.followsOutput());
    bar(view)->setValue(250);const auto anchor=bar(view)->value();view.resize(500,360);QTest::qWait(50);QVERIFY(!view.followsOutput());QVERIFY(bar(view)->value()>=anchor-60);
}
void TranscriptTest::conversationSwitchRestoresDisclosure(){ConversationView view;const QJsonArray rows{message("a","assistant",{text("r","Thought","done","reasoning")})};view.setConversation("first",rows,false);expose(view);auto *toggle=disclosure(view,"r");QVERIFY(toggle);toggle->click();QVERIFY(!toggle->isChecked());view.setConversation("second",rows,false);QVERIFY(disclosure(view,"r")->isChecked());view.setConversation("first",rows,false);QVERIFY(!disclosure(view,"r")->isChecked());}
void TranscriptTest::provisionalRetentionDoesNotDuplicate(){ConversationView view;auto row=message("a","assistant",{text("p","Answer","streaming")});row["provisional"]=true;view.setConversation("chat",{row},true);expose(view);QPointer<QWidget> existing=part(view,"p");row["provisional"]=false;row["parts"]=QJsonArray{text("p","Answer")};view.setConversation("chat",{row},false);QCOMPARE(part(view,"p"),existing.data());QCOMPARE(view.findChildren<QWidget*>("message_a").size(),1);QCOMPARE(view.toPlainText(),QString("Answer"));}
void TranscriptTest::removedRowsAreDestroyed(){ConversationView view;view.setConversation("chat",longHistory(),false);expose(view);QPointer<QWidget> removed=part(view,"p1");view.setConversation("chat",{message("a","assistant",{text("p","Remaining")})},false);QVERIFY(removed.isNull());view.clear();QCOMPARE(view.toPlainText(),QString());QCOMPARE(view.conversationId(),QString());QCOMPARE(view.findChildren<TranscriptText*>().size(),0);QTest::qWait(30);}
void TranscriptTest::attachmentChipsAndPlainText(){ConversationView view;auto row=message("u","user",{text("p","Review this file")});row["attachments"]=QJsonArray{QJsonObject{{"path","/private/path/notes.txt"},{"originalFileName","notes.txt"}},QJsonObject{{"path","/other/report.pdf"},{"originalFileName","report.pdf"}}};view.setConversation("chat",{row},false);expose(view,320,400);QCOMPARE(view.findChildren<QLabel*>("attachmentChip").size(),2);QVERIFY(view.toPlainText().contains("notes.txt"));QVERIFY(view.toPlainText().contains("report.pdf"));for(auto *chip:view.findChildren<QLabel*>("attachmentChip"))QVERIFY(chip->width()<=view.width());}
void TranscriptTest::themeChangesCodePalette(){ConversationView view;auto palette=view.palette();palette.setColor(QPalette::Window,QColor("#fafafa"));palette.setColor(QPalette::Text,QColor("#222222"));view.setPalette(palette);view.setConversation("chat",{message("a","assistant",{text("p","```cpp\nconst int value = 2;\n```")})},false);expose(view);auto *code=view.findChild<CodeEditor*>();QVERIFY(code);QVERIFY(code->palette().color(QPalette::Base).lightness()>128);palette.setColor(QPalette::Window,QColor("#20232b"));palette.setColor(QPalette::Text,QColor("#eeeeee"));view.setPalette(palette);QTest::qWait(20);QVERIFY2(code->palette().color(QPalette::Base).lightness()<128,qPrintable(QString("root=%1 card=%2 code=%3 expectedDark=%4").arg(view.palette().color(QPalette::Window).name(),code->parentWidget()->palette().color(QPalette::Window).name(),code->palette().color(QPalette::Base).name()).arg(code->parentWidget()->property("expectedDark").toBool())));}
void TranscriptTest::largeHistoryRetainsUnchangedDocuments(){ConversationView view;QJsonArray rows;for(int i=0;i<160;++i)rows.append(message("m"+QString::number(i),"assistant",{text("p"+QString::number(i),"A short settled message.")}));view.setConversation("chat",rows,false);expose(view);auto *original=part(view,"p0")->findChild<TranscriptText*>();const int updates=original->updateCount();for(int i=0;i<8;++i){rows[159]=message("m159","assistant",{text("p159",QString("Streaming ").repeated(i+1),"streaming")});view.setConversation("chat",rows,true);}QCOMPARE(original->updateCount(),updates);QCOMPARE(view.findChildren<TranscriptText*>().size(),160);}
void TranscriptTest::replacementOfAdjacentFenceKinds(){ConversationView view;view.setConversation("chat",{message("a","assistant",{text("p","```js\none\n```\n```cpp\ntwo\n```")})},false);expose(view);QCOMPARE(view.findChildren<CodeEditor*>().size(),2);view.setConversation("chat",{message("a","assistant",{text("p","Replacement prose\n```cpp\ntwo\n```")})},false);QTest::qWait(20);QCOMPARE(view.findChildren<CodeEditor*>().size(),1);QCOMPARE(view.findChildren<TranscriptText*>().size(),1);QCOMPARE(view.findChild<CodeEditor*>()->sourceText(),QString("two\n"));}
void TranscriptTest::changedPartRetainsSelectedText(){ConversationView view;view.setConversation("chat",{message("a","assistant",{text("p","Choose **selected** text.","streaming")})},true);expose(view);auto *browser=view.findChild<TranscriptText*>();auto cursor=browser->document()->find("selected");QVERIFY(!cursor.isNull());browser->setTextCursor(cursor);view.setConversation("chat",{message("a","assistant",{text("p","Choose **selected** text.\n\nAnother paragraph.","streaming")})},true);QCOMPARE(browser->textCursor().selectedText(),QString("selected"));}
void TranscriptTest::scrollEndNearBottomRestartsFollowing(){ConversationView view;view.setConversation("chat",longHistory(),true);expose(view,600,360);QTRY_COMPARE(bar(view)->value(),bar(view)->maximum());wheelUp(view);QVERIFY(!view.followsOutput());bar(view)->setValue(bar(view)->maximum()-30);QTRY_VERIFY_WITH_TIMEOUT(view.followsOutput(),500);}
void TranscriptTest::scrollStateSurvivesChatSwitch(){ConversationView view;auto rows=longHistory();view.setConversation("first",rows,false);expose(view,600,360);QTRY_VERIFY(bar(view)->maximum()>1000);bar(view)->setValue(300);QTest::qWait(20);QVERIFY(!view.followsOutput());const int position=bar(view)->value();view.setConversation("second",{message("x","user",{text("x","Short chat")})},false);QTest::qWait(30);view.setConversation("first",rows,false);QTest::qWait(60);QVERIFY(!view.followsOutput());QVERIFY(qAbs(position-bar(view)->value())<=2);}
void TranscriptTest::scrollObserverCanDeleteView(){auto *view=new ConversationView;view->setConversation("first",longHistory(),false);expose(*view,600,360);bar(*view)->setValue(100);QPointer<ConversationView> guarded=view;connect(bar(*view),&QScrollBar::valueChanged,view,[view](int value){if(value>100)delete view;});view->scrollToBottom();QTRY_VERIFY(guarded.isNull());}
void TranscriptTest::scrollObserverCanSwitchConversation(){ConversationView view;view.setConversation("first",longHistory(),false);expose(view,600,360);bar(view)->setValue(100);bool replaced=false;connect(bar(view),&QScrollBar::valueChanged,&view,[&](int value){if(value>100&&!replaced){replaced=true;view.setConversation("replacement",{message("replacement","user",{text("replacement","Replacement chat")})},false);}});view.scrollToBottom();QTRY_VERIFY(replaced);QTest::qWait(30);QCOMPARE(view.conversationId(),QString("replacement"));QCOMPARE(view.toPlainText(),QString("Replacement chat"));QCOMPARE(view.findChildren<TranscriptText*>().size(),1);}
void TranscriptTest::fontScaleIsAppliedOnce(){ConversationView view;view.setConversation("chat",{message("a","assistant",{text("p","```cpp\nint n = 1;\n```"),tool("t","write","output-available")})},false);expose(view);auto *code=view.findChild<CodeEditor*>();QVERIFY(code);QCOMPARE(code->font().pixelSize(),12);QCOMPARE(disclosure(view,"t")->font().pixelSize(),12);auto face=view.font();face.setPixelSize(18);view.setFont(face);QTest::qWait(20);QCOMPARE(code->font().pixelSize(),15);QCOMPARE(disclosure(view,"t")->font().pixelSize(),15);}
void TranscriptTest::emptyReasoningHeaderAppearsBeforeText(){ConversationView view;view.setConversation("chat",{message("a","assistant",{text("r",{},"streaming","reasoning")})},true);expose(view);QPointer<QWidget> original=part(view,"r");QVERIFY(original);auto *header=disclosure(view,"r");QVERIFY(header->isVisible());QCOMPARE(header->text(),QString("Thinking..."));view.setConversation("chat",{message("a","assistant",{text("r","First reasoning delta","streaming","reasoning")})},true);QCOMPARE(part(view,"r"),original.data());QVERIFY(view.toPlainText().contains("First reasoning delta"));}
void TranscriptTest::completedReadBeforeReadyIsCompact(){ConversationView view;auto read=tool("read","read","running");read["input"]=QJsonObject{{"path","w1/probe.txt"}};view.setConversation("chat",{message("read-round","assistant",{read})},true);expose(view);QPointer<QWidget> stable=part(view,"read");QCOMPARE(disclosure(view,"read")->text(),QString("Reading probe.txt..."));read["state"]="output-available";read["output"]="Acknowledgement body from read";view.setConversation("chat",{message("read-round","assistant",{read}),message("answer-round","assistant",{text("answer","READY")})},false);QTest::qWait(20);QCOMPARE(part(view,"read"),stable.data());auto *header=disclosure(view,"read");QCOMPARE(header->text(),QString("Read probe.txt"));QVERIFY(!header->isCheckable());QCOMPARE(header->arrowType(),Qt::NoArrow);QVERIFY(!part(view,"read")->findChild<TranscriptText*>("toolOutput")->isVisible());QVERIFY(part(view,"answer")->isVisible());}
void TranscriptTest::queuedApprovalKeepsOriginalIdentity(){ConversationView view;view.setConversation("first",{message("first-message","assistant",{tool("same","write","approval-requested")})},true);expose(view);QList<QStringList> delivered;connect(&view,&ConversationView::approvalDecided,&view,[&](const QString &chat,const QString &messageId,const QString &partId,const QString &call,bool allowed){delivered.append({chat,messageId,partId,call,allowed?"allow":"deny"});},Qt::QueuedConnection);view.findChild<QPushButton*>("approveTool")->click();view.setConversation("second",{message("second-message","assistant",{tool("same","write","approval-requested")})},true);QTRY_COMPARE(delivered.size(),1);QCOMPARE(delivered[0],QStringList({"first","first-message","same","call-same","allow"}));QVERIFY(view.findChild<QPushButton*>("approveTool")->isEnabled());}
void TranscriptTest::retiredApprovalCannotEmitAgain(){ConversationView view;view.setConversation("chat",{message("a","assistant",{tool("t","write","approval-requested")})},true);expose(view);QSignalSpy decisions(&view,&ConversationView::approvalDecided);auto *approve=view.findChild<QPushButton*>("approveTool");view.setConversation("chat",{message("a","assistant",{tool("t","write","output-available")})},false);QVERIFY(QMetaObject::invokeMethod(approve,"clicked",Q_ARG(bool,false)));QCOMPARE(decisions.size(),0);}

int main(int argc,char **argv){qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);AppFonts::load();QFont font("Geist");font.setPixelSize(14);app.setFont(font);TranscriptTest test;return QTest::qExec(&test,argc,argv);}
#include "test_transcript.moc"
