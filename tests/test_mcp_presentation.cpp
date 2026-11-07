#include "core/AgentEngine.h"
#include "core/QuestionForm.h"
#include "harness/QuestionMcpStdio.h"
#include "harness/AcpToolPresentation.h"
#include "harness/AcpHarnessSession.h"
#include "core/AgentPresentation.h"
#include <QPushButton>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPalette>
#include "ui/ConversationView.h"
#include "ui/QuestionPanel.h"
#include "ui/AppStyle.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
void configure(AgentEngine& engine,const QString& workspace,bool bridge=true){ProviderConfig p;p.id="fixture";p.name="Fixture";p.kind="opencode";p.protocol="opencode-acp";p.model="fixture";p.harnessExecutable=QDir(QCoreApplication::applicationDirPath()).filePath("acp_mcp_fixture.exe");engine.setProvider(p);engine.setHarnessContext("owner",workspace);engine.setQuestionToolEnabled(bridge);}
QJsonArray tools(const QJsonArray& rows){QJsonArray out;for(const auto& row:rows)for(const auto& value:row.toObject()["parts"].toArray())if(value.toObject()["type"]=="tool")out.append(value);return out;}
int cards(ConversationView& view){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);int count=0;for(const auto* widget:view.findChildren<QWidget*>())if(widget->property("partType")=="tool"&&!widget->isHidden())++count;return count;}
QJsonObject prompt(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readLine()).object();}
}
class McpPresentationTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void cleanup(){qunsetenv("ADE_MCP_SCENARIO");qunsetenv("ADE_MCP_PROMPT_LOG");qunsetenv("ADE_MCP_DECISION_LOG");}
 void realOuterWrapperAndNativeFormRenderOneCard(){
  qputenv("ADE_MCP_SCENARIO","wrapper");QTemporaryDir dir;AgentEngine engine;configure(engine,dir.path());ConversationView view;view.resize(620,700);view.show();QuestionPanel panel;
  connect(&engine,&AgentEngine::displayChanged,&view,[&]{view.setConversation("owner",engine.displayMessages(),engine.isBusy());});connect(&engine,&AgentEngine::questionChanged,&panel,[&]{panel.setQuestion("owner",engine.pendingQuestion());});
  QSignalSpy done(&engine,&AgentEngine::finished);QVERIFY(engine.submitWithAttachments("Ask one structured question",{}));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),5000);QTRY_COMPARE(tools(engine.displayMessages()).size(),2);QVERIFY(!panel.token().isEmpty());QTRY_COMPARE(cards(view),1);
  const auto token=engine.pendingQuestion()["id"].toString();QVERIFY(engine.submitQuestionStep(token,0,{{"answer","accepted"}}));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(engine.lastTurnSucceeded());QCOMPARE(cards(view),1);QCOMPARE(tools(engine.displayMessages()).size(),2);QVERIFY(QJsonDocument(engine.messages()).toJson().contains("question-wrapper"));QVERIFY(QJsonDocument(engine.messages()).toJson().contains("wrapper settled"));
  AgentEngine restored;configure(restored,dir.path());restored.setMessages(engine.messages());view.setConversation("owner",restored.displayMessages(),false);QCOMPARE(cards(view),1);
 }
 void enabledBridgePrependsSourcePreference(){qputenv("ADE_MCP_SCENARIO","hold-after-answer");QTemporaryDir dir;const auto log=dir.filePath("prompt.jsonl");qputenv("ADE_MCP_PROMPT_LOG",log.toUtf8());AgentEngine engine;configure(engine,dir.path());QVERIFY(engine.submitWithAttachments("Fixture prompt",{}));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),5000);const auto captured=prompt(log);const auto bytes=QJsonDocument(captured).toJson();QCOMPARE(bytes.count("<stagewise-agent-instructions>"),1);QCOMPARE(captured["prompt"].toArray().first().toObject()["text"].toString(),QString("<stagewise-agent-instructions>\nFor structured user input, use the MCP tool `stagewise_request_user_input`. Do not use a provider-native question tool or ask the structured question in plain text. This tool is never used for command or tool approval: invoke the command or tool directly and let the host show its approval UI.\n</stagewise-agent-instructions>"));QVERIFY(bytes.contains("For structured user input, use the MCP tool"));QVERIFY(bytes.contains("never used for command or tool approval"));engine.cancel();QTRY_VERIFY(!engine.isBusy());}
 void disabledBridgeDoesNotAdvertisePreference(){QTemporaryDir dir;const auto log=dir.filePath("prompt.jsonl");qputenv("ADE_MCP_PROMPT_LOG",log.toUtf8());AgentEngine engine;configure(engine,dir.path(),false);QSignalSpy done(&engine,&AgentEngine::finished);QVERIFY(engine.submitWithAttachments("No bridge",{}));QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(engine.pendingQuestion().isEmpty());QVERIFY(!QJsonDocument(prompt(log)).toJson().contains("stagewise_request_user_input"));}
 void exactSourceWrapperDetection_data(){
  QTest::addColumn<QJsonObject>("tool");QTest::addColumn<bool>("hidden");
  QTest::newRow("native-name")<<QJsonObject{{"name","request_user_input"}}<<true;
  QTest::newRow("qualified-name")<<QJsonObject{{"name","mcp.stagewise_request_user_input.v2"}}<<true;
  QTest::newRow("title-substring")<<QJsonObject{{"title","Running stagewise_request_user_input tool"}}<<true;
  QTest::newRow("input-tool")<<QJsonObject{{"rawInput",QJsonObject{{"tool","stagewise_request_user_input"}}}}<<true;
  QTest::newRow("metadata-name")<<QJsonObject{{"_meta",QJsonObject{{"vendor",QJsonObject{{"toolName","request_user_input"}}}}}}<<true;
  QTest::newRow("ordinary-native-prefix")<<QJsonObject{{"name","request_user_input_extra"}}<<false;
  QTest::newRow("ordinary-native-suffix")<<QJsonObject{{"name","other_request_user_input"}}<<false;
  QTest::newRow("ordinary-native-title")<<QJsonObject{{"title","request_user_input"}}<<false;
  QTest::newRow("case-sensitive")<<QJsonObject{{"name","STAGEWISE_REQUEST_USER_INPUT"}}<<false;
  QTest::newRow("input-not-substring")<<QJsonObject{{"rawInput",QJsonObject{{"tool","x_stagewise_request_user_input"}}}}<<false;
  QTest::newRow("wrong-types")<<QJsonObject{{"name",42},{"title",QJsonArray{"stagewise_request_user_input"}},{"rawInput",QJsonObject{{"tool",QJsonObject{{"name","stagewise_request_user_input"}}}}}}<<false;
 }
 void exactSourceWrapperDetection(){QFETCH(QJsonObject,tool);QFETCH(bool,hidden);QCOMPARE(AcpToolPresentation::isQuestionWrapper(tool),hidden);const auto mapped=AcpToolPresentation::map(tool,{});QVERIFY(AcpToolPresentation::valid(mapped));QCOMPARE(mapped["questionWrapper"].toBool(),hidden);}
 void approvalAndMalformedMetadataNeverSuppress(){
  const auto mapped=AcpToolPresentation::map({{"name","request_user_input"}},{});
  QJsonObject part{{"type","tool"},{"state","input-available"},{"acpPresentation",mapped}};QVERIFY(AcpToolPresentation::suppressQuestionWrapperPart(part));
  part["state"]="approval-requested";QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(part));part["state"]="output-available";part["approvalSeen"]=true;QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(part));
  part.remove("approvalSeen");auto invalid=mapped;invalid["questionWrapper"]="true";part["acpPresentation"]=invalid;QVERIFY(!AcpToolPresentation::valid(invalid));QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(part));
  part["acpPresentation"]=mapped;part["type"]="text";QVERIFY(!AcpToolPresentation::suppressQuestionWrapperPart(part));
 }
 void realLateWrapperMetadataHidesOnlyOrdinaryCard(){
  qputenv("ADE_MCP_SCENARIO","wrapper-late");QTemporaryDir dir;AgentEngine engine;configure(engine,dir.path());ConversationView view;bool ordinarySeen=false;QString originalId;
  connect(&engine,&AgentEngine::displayChanged,&view,[&]{const auto parts=tools(engine.displayMessages());for(const auto& value:parts){const auto part=value.toObject();if(part["toolCallId"].toString().endsWith("/question-wrapper")){if(originalId.isEmpty())originalId=part["id"].toString();if(!part["acpPresentation"].toObject()["questionWrapper"].toBool())ordinarySeen=true;}}view.setConversation("owner",engine.displayMessages(),engine.isBusy());});
  QVERIFY(engine.submitWithAttachments("Late metadata",{}));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),5000);QTRY_COMPARE(cards(view),1);QVERIFY(ordinarySeen);QVERIFY(!originalId.isEmpty());const auto parts=tools(engine.displayMessages());QCOMPARE(parts.size(),2);QCOMPARE(parts.first().toObject()["id"].toString(),originalId);engine.cancel();QTRY_VERIFY(!engine.isBusy());
 }
 void realApprovalFirstRetainsVisibleTerminalCard_data(){QTest::addColumn<bool>("allow");QTest::newRow("allow")<<true;QTest::newRow("deny")<<false;}
 void realApprovalFirstRetainsVisibleTerminalCard(){
  QFETCH(bool,allow);qputenv("ADE_MCP_SCENARIO","wrapper-approval-first");QTemporaryDir dir;const auto log=dir.filePath("decisions.txt");qputenv("ADE_MCP_DECISION_LOG",log.toUtf8());AgentEngine engine;configure(engine,dir.path());ConversationView view;view.resize(620,700);view.show();
  connect(&engine,&AgentEngine::displayChanged,&view,[&]{view.setConversation("owner",engine.displayMessages(),engine.isBusy());});
  connect(&view,&ConversationView::approvalDecided,&engine,[&](const QString& chat,const QString&,const QString&,const QString& call,bool decision){if(chat=="owner")engine.approveTool(call,decision);});
  QSignalSpy approval(&engine,&AgentEngine::approvalRequired),done(&engine,&AgentEngine::finished);QVERIFY(engine.submitWithAttachments("Approval stays authoritative",{}));QTRY_COMPARE_WITH_TIMEOUT(approval.size(),1,5000);
  QTRY_VERIFY(tools(engine.displayMessages()).first().toObject()["acpPresentation"].toObject()["questionWrapper"].toBool());QCOMPARE(cards(view),1);const auto before=tools(engine.displayMessages()).first().toObject();QCOMPARE(before["state"].toString(),QString("approval-requested"));QCOMPARE(before["approvalSeen"],QJsonValue(true));
  engine.approveTool("question-wrapper",true);QVERIFY(!QFile::exists(log));QVERIFY(engine.isBusy());auto* button=view.findChild<QPushButton*>(allow?"approveTool":"denyTool");QVERIFY(button);QVERIFY(button->isEnabled());button->click();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QCOMPARE(cards(view),1);
  const auto terminal=tools(engine.displayMessages()).first().toObject();QCOMPARE(terminal["id"],before["id"]);QCOMPARE(terminal["approvalSeen"],QJsonValue(true));QVERIFY((QStringList{"output-available","output-denied","output-error"}.contains(terminal["state"].toString())));QFile decisions(log);QVERIFY(decisions.open(QIODevice::ReadOnly));QCOMPARE(decisions.readAll(),allow?QByteArray("yes\n"):QByteArray("no\n"));
  AgentEngine restored;configure(restored,dir.path());restored.setMessages(engine.messages());view.setConversation("owner",restored.displayMessages(),false);QCOMPARE(cards(view),1);QVERIFY(!view.findChild<QPushButton*>("approveTool")->isEnabled());
 }
 void cancellationAndChatSwitchKeepForeignViewUnchanged(){
  qputenv("ADE_MCP_SCENARIO","wrapper");QTemporaryDir dir;AgentEngine engine;configure(engine,dir.path());ConversationView view;QString current="owner";
  connect(&engine,&AgentEngine::displayChanged,&view,[&]{if(current=="owner")view.setConversation(current,engine.displayMessages(),engine.isBusy());});
  QVERIFY(engine.submitWithAttachments("Ask",{}));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),5000);const auto token=engine.pendingQuestion()["id"].toString();current="other";view.setConversation(current,{{QJsonObject{{"id","other-row"},{"role","user"},{"parts",QJsonArray{QJsonObject{{"id","other-part"},{"type","text"},{"text","Foreign draft"},{"state","done"}}}}}}},false);const auto text=view.toPlainText();engine.cancel();QTRY_VERIFY(!engine.isBusy());QCOMPARE(view.conversationId(),current);QCOMPARE(view.toPlainText(),text);QCOMPARE(cards(view),0);QVERIFY(!engine.submitQuestionStep(token,0,{{"answer","stale"}}));
 }
 void cancellationBeforePromptDoesNotSendPreference(){
  QTemporaryDir dir;const auto log=dir.filePath("prompt.jsonl");qputenv("ADE_MCP_PROMPT_LOG",log.toUtf8());AcpHarnessOptions options;options.executable=QDir(QCoreApplication::applicationDirPath()).filePath("acp_mcp_fixture.exe");options.providerId="fixture";options.model="fixture";options.workspace=dir.path();options.questionMcpExecutable=QCoreApplication::applicationFilePath();AcpHarnessSession session;QSignalSpy done(&session,&AcpHarnessSession::finished);QVERIFY(session.startTurn(options,{QJsonObject{{"type","text"},{"text","Canceled before prompt"}}}));session.interrupt();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QCOMPARE(done.first()[0].toJsonObject()["status"],QJsonValue("interrupted"));QVERIFY(!QFile::exists(log));
 }
 void nativeQuestionLayout_data(){QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");QTest::newRow("light-620")<<false<<620;QTest::newRow("dark-620")<<true<<620;QTest::newRow("light-350")<<false<<350;QTest::newRow("dark-350")<<true<<350;}
 void nativeQuestionLayout(){
  QFETCH(bool,dark);QFETCH(int,width);initializeAppStyle(*qApp);
  auto palette=qApp->palette();palette.setColor(QPalette::Window,QColor(dark?"#21242b":"#fdfdfd"));palette.setColor(QPalette::Base,QColor(dark?"#21242b":"#fdfdfd"));palette.setColor(QPalette::Text,QColor(dark?"#e8e8ed":"#292a2d"));palette.setColor(QPalette::WindowText,palette.color(QPalette::Text));palette.setColor(QPalette::Button,QColor(dark?"#2b303a":"#f2f2f2"));palette.setColor(QPalette::ButtonText,palette.color(QPalette::Text));qApp->setPalette(palette);
  if(dark){auto style=qApp->styleSheet();const QList<QPair<QString,QString>> colors={{"#292a2d","#e8e8ed"},{"#eff4fa","#1d222c"},{"#fdfdfd","#21242b"},{"#fcfcfc","#21242b"},{"#fafafa","#252a33"},{"#f2f2f2","#2b303a"},{"#e6e6e6","#3d424b"},{"#e4e6e8","#3d424b"},{"#64676c","#c4c8d0"},{"#6a6d72","#bec3cb"},{"#fff","#252a33"}};for(const auto& pair:colors)style.replace(pair.first,pair.second,Qt::CaseInsensitive);qApp->setStyleSheet(style);}
  qputenv("ADE_MCP_SCENARIO","wrapper");QTemporaryDir dir;AgentEngine engine;configure(engine,dir.path());QWidget surface;surface.setObjectName("chatSurface");surface.setAutoFillBackground(true);auto* layout=new QVBoxLayout(&surface);layout->setContentsMargins(0,0,0,0);auto* view=new ConversationView(&surface);auto* panel=new QuestionPanel(&surface);layout->addWidget(view,1);layout->addWidget(panel);surface.resize(width,700);surface.show();
  connect(&engine,&AgentEngine::displayChanged,view,[&]{view->setConversation("owner",engine.displayMessages(),engine.isBusy());});connect(&engine,&AgentEngine::questionChanged,panel,[&]{panel->setQuestion("owner",engine.pendingQuestion());});connect(panel,&QuestionPanel::submitStep,&engine,[&](const QString& chat,const QString& token,int step,const QJsonObject& answer){if(chat=="owner")engine.submitQuestionStep(token,step,answer);});
  QSignalSpy done(&engine,&AgentEngine::finished);QVERIFY(engine.submitWithAttachments("Ask for a short project name.",{}));QTRY_VERIFY_WITH_TIMEOUT(!engine.pendingQuestion().isEmpty(),5000);QTRY_COMPARE(cards(*view),1);QTRY_VERIFY(panel->isVisible());auto* input=panel->findChild<QLineEdit*>("answer_answer");QVERIFY(input);QTRY_VERIFY(input->isVisible());QVERIFY(surface.rect().contains(QRect(input->mapTo(&surface,QPoint{}),input->size())));QVERIFY(surface.width()<=width);
  const auto capture=qEnvironmentVariable("ADE_MCP_RENDER_DIRECTORY");if(!capture.isEmpty()){QVERIFY(QDir().mkpath(capture));QTest::qWait(80);QVERIFY(surface.grab().save(QDir(capture).filePath(QString("question-%1-%2-pending.png").arg(dark?"dark":"light").arg(width))));}
  input->setText("Native test");auto* submit=panel->findChild<QPushButton*>("questionSubmit");QVERIFY(submit);QVERIFY(submit->isVisible());QVERIFY(surface.rect().contains(QRect(submit->mapTo(&surface,QPoint{}),submit->size())));submit->click();QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY(engine.lastTurnSucceeded());QTRY_COMPARE(cards(*view),1);QTRY_VERIFY(panel->isHidden());
  if(!capture.isEmpty()){QTest::qWait(80);QVERIFY(surface.grab().save(QDir(capture).filePath(QString("question-%1-%2-completed.png").arg(dark?"dark":"light").arg(width))));}
 }


};
int main(int argc,char** argv){if(argc==2&&QByteArray(argv[1])=="--question-mcp-stdio")return runQuestionMcpStdio(argc,argv,QuestionForm::toolDefinition()["function"].toObject()["parameters"].toObject());QApplication app(argc,argv);McpPresentationTest tests;return QTest::qExec(&tests,argc,argv);}
#include "test_mcp_presentation.moc"
