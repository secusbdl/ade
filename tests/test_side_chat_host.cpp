#include "ui/MainWindow.h"
#include "ui/SideChatPane.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/MarkdownParts.h"
#include "ui/QuestionPanel.h"
#include "ui/MessageQueuePanel.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/AppStyle.h"
#include "ui/WorkbenchSplitter.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "attachments/AttachmentStore.h"
#include <QApplication>
#include <QBuffer>
#include <QAction>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
QByteArray chunk(const QJsonObject& delta,const QString& reason={}){return "data: "+QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"delta",delta},{"finish_reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}}}}}).toJson(QJsonDocument::Compact)+"\n\n";}
class Provider:public QObject{
public:
QTcpServer server;QList<QJsonObject> requests;QList<QPointer<QTcpSocket>> sockets;
Provider(){server.listen(QHostAddress::LocalHost);connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QTcpSocket::readyRead,this,[this,socket]{if(socket->property("done").toBool()){socket->readAll();return;}const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;int length=0;for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();if(bytes.size()<end+4+length)return;socket->setProperty("done",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());sockets.append(socket);socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");});}});}
bool seed(const QString& data,const QString& workspace={}){ProviderConfig config;config.id="fixture";config.name="Fixture";config.model="fixture-model";config.apiKey="side-chat-test-token";config.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());if(!ProviderStore::save(data+"/providers.json",{config}))return false;QList<Conversation> chats;for(const auto& id:QStringList{"parent","other"}){Conversation chat;chat.id=id;chat.title=id;chat.titleLocked=true;chat.autoTitleAttempted=true;chat.workspace=workspace;chat.providerId=config.id;chat.model=config.model;chat.messages={QJsonObject{{"role","user"},{"content","Inherited question"}},QJsonObject{{"role","assistant"},{"content","Inherited answer"}}};chats.append(chat);}return SessionStore(data).save(chats);}
void send(int index,const QJsonObject& delta){if(auto socket=sockets.value(index)){socket->write(chunk(delta));socket->flush();}}
void finish(int index,const QString& reason="stop"){if(auto socket=sockets.value(index)){socket->write(chunk({},reason)+"data: [DONE]\n\n");socket->disconnectFromHost();}}
void question(int index){const QJsonObject form{{"title","Side question"},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{QJsonObject{{"questionId","name"},{"type","input"},{"label","Name"},{"required",true}}}}}}}};send(index,{{"tool_calls",QJsonArray{QJsonObject{{"index",0},{"id","question-call"},{"type","function"},{"function",QJsonObject{{"name","askUserQuestions"},{"arguments",QString::fromUtf8(QJsonDocument(form).toJson(QJsonDocument::Compact))}}}}}}});finish(index,"tool_calls");}
};
void expose(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,750);window.show();}
SideChatPane* create(MainWindow& window){auto* button=window.findChild<QToolButton*>("openSideChatButton");if(!button)return nullptr;button->click();const auto panes=window.findChildren<SideChatPane*>();for(auto* pane:panes)if(!pane->chatId().isEmpty())return pane;return nullptr;}
Conversation saved(const QString& data,const QString& id){for(const auto& chat:SessionStore(data).load())if(chat.id==id)return chat;return {};}
AgentEngine* engine(MainWindow& window,const QString& id){for(auto* host:window.findChildren<AgentToolHost*>())if(host->agentId()==id)return qobject_cast<AgentEngine*>(host->parent());return nullptr;}
bool listed(MainWindow& window,const QString& id){auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id)return true;return false;}
bool action(MainWindow& window,const QString& id,const QString& name){auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){list->scrollToItem(list->item(i));const auto point=list->visualItemRect(list->item(i)).center();emit list->customContextMenuRequested(point);for(auto* menu:window.findChildren<QMenu*>("conversationMenu"))if(auto* item=menu->findChild<QAction*>(name)){item->trigger();menu->close();return true;}}return false;}
bool put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
void capture(MainWindow& window,const QString& name){const auto directory=qEnvironmentVariable("ADE_SIDE_RENDER_DIRECTORY");if(directory.isEmpty())return;QDir().mkpath(directory);window.grab().save(QDir(directory).filePath(name+".png"));}
QString backgroundMismatch(MainWindow& window,SideChatPane* pane,const QColor& expected){
    // Render the whole host hierarchy: grabbing the pane by itself can supply
    // a different backing background and conceal transparent-child defects.
    const auto image=window.grab().toImage();const auto ratio=image.devicePixelRatio();
    auto check=[&](const QString& name,QWidget* widget,const QPoint& local){
        if(!widget)return name+" widget is missing";
        const auto point=widget->mapTo(&window,local);
        if(!window.rect().contains(point))return name+" sample is outside the visible window";
        const auto actual=image.pixelColor(qRound(point.x()*ratio),qRound(point.y()*ratio));
        return actual==expected?QString():name+" background: "+actual.name()+"; expected "+expected.name();
    };
    auto* header=pane->findChild<QToolButton*>("sideChatChangesButton");
    auto* transcript=pane->transcript()->findChild<QScrollArea*>("transcriptScroll");
    auto* controls=pane->findChild<QScrollArea*>("sideChatControls");
    auto* fields=pane->questions()->findChild<QScrollArea*>("questionFields");
    if(!header||!transcript||!controls||!fields)return "Expected visible chat surfaces are missing";
    // These are blank pixels inside the actual header and child viewports,
    // not the outer pane corner that can hide an inherited blue child fill.
    for(const auto& mismatch:{check("header",header,{3,3}),
                             check("transcript",transcript->viewport(),{transcript->viewport()->width()/2,transcript->viewport()->height()-4}),
                             check("controls",controls->viewport(),{3,controls->viewport()->height()/2}),
                             check("question body",fields->viewport(),{3,fields->viewport()->height()-3})})
        if(!mismatch.isEmpty())return mismatch;
    return {};
}
}
class SideChatHostTest:public QObject{
Q_OBJECT
QTemporaryDir home;QByteArray originalHome;
private slots:
void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));originalHome=qgetenv("USERPROFILE");qputenv("USERPROFILE",home.path().toUtf8());}
void cleanupTestCase(){qputenv("USERPROFILE",originalHome);}
void forkIsIndependentAndHiddenThenPromotes(){QTemporaryDir data,workspace;Provider provider;QVERIFY(provider.seed(data.path(),workspace.path()));MainWindow window(data.path());expose(window);window.findChild<Composer*>("composerInput")->setPlainText("Parent draft");auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();QCOMPARE(window.currentSessionId(),QString("parent"));QVERIFY(!listed(window,id));const auto child=saved(data.path(),id);QCOMPARE(child.sideChatParentId,QString("parent"));QCOMPARE(child.messages.size(),2);QVERIFY(child.draft.isEmpty());QVERIFY(child.queuedMessages.isEmpty());QVERIFY(pane->transcript()->toPlainText().contains("Inherited answer"));pane->composer()->setPlainText("Side draft");QTRY_COMPARE(saved(data.path(),id).draft,QString("Side draft"));QCOMPARE(window.findChild<Composer*>("composerInput")->toPlainText(),QString("Parent draft"));pane->findChild<QToolButton*>("promoteSideChatButton")->click();QCOMPARE(window.currentSessionId(),id);QVERIFY(saved(data.path(),id).sideChatParentId.isEmpty());QVERIFY(listed(window,id));QCOMPARE(window.findChild<Composer*>("composerInput")->toPlainText(),QString("Side draft"));}
void sideStreamsAndQueuesWithoutChangingParent(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("First child prompt");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);provider.send(0,{{"reasoning_content","Child reasoning"}});provider.send(0,{{"content","Child response"}});QTRY_VERIFY(pane->transcript()->toPlainText().contains("Child response"));QVERIFY(!window.findChild<ConversationView*>("conversationView")->toPlainText().contains("Child response"));pane->composer()->setPlainText("Queued child prompt");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(engine(window,id)->queuedMessages().size(),1);QVERIFY(!pane->findChild<QToolButton*>("promoteSideChatButton")->isEnabled());provider.finish(0);QTRY_COMPARE(provider.requests.size(),2);provider.send(1,{{"content","Second child response"}});provider.finish(1);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(window.currentSessionId(),QString("parent"));QCOMPARE(saved(data.path(),"parent").messages.size(),2);QVERIFY(saved(data.path(),id).messages.size()>=6);}
void questionAnswersStayInChild(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("Ask me");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);provider.question(0);QTRY_VERIFY(!engine(window,id)->pendingQuestion().isEmpty());QTRY_VERIFY(pane->questions()->isVisible());const auto token=engine(window,id)->pendingQuestion()["id"].toString();emit pane->questions()->submitStep("parent",token,0,{{"name","wrong"}});QVERIFY(!engine(window,id)->pendingQuestion().isEmpty());emit pane->questions()->submitStep(id,token,0,{{"name","Side answer"}});QTRY_COMPARE(provider.requests.size(),2);QVERIFY(QJsonDocument(provider.requests[1]).toJson().contains("Side answer"));QVERIFY(!QJsonDocument(provider.requests[1]).toJson().contains("wrong"));provider.finish(1);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(saved(data.path(),"parent").messages.size(),2);}
void fileCompletionTargetsChildOnly(){QTemporaryDir data,workspace;Provider provider;QVERIFY(provider.seed(data.path(),workspace.path()));QVERIFY(put(workspace.filePath("side.txt"),"child completion"));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();auto* composer=pane->composer();composer->setFocus();composer->setPlainText("@side");composer->moveCursor(QTextCursor::End);auto* list=composer->findChild<QListWidget*>("composerCompletionList");QTRY_VERIFY_WITH_TIMEOUT(list->count()>0,5000);int chosen=-1;for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()=="file:ws1/side.txt")chosen=i;QVERIFY(chosen>=0);list->setCurrentRow(chosen);QTest::keyClick(composer,Qt::Key_Return);QCOMPARE(saved(data.path(),id).draftAttachments.size(),1);QVERIFY(saved(data.path(),"parent").draftAttachments.isEmpty());QVERIFY(composer->toPlainText().contains("path:ws1/side.txt"));}
void closeDiscardsBusyChildAndKeepsParent(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("Running child");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);auto* tabs=window.findChild<QTabWidget*>();emit tabs->tabCloseRequested(tabs->indexOf(pane));QTRY_VERIFY(!pane);QVERIFY(saved(data.path(),id).id.isEmpty());QVERIFY(!saved(data.path(),"parent").id.isEmpty());QVERIFY(!engine(window,id));}
void archiveUnloadsChildrenAndRestoreReopens(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("Retained child draft");QTRY_COMPARE(saved(data.path(),id).draft,QString("Retained child draft"));QVERIFY(action(window,"parent","archiveConversation"));QTRY_VERIFY(!pane);QVERIFY(!engine(window,id));QVERIFY(saved(data.path(),"parent").archived);QVERIFY(!saved(data.path(),id).archived);QCOMPARE(saved(data.path(),id).sideChatParentId,QString("parent"));auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings);emit settings->restoreConversationRequested("parent");QTRY_VERIFY(!saved(data.path(),"parent").archived);QTRY_VERIFY(window.findChild<SideChatPane*>());QCOMPARE(window.findChild<SideChatPane*>()->composer()->toPlainText(),QString("Retained child draft"));}
void archiveRechecksFamilyMutationAfterCancellation_data(){QTest::addColumn<bool>("mutateParent");QTest::newRow("parent")<<true;QTest::newRow("side-child")<<false;}
void archiveRechecksFamilyMutationAfterCancellation(){
    QFETCH(bool,mutateParent);QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();QPointer<AgentToolHost> target;for(auto* host:window.findChildren<AgentToolHost*>())if(host->agentId()==(mutateParent?QString("parent"):id))target=host;QVERIFY(target);
    pane->composer()->setPlainText("Keep this family active");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);auto* child=engine(window,id);QVERIFY(child&&child->isBusy());bool started=false;
    connect(child,&AgentEngine::finished,&window,[&]{if(started||!target)return;target->recoverMutations("archive-reentry-recovery");started=target->mutationBusy();});
    QVERIFY(action(window,"parent","archiveConversation"));QVERIFY(started);QVERIFY2(pane,"Archive must preserve side panes when a cancellation callback starts a family file mutation");QVERIFY(target);QVERIFY(!saved(data.path(),"parent").archived);QVERIFY(!saved(data.path(),id).archived);QVERIFY(engine(window,id));QTRY_VERIFY(!target->mutationBusy());
}
void deleteParentCascadesWithSaveConflictProtection(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();SessionStore external(data.path());auto changed=external.load();changed[0].title="External edit";QVERIFY(external.save(changed));QVERIFY(action(window,"parent","deleteConversation"));auto* dialog=window.findChild<QMessageBox*>("deleteConversationDialog");QVERIFY(dialog);dialog->done(QMessageBox::Yes);QVERIFY(pane);QVERIFY(!saved(data.path(),id).id.isEmpty());QVERIFY(!saved(data.path(),"parent").id.isEmpty());}
void restartRestoresChildWithoutSelectingItAsMain(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));QString id;{MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);id=pane->chatId();pane->composer()->setPlainText("Restart child");QTRY_COMPARE(saved(data.path(),id).draft,QString("Restart child"));}MainWindow restored(data.path());expose(restored);QCOMPARE(restored.currentSessionId(),QString("parent"));auto* pane=restored.findChild<SideChatPane*>();QVERIFY(pane);QCOMPARE(pane->chatId(),id);QCOMPARE(pane->composer()->toPlainText(),QString("Restart child"));QVERIFY(!listed(restored,id));}
void confirmedParentDeletionRemovesChild(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();QVERIFY(action(window,"parent","deleteConversation"));auto* dialog=window.findChild<QMessageBox*>("deleteConversationDialog");QVERIFY(dialog);dialog->done(QMessageBox::Yes);QTRY_VERIFY(!pane);QVERIFY(saved(data.path(),id).id.isEmpty());QVERIFY(saved(data.path(),"parent").id.isEmpty());QVERIFY(!engine(window,id));SessionStore store(data.path());store.load();QVERIFY(store.pendingCleanup().isEmpty());}
void promotionSaveConflictLeavesPaneAndParentIdentity(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);QPointer<SideChatPane> pane=create(window);QVERIFY(pane);const auto id=pane->chatId();SessionStore external(data.path());auto changed=external.load();changed[0].title="External edit";QVERIFY(external.save(changed));pane->findChild<QToolButton*>("promoteSideChatButton")->click();QVERIFY(pane);QCOMPARE(window.currentSessionId(),QString("parent"));QCOMPARE(saved(data.path(),id).sideChatParentId,QString("parent"));QVERIFY(!listed(window,id));}
void rejectedHistoryKeepsChildDraftAndAttachment(){QTemporaryDir data,workspace;Provider provider;QVERIFY(provider.seed(data.path(),workspace.path()));SessionStore seedStore(data.path());auto chats=seedStore.load();chats[0].messages.append(QJsonObject{{"role","tool"},{"tool_call_id","orphan"},{"content","Malformed inherited result"}});QVERIFY(seedStore.save(chats));QVERIFY(put(workspace.filePath("context.txt"),"attached context"));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();emit pane->filesAttached(id,{workspace.filePath("context.txt")});QCOMPARE(saved(data.path(),id).draftAttachments.size(),1);const QString text="  rejected child  \n";pane->composer()->setPlainText(text);pane->findChild<QToolButton*>("sideChatSendButton")->click();QCOMPARE(provider.requests.size(),0);QCOMPARE(pane->composer()->toPlainText(),text);QCOMPARE(saved(data.path(),id).draft,text);QCOMPARE(saved(data.path(),id).draftAttachments.size(),1);}
void inlineApprovalUsesChildRowAndChatIdentity(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("Request tool");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);const auto args=QString::fromUtf8(QJsonDocument(QJsonObject{{"explanation","Side fixture"},{"script","return 3;"}}).toJson(QJsonDocument::Compact));provider.send(0,{{"tool_calls",QJsonArray{QJsonObject{{"index",0},{"id","side-call"},{"type","function"},{"function",QJsonObject{{"name","executeSandboxJs"},{"arguments",args}}}}}}});provider.finish(0,"tool_calls");QString rowId,partId;auto locate=[&]{for(const auto& rowValue:engine(window,id)->displayMessages()){const auto row=rowValue.toObject();for(const auto& partValue:row["parts"].toArray()){const auto part=partValue.toObject();if(part["state"]=="approval-requested"){rowId=row["id"].toString();partId=part["id"].toString();return true;}}}return false;};QTRY_VERIFY(locate());emit pane->transcript()->approvalDecided("parent",rowId,partId,"side-call",false);QCOMPARE(provider.requests.size(),1);emit pane->transcript()->approvalDecided(id,rowId,partId,"side-call",false);QTRY_COMPARE(provider.requests.size(),2);provider.finish(1);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(saved(data.path(),"parent").messages.size(),2);}
void integratedThemesQuestionQueueAndCompactGeometry(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();pane->composer()->setPlainText("Review this implementation");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);provider.send(0,{{"reasoning_content","Check the constraints before applying the change."}});provider.send(0,{{"content","The change is ready for review.\n\n```cpp\nconst int answer = 42;\n```"}});pane->composer()->setPlainText("Then check the tests");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(engine(window,id)->queuedMessages().size(),1);provider.question(0);QTRY_VERIFY(!engine(window,id)->pendingQuestion().isEmpty());QTRY_VERIFY(pane->questions()->isVisible());auto* settings=window.findChild<SettingsPage*>();for(const auto& theme:QStringList{"light","dark","light"}){QVERIFY(settings->setPreference("personalization.appearance",theme));for(const auto width:{1100,350}){window.resize(width,750);QTest::qWait(80);QVERIFY(pane->isVisible());QVERIFY(pane->questions()->isVisible());QVERIFY(pane->queue()->isVisible());QVERIFY(window.width()==width);const auto mismatch=backgroundMismatch(window,pane,QColor(theme=="dark"?"#21242b":"#fdfdfd"));QVERIFY2(mismatch.isEmpty(),qPrintable(mismatch));auto* controls=pane->findChild<QScrollArea*>("sideChatControls");QVERIFY(controls);for(const auto& name:QStringList{"answer_name","questionSubmit","questionWriteMessage"}){auto* control=pane->findChild<QWidget*>(name);QVERIFY(control);QTRY_VERIFY(controls->viewport()->rect().contains(control->mapTo(controls->viewport(),control->rect().center())));}QVERIFY(controls->viewport()->rect().contains(pane->queue()->mapTo(controls->viewport(),QPoint(8,8))));capture(window,QString("side-question-queue-%1-%2").arg(theme).arg(width));}}pane->findChild<QToolButton*>("sideChatStopButton")->click();engine(window,id)->cancel();QTRY_VERIFY(!engine(window,id)->isBusy());}
// stop_agent (S3): with a side-chat tab active, Ctrl+C stops that side chat,
// from its composer, through the window fallback and from the main composer
// (upstream disables the main listener while a side chat is the active
// surface). A busy parent must keep running. Before, none of these existed and
// the main composer stopped the main agent regardless of the active surface.
void ctrlCInSideChatTargetsSideChat_data(){QTest::addColumn<bool>("parentBusy");QTest::newRow("parent-idle")<<false;QTest::newRow("parent-busy")<<true;}
void ctrlCInSideChatTargetsSideChat(){
    QFETCH(bool,parentBusy);
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);
    window.activateWindow();QApplication::setActiveWindow(&window);QCoreApplication::processEvents();
    auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();auto* tabs=window.findChild<QTabWidget*>("contentTabs");auto* split=window.findChild<WorkbenchSplitter*>();auto* mainComposer=window.findChild<Composer*>("composerInput");QVERIFY(tabs&&split&&mainComposer);
    const auto parentRunning=[&]{auto* parent=engine(window,"parent");return parent&&parent->isBusy();};
    int requests=0;
    if(parentBusy){window.submitPrompt("Running parent");++requests;QTRY_COMPARE(provider.requests.size(),requests);QTRY_VERIFY(parentRunning());}
    tabs->setCurrentWidget(pane);split->setContentCollapsed(false);QVERIFY(split->contentVisible());
    const auto runChild=[&](const QString& prompt){pane->composer()->setPlainText(prompt);pane->findChild<QToolButton*>("sideChatSendButton")->click();};
    runChild("Running child");++requests;QTRY_COMPARE(provider.requests.size(),requests);QTRY_VERIFY(engine(window,id)&&engine(window,id)->isBusy());
    pane->composer()->setFocus();QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(pane->composer()));
    QTest::keyClick(pane->composer(),Qt::Key_C,Qt::ControlModifier);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(parentRunning(),parentBusy);
    // Window fallback from a focusable, non-text widget of the side pane.
    runChild("Second child");++requests;QTRY_COMPARE(provider.requests.size(),requests);QTRY_VERIFY(engine(window,id)->isBusy());
    auto* stop=window.findChild<QShortcut*>("shortcut_stop_agent");QVERIFY(stop);QTRY_VERIFY(stop->isEnabled());QSignalSpy activated(stop,&QShortcut::activated);
    auto* changes=pane->findChild<QToolButton*>("sideChatChangesButton");QVERIFY(changes);changes->setFocus(Qt::OtherFocusReason);QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(changes));
    QTest::keyClick(changes,Qt::Key_C,Qt::ControlModifier);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(activated.size(),1);QCOMPARE(parentRunning(),parentBusy);
    // The main composer resolves the same active surface.
    runChild("Third child");++requests;QTRY_COMPARE(provider.requests.size(),requests);QTRY_VERIFY(engine(window,id)->isBusy());
    mainComposer->setFocus();QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(mainComposer));
    QTest::keyClick(mainComposer,Qt::Key_C,Qt::ControlModifier);QTRY_VERIFY(!engine(window,id)->isBusy());QCOMPARE(parentRunning(),parentBusy);
    if(parentBusy){
        // Collapsing the content makes the main chat the active surface again.
        split->setContentCollapsed(true);QVERIFY(!split->contentVisible());
        mainComposer->setFocus();QTRY_COMPARE(QApplication::focusWidget(),static_cast<QWidget*>(mainComposer));
        QTest::keyClick(mainComposer,Qt::Key_C,Qt::ControlModifier);QTRY_VERIFY(!parentRunning());
    }
    QCOMPARE(window.currentSessionId(),QString("parent"));
}
void hiddenChildImagesResolveOnlyOwnedBytes(){QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();auto encoded=[](const QColor& color){QImage image(40,30,QImage::Format_ARGB32);image.fill(color);QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return bytes;};AttachmentStore store(data.path());const auto own=store.write(id,"own.png",encoded(QColor("#00b8c8")));const auto parent=store.write("parent","parent.png",encoded(QColor("#c11e3b")));const auto foreign=store.write("other","other.png",encoded(QColor("#387b22")));QVERIFY(!own.path.isEmpty()&&!parent.path.isEmpty()&&!foreign.path.isEmpty());pane->composer()->setPlainText("Render owned image while hidden");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);auto* tabs=window.findChild<QTabWidget*>();auto* unrelated=new QWidget;tabs->setCurrentIndex(tabs->addTab(unrelated,"Fixture alternate tab"));QVERIFY(!pane->isVisible());provider.send(0,{{"content","![own](path:"+own.path+")\n\n![parent](path:"+parent.path+")\n\n![foreign](path:"+foreign.path+")"}});provider.finish(0);auto count=[&]{int result=0;for(auto* text:pane->transcript()->findChildren<TranscriptText*>())result+=text->document()->property("trustedImageCount").toInt();return result;};QTRY_COMPARE(count(),1);QVERIFY(!pane->isVisible());tabs->setCurrentWidget(pane);QTest::qWait(50);const auto image=pane->transcript()->grab().toImage();int ownPixels=0,foreignPixels=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const auto color=image.pixelColor(x,y);ownPixels+=color==QColor("#00b8c8");foreignPixels+=color==QColor("#c11e3b")||color==QColor("#387b22");}QVERIFY(ownPixels>100);QCOMPARE(foreignPixels,0);for(auto* text:window.findChild<ConversationView*>("conversationView")->findChildren<TranscriptText*>())QCOMPARE(text->document()->property("trustedImageCount").toInt(),0);}
};
QTEST_MAIN(SideChatHostTest)
#include "test_side_chat_host.moc"

