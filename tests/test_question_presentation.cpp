#include "ui/ConversationView.h"
#include "ui/MarkdownParts.h"
#include "ui/AppStyle.h"
#include "ui/QuestionToolPart.h"
#include <QClipboard>
#include <QDynamicPropertyChangeEvent>
#include <QDir>
#include <QScrollBar>
#include <QTemporaryDir>
#include <functional>
#include <QApplication>
#include <QJsonDocument>
#include <QPushButton>
#include <QLabel>
#include <QPointer>
#include <QtTest>
using namespace ade;
namespace {
QJsonObject form(){return {{"title","Project choices"},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{
 QJsonObject{{"questionId","password"},{"type","input"},{"inputType","password"},{"label","Pass phrase"}},
 QJsonObject{{"questionId","color"},{"type","radio-group"},{"label","**Theme**"},{"options",QJsonArray{QJsonObject{{"value","dark"},{"label","Dark theme"}}}}},
 QJsonObject{{"questionId","agree"},{"type","checkbox"},{"label","Continue?"}}
}}}}}};}
QJsonObject part(QString state="input-available",QJsonObject result={}){QJsonObject out{{"id","question-part"},{"type","tool"},{"toolCallId","question-call"},{"name","askUserQuestions"},{"state",state},{"input",form()},{"inputText",QString::fromUtf8(QJsonDocument(form()).toJson(QJsonDocument::Compact))}};if(!result.isEmpty())out["output"]=QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));return out;}
QJsonArray rows(QJsonObject value){return {QJsonObject{{"id","question-row"},{"role","assistant"},{"parts",QJsonArray{value}}}};}
class Hook final:public QObject {public:std::function<void(QObject*,QEvent*)> callback;bool eventFilter(QObject* object,QEvent* event)override{const QPointer<QObject> target(object);if(callback)callback(object,event);return target.isNull();}};
TranscriptText* answerWidget(ConversationView& view,const QString& id){for(auto* text:view.findChildren<TranscriptText*>("questionAnswerValue"))if(text->parentWidget()->property("questionId")==id)return text;return nullptr;}
bool visibleButton(ConversationView& view,const QString& name){for(auto* b:view.findChildren<QPushButton*>(name))if(b->isVisible())return true;return false;}
}
class QuestionPresentationTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void pendingUsesCompactSourceStatus(){ConversationView view;view.resize(620,600);view.show();view.setConversation("owner",rows(part()),true);QCoreApplication::processEvents();QVERIFY(view.toPlainText().contains("Waiting for response..."));QVERIFY(!visibleButton(view,"copyToolArguments"));QVERIFY(!view.toPlainText().contains("inputType"));}
 void completionFormatsAndMasksWithoutMutatingCanonical(){auto value=part("output-available",{{"answers",QJsonObject{{"password","SYNTHETIC_PRIVATE_ANSWER"},{"color","dark"},{"agree",true}}},{"cancelled",false}});const auto original=value;ConversationView view;view.resize(350,600);view.show();view.setConversation("owner",rows(value),false);QCoreApplication::processEvents();const auto rendered=view.toPlainText();QVERIFY2(!rendered.contains("SYNTHETIC_PRIVATE_ANSWER"),"Question transcript must mask password answers.");QVERIFY(rendered.contains(QString::fromUtf8("••••••")));QVERIFY(rendered.contains("Dark theme"));QVERIFY(rendered.contains("Yes"));QVERIFY(!visibleButton(view,"copyToolResult"));QCOMPARE(value,original);QVERIFY(value["output"].toString().contains("SYNTHETIC_PRIVATE_ANSWER"));}
 void stateLabels_data(){
  QTest::addColumn<QString>("state");QTest::addColumn<QString>("output");QTest::addColumn<QString>("expected");
  QTest::newRow("streaming")<<"input-streaming"<<""<<"Writing up some questions...";
  QTest::newRow("available")<<"input-available"<<""<<"Waiting for response...";
  QTest::newRow("native-running")<<"running"<<""<<"Waiting for response...";
  QTest::newRow("error")<<"output-error"<<"Error: stopped"<<"Questions cancelled (agent stopped)";
  QTest::newRow("native-cancel-without-output")<<"canceled"<<""<<"Questions cancelled (agent stopped)";
  QTest::newRow("success")<<"output-available"<<R"({"answers":{"agree":true},"cancelled":false})"<<"Project choices";
  QTest::newRow("dismissed-partial")<<"canceled"<<R"({"answers":{"agree":false},"cancelled":true,"cancelReason":"user_cancelled"})"<<"User dismissed questions";
  QTest::newRow("stopped-partial")<<"output-available"<<R"({"answers":{"agree":false},"cancelled":true,"cancelReason":"agent_stopped"})"<<"Questions cancelled (agent stopped)";
  QTest::newRow("message-notice")<<"canceled"<<R"({"answers":{"agree":true},"cancelled":false,"notice":"Read following message"})"<<"User sent answers and message";
  QTest::newRow("notice-priority")<<"output-available"<<R"({"answers":{"agree":true},"cancelled":true,"notice":"Read following message"})"<<"User sent answers and message";
  QTest::newRow("empty-object")<<"output-available"<<"{}"<<"Project choices";
  QTest::newRow("malformed-output")<<"output-available"<<"not JSON"<<"Project choices";
 }
 void stateLabels(){QFETCH(QString,state);QFETCH(QString,output);QFETCH(QString,expected);auto value=part(state);if(!output.isEmpty())value["output"]=output;const auto display=questionToolDisplay(value);QCOMPARE(display.summary,expected);if(output.contains("agree")){QCOMPARE(display.answers.size(),1);QCOMPARE(display.answers.first().label,QString("Continue?"));}ConversationView view;view.setConversation("owner",rows(value),state=="running");QVERIFY(view.toPlainText().startsWith(expected));}
 void answerFormatting_data(){
  QTest::addColumn<QJsonObject>("field");QTest::addColumn<QJsonValue>("value");QTest::addColumn<QString>("expected");
  QJsonObject input{{"type","input"}},password{{"type","input"},{"inputType","password"}},radio{{"type","radio-group"},{"options",QJsonArray{QJsonObject{{"value","d"},{"label","Dark"}},QJsonObject{{"value","l"},{"label","Light"}}}}};auto checks=radio;checks["type"]="checkbox-group";
  QTest::newRow("empty")<<input<<QJsonValue("")<<"(empty)";
  QTest::newRow("null")<<input<<QJsonValue(QJsonValue::Null)<<"(empty)";
  QTest::newRow("password")<<password<<QJsonValue("SYNTHETIC_PRIVATE_ANSWER")<<QString::fromUtf8("••••••");
  QTest::newRow("empty-password")<<password<<QJsonValue("")<<"(empty)";
  QTest::newRow("other")<<radio<<QJsonValue("__other__:Custom theme")<<"Custom theme";
  QTest::newRow("empty-other")<<radio<<QJsonValue("__other__:")<<"(empty)";
  QTest::newRow("radio-label")<<radio<<QJsonValue("d")<<"Dark";
  QTest::newRow("radio-unknown")<<radio<<QJsonValue("unknown")<<"unknown";
  QTest::newRow("checks-labels")<<checks<<QJsonValue(QJsonArray{"d","l"})<<"Dark, Light";
  QTest::newRow("checks-empty")<<checks<<QJsonValue(QJsonArray{})<<"(none)";
  QTest::newRow("true")<<input<<QJsonValue(true)<<"Yes";
  QTest::newRow("false")<<input<<QJsonValue(false)<<"No";
  QTest::newRow("number")<<input<<QJsonValue(12.5)<<"12.5";
  QTest::newRow("unknown-object")<<input<<QJsonValue(QJsonObject{{"not","shown as JSON"}})<<"[object Object]";
 }
 void answerFormatting(){QFETCH(QJsonObject,field);QFETCH(QJsonValue,value);QFETCH(QString,expected);field["questionId"]="q";field["label"]="Answer";auto input=form();input["steps"]=QJsonArray{QJsonObject{{"fields",QJsonArray{field}}}};auto tool=part("output-available",{{"answers",QJsonObject{{"q",value}}}});tool["input"]=input;const auto display=questionToolDisplay(tool);QCOMPARE(display.answers.size(),1);QCOMPARE(display.answers.first().value,expected);}
 void passwordSelectionCopiesOnlyMask(){ConversationView view;view.resize(620,600);view.show();view.setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"password","SYNTHETIC_PRIVATE_ANSWER"}}}})),false);auto* value=answerWidget(view,"password");QVERIFY(value);value->selectAll();value->copy();QCOMPARE(QApplication::clipboard()->text(),QString::fromUtf8("••••••"));for(auto* button:view.findChildren<QPushButton*>("copyToolResult")){QVERIFY(!button->isVisible());button->click();QVERIFY(!QApplication::clipboard()->text().contains("SYNTHETIC_PRIVATE_ANSWER"));}}
 void partialCompletionKeepsWidgetAndSelection(){ConversationView view;view.resize(620,600);view.show();view.setConversation("owner",rows(part("input-streaming")),true);auto* card=view.findChild<QuestionToolPart*>();QVERIFY(card);auto completed=part("output-available",{{"answers",QJsonObject{{"color","dark"},{"agree",false}}}});view.setConversation("owner",rows(completed),false);QCOMPARE(view.findChild<QuestionToolPart*>(),card);auto* value=answerWidget(view,"color");QVERIFY(value);auto cursor=value->textCursor();cursor.setPosition(0);cursor.setPosition(4,QTextCursor::KeepAnchor);value->setTextCursor(cursor);completed["output"]=QString::fromUtf8(QJsonDocument(QJsonObject{{"answers",QJsonObject{{"color","dark"},{"agree",true}}}}).toJson(QJsonDocument::Compact));view.setConversation("owner",rows(completed),false);QCOMPARE(answerWidget(view,"color"),value);QCOMPARE(value->textCursor().selectedText(),QString("Dark"));auto grouped=rows(completed);grouped.append(QJsonObject{{"id","next-round"},{"role","assistant"},{"parts",QJsonArray{QJsonObject{{"id","next-text"},{"type","text"},{"state","done"},{"text","Follow-up"}}}}});view.setConversation("owner",grouped,false);QCOMPARE(view.findChild<QuestionToolPart*>(),card);QCOMPARE(answerWidget(view,"color"),value);}
 void approvalControlsStayScopedAndVisible(){ConversationView view;view.resize(620,600);view.show();view.setConversation("owner",rows(part("approval-requested")),true);auto* approve=view.findChild<QPushButton*>("approveTool");QVERIFY(approve);QTRY_VERIFY(approve->isVisible());QVERIFY(approve->isEnabled());QSignalSpy decisions(&view,&ConversationView::approvalDecided);approve->click();QCOMPARE(decisions.size(),1);QCOMPARE(decisions.first()[0].toString(),QString("owner"));QCOMPARE(decisions.first()[1].toString(),QString("question-row"));QCOMPARE(decisions.first()[2].toString(),QString("question-part"));QCOMPARE(decisions.first()[3].toString(),QString("question-call"));approve->click();QCOMPARE(decisions.size(),1);view.setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"agree",true}}}})),false);QVERIFY(!approve->isVisible());QVERIFY(view.toPlainText().contains("Yes"));}
 void similarlyNamedToolKeepsGenericPresentation(){auto value=part();value["name"]="custom.askUserQuestions";ConversationView view;view.resize(620,600);view.show();view.setConversation("owner",rows(value),true);QTRY_VERIFY(visibleButton(view,"copyToolArguments"));QVERIFY(view.toPlainText().contains("custom.askUserQuestions"));}
 void summaryObserverMayReplaceOrDestroy_data(){QTest::addColumn<bool>("destroy");QTest::newRow("delete-view")<<true;QTest::newRow("replace-chat")<<false;}
 void summaryObserverMayReplaceOrDestroy(){QFETCH(bool,destroy);QPointer<ConversationView> view=new ConversationView;view->setConversation("owner",rows(part()),true);auto* card=view->findChild<QuestionToolPart*>();QVERIFY(card);Hook hook;bool fired=false;hook.callback=[&](QObject*,QEvent* event){if(fired||event->type()!=QEvent::DynamicPropertyChange||static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()!="questionSummary")return;fired=true;if(destroy)delete view.data();else view->setConversation("other",{QJsonObject{{"id","other-row"},{"role","user"},{"parts",QJsonArray{QJsonObject{{"id","other-text"},{"type","text"},{"state","done"},{"text","New context"}}}}}},false);};card->installEventFilter(&hook);view->setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"agree",true}}}})),false);QVERIFY(fired);if(destroy)QVERIFY(!view);else {QVERIFY(view);QCOMPARE(view->conversationId(),QString("other"));QCOMPARE(view->toPlainText(),QString("New context"));delete view.data();}}
 void sameChatReentryPreservesNewerQuestion(){ConversationView view;view.setConversation("owner",rows(part()),true);auto* card=view.findChild<QuestionToolPart*>();QVERIFY(card);Hook hook;bool fired=false;auto replacement=part("output-available",{{"answers",QJsonObject{{"agree",false}}}});auto input=form();input["title"]="Newer summary";replacement["input"]=input;hook.callback=[&](QObject*,QEvent* event){if(fired||event->type()!=QEvent::DynamicPropertyChange||static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()!="questionSummary")return;fired=true;view.setConversation("owner",rows(replacement),false);};card->installEventFilter(&hook);view.setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"agree",true}}}})),false);QVERIFY(fired);QCOMPARE(view.findChild<QLabel*>("questionSummary")->text(),QString("Newer summary"));QCOMPARE(answerWidget(view,"agree")->toPlainText(),QString("No"));QVERIFY(!view.toPlainText().contains("Yes"));}
 void labelLinkRetainsRawOwnerAndDeniedResources(){auto tool=part("output-available",{{"answers",QJsonObject{{"color","dark"}}}});auto input=form();auto steps=input["steps"].toArray();auto step=steps[0].toObject();auto fields=step["fields"].toArray();auto color=fields[1].toObject();color["label"]="[Theme](https://example.invalid/theme) ![denied](file:///missing-fixture.png)";fields[1]=color;step["fields"]=fields;steps[0]=step;input["steps"]=steps;tool["input"]=input;ConversationView view;view.setConversation("owner",rows(tool),false);auto* label=view.findChild<TranscriptText*>("questionAnswerLabel");QVERIFY(label);QSignalSpy links(&view,&ConversationView::scopedLinkActivated),images(&view,&ConversationView::imageRequested);QVERIFY(QMetaObject::invokeMethod(label,"anchorClicked",Q_ARG(QUrl,QUrl("https://example.invalid/theme"))));QCOMPARE(links.size(),1);QCOMPARE(links.first()[0].toString(),QString("owner"));QCOMPARE(links.first()[1].toString(),QString("question-row"));QCOMPARE(links.first()[2].toString(),QString("question-part"));QVERIFY(!label->document()->resource(QTextDocument::ImageResource,QUrl("file:///missing-fixture.png")).isValid());QCoreApplication::processEvents();QCOMPARE(images.size(),0);}
 void boundsPreserveUnicodeAndDoNotDumpMalformedJson(){auto value=part("output-available",{{"answers",QJsonObject{{"unknown",QString(65535,'x')+QString::fromUtf8("😀")}}}});const auto display=questionToolDisplay(value);QVERIFY(display.truncated);QCOMPARE(display.answers.size(),1);QVERIFY(!display.answers.first().value.back().isHighSurrogate());QVERIFY(questionToolPlainText(value).size()<66000);value["output"]=QString(2*1024*1024+1,'x');QCOMPARE(questionToolPlainText(value),QString("Project choices"));}

 void fontScaleTracksParent(){ConversationView view;auto f=view.font();f.setPixelSize(13);view.setFont(f);view.setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"color","dark"}}}})),false);auto* summary=view.findChild<QLabel*>("questionSummary");auto* answer=answerWidget(view,"color");QVERIFY(summary);QVERIFY(answer);QCOMPARE(summary->height(),24);f.setPixelSize(26);view.setFont(f);QCOMPARE(summary->height(),48);QCOMPARE(summary->font().pixelSize(),24);QCOMPARE(answer->font().pixelSize(),28);}
 void questionRenders_data(){QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");QTest::newRow("light-620")<<false<<620;QTest::newRow("dark-620")<<true<<620;QTest::newRow("light-350")<<false<<350;QTest::newRow("dark-350")<<true<<350;}
 void questionRenders(){
  QFETCH(bool,dark);QFETCH(int,width);initializeAppStyle(*qApp);auto palette=qApp->palette();palette.setColor(QPalette::Window,QColor(dark?"#21242b":"#fdfdfd"));palette.setColor(QPalette::Base,palette.color(QPalette::Window));palette.setColor(QPalette::Text,QColor(dark?"#e8e8ed":"#292a2d"));palette.setColor(QPalette::WindowText,palette.color(QPalette::Text));palette.setColor(QPalette::Button,QColor(dark?"#2b303a":"#f2f2f2"));palette.setColor(QPalette::ButtonText,palette.color(QPalette::Text));qApp->setPalette(palette);
  if(dark){auto style=qApp->styleSheet();for(const auto& pair:QList<QPair<QString,QString>>{{"#292a2d","#e8e8ed"},{"#fdfdfd","#21242b"},{"#fafafa","#252a33"},{"#f2f2f2","#2b303a"},{"#e6e6e6","#3d424b"},{"#e4e6e8","#3d424b"},{"#fff","#252a33"}})style.replace(pair.first,pair.second,Qt::CaseInsensitive);qApp->setStyleSheet(style);}
  ConversationView view;view.setPalette(palette);view.setAutoFillBackground(true);view.resize(width,420);view.show();view.setConversation("owner",rows(part()),true);QTRY_VERIFY(view.findChild<QLabel*>("questionSummary")->isVisible());QVERIFY(view.findChild<QLabel*>("questionSummary")->height()<=26);QVERIFY(!visibleButton(view,"copyToolArguments"));const auto directory=qEnvironmentVariable("ADE_QUESTION_RENDER_DIRECTORY");if(!directory.isEmpty()){QVERIFY(QDir().mkpath(directory));QTest::qWait(50);QVERIFY(view.grab().save(QDir(directory).filePath(QString("question-%1-%2-pending.png").arg(dark?"dark":"light").arg(width))));}
  view.setConversation("owner",rows(part("output-available",{{"answers",QJsonObject{{"password","SYNTHETIC_PRIVATE_ANSWER"},{"color","dark"},{"agree",true}}}})),false);QTRY_VERIFY(answerWidget(view,"password")->isVisible());QTest::qWait(30);for(auto* answer:view.findChildren<TranscriptText*>("questionAnswerValue")){QVERIFY(view.rect().contains(QRect(answer->mapTo(&view,QPoint{}),answer->size())));QCOMPARE(answer->horizontalScrollBar()->maximum(),0);}QVERIFY(view.toPlainText().contains(QString::fromUtf8("••••••")));if(!directory.isEmpty())QVERIFY(view.grab().save(QDir(directory).filePath(QString("question-%1-%2-completed.png").arg(dark?"dark":"light").arg(width))));
 }

};
QTEST_MAIN(QuestionPresentationTest)
#include "test_question_presentation.moc"
