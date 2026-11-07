#include "ui/MainWindow.h"
#include "ui/SideChatPane.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "attachments/AttachmentStore.h"
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>
#include <QtTest>
// Host Retry affordance (agent-history slice 1). Before the change no
// transcriptRetryButton existed and a failed turn could only be re-sent as a
// duplicate user message; every test below locates that control first.
using namespace ade;
namespace {
QByteArray chunk(const QJsonObject& delta,const QString& reason={}){return "data: "+QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"delta",delta},{"finish_reason",reason.isEmpty()?QJsonValue():QJsonValue(reason)}}}}}).toJson(QJsonDocument::Compact)+"\n\n";}
class Provider:public QObject{
public:
QTcpServer server;QList<QJsonObject> requests;QList<QPointer<QTcpSocket>> sockets;
Provider(){server.listen(QHostAddress::LocalHost);connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QTcpSocket::readyRead,this,[this,socket]{if(socket->property("done").toBool()){socket->readAll();return;}const auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;int length=0;for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();if(bytes.size()<end+4+length)return;socket->setProperty("done",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());sockets.append(socket);socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");});}});}
bool seed(const QString& data,const QString& workspace={}){ProviderConfig config;config.id="fixture";config.name="Fixture";config.model="fixture-model";config.apiKey="history-test-token";config.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());if(!ProviderStore::save(data+"/providers.json",{config}))return false;QList<Conversation> chats;for(const auto& id:QStringList{"parent","other"}){Conversation chat;chat.id=id;chat.title=id;chat.titleLocked=true;chat.autoTitleAttempted=true;chat.workspace=workspace;chat.providerId=config.id;chat.model=config.model;chat.messages={QJsonObject{{"role","user"},{"content","Inherited question"}},QJsonObject{{"role","assistant"},{"content","Inherited answer"}}};chats.append(chat);}return SessionStore(data).save(chats);}
void finish(int index){if(auto socket=sockets.value(index)){socket->write(chunk({{"content","Answer"}})+chunk({},"stop")+"data: [DONE]\n\n");socket->disconnectFromHost();}}
// The stream closes before any output or completion marker: an accepted turn
// fails with an empty output, retaining no assistant row.
void fail(int index){if(auto socket=sockets.value(index))socket->disconnectFromHost();}
};
void expose(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,750);window.show();}
SideChatPane* create(MainWindow& window){auto* button=window.findChild<QToolButton*>("openSideChatButton");if(!button)return nullptr;button->click();for(auto* pane:window.findChildren<SideChatPane*>())if(!pane->chatId().isEmpty())return pane;return nullptr;}
Conversation saved(const QString& data,const QString& id){for(const auto& chat:SessionStore(data).load())if(chat.id==id)return chat;return {};}
AgentEngine* engine(MainWindow& window,const QString& id){for(auto* host:window.findChildren<AgentToolHost*>())if(host->agentId()==id)return qobject_cast<AgentEngine*>(host->parent());return nullptr;}
bool action(MainWindow& window,const QString& id,const QString& name){auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){list->scrollToItem(list->item(i));const auto point=list->visualItemRect(list->item(i)).center();emit list->customContextMenuRequested(point);for(auto* menu:window.findChildren<QMenu*>("conversationMenu"))if(auto* item=menu->findChild<QAction*>(name)){item->trigger();menu->close();return true;}}return false;}
bool put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
ConversationView* mainView(MainWindow& window){return window.findChild<ConversationView*>("conversationView");}
QPushButton* retryButton(ConversationView* view){return view?view->findChild<QPushButton*>("transcriptRetryButton"):nullptr;}
bool retryShown(ConversationView* view){auto* card=view?view->findChild<QFrame*>("transcriptRuntimeError"):nullptr;return card&&card->isVisibleTo(view);}
QString statusText(MainWindow& window){auto* label=window.findChild<QLabel*>("sessionStatus");return label?label->text():QString();}
QStringList users(const QJsonArray& history){QStringList out;for(const auto& value:history)if(value.toObject()["role"]=="user")out.append(value.toObject()["content"].toString());return out;}
// Sends through the main composer and fails the accepted turn.
bool failMain(MainWindow& window,Provider& provider,const QString& text,int index){
    auto* composer=window.findChild<Composer*>("composerInput");if(!composer)return false;
    composer->setPlainText(text);window.findChild<QToolButton*>("sendButton")->click();
    if(!QTest::qWaitFor([&]{return provider.requests.size()==index+1;},5000))return false;
    provider.fail(index);
    return QTest::qWaitFor([&]{auto* active=engine(window,"parent");return active&&!active->isBusy()&&!active->turnFailure().isEmpty();},5000);
}
}
class HistoryActionsHostTest:public QObject{
Q_OBJECT
QTemporaryDir home;QByteArray originalHome;
private slots:
void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));originalHome=qgetenv("USERPROFILE");qputenv("USERPROFILE",home.path().toUtf8());}
void cleanupTestCase(){qputenv("USERPROFILE",originalHome);}
void mainRetryAppearsOnlyAfterAcceptedFailure(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);
    auto* view=mainView(window);auto* retry=retryButton(view);QVERIFY(retry);QVERIFY(!retryShown(view));
    auto* composer=window.findChild<Composer*>("composerInput");composer->setPlainText("FAILING_PROMPT");window.findChild<QToolButton*>("sendButton")->click();
    QTRY_COMPARE(provider.requests.size(),1);QVERIFY(!retryShown(view));
    provider.fail(0);QTRY_VERIFY(retryShown(view));QVERIFY(retry->isEnabled());
    // Upstream GenericError: the card shows the message; the transcript's own error
    // part for the same failed turn is hidden while the card owns it.
    QVERIFY(view->findChild<QLabel*>("transcriptRuntimeErrorTitle")->isVisibleTo(view));
    QVERIFY(view->findChild<QLabel*>("transcriptRuntimeErrorText")->isVisibleTo(view));
    QVERIFY(!view->findChild<QLabel*>("transcriptRuntimeErrorText")->text().isEmpty());
    {bool errorPart=false;for(auto* part:view->findChildren<QWidget*>())if(part->property("partType").toString()=="error"){errorPart=true;QVERIFY(!part->isVisibleTo(view));}QVERIFY(errorPart);}
    QTRY_VERIFY(!saved(data.path(),"parent").turnFailure.isEmpty());
    composer->setPlainText("Unsent draft");
    retry->click();
    QTRY_COMPARE(provider.requests.size(),2);
    QCOMPARE(QJsonDocument(provider.requests[1]).toJson().count("FAILING_PROMPT"),1);
    QCOMPARE(composer->toPlainText(),QString("Unsent draft"));
    QVERIFY(saved(data.path(),"parent").turnFailure.isEmpty());
    QCOMPARE(users(saved(data.path(),"parent").messages),(QStringList{"Inherited question","FAILING_PROMPT"}));
    QTRY_VERIFY(!retryShown(view));
    provider.finish(1);QTRY_VERIFY(!engine(window,"parent")->isBusy());QVERIFY(!retryShown(view));
    QCOMPARE(composer->toPlainText(),QString("Unsent draft"));
}
void successfulAndStoppedTurnsHaveNoRetry(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* view=mainView(window);
    auto* composer=window.findChild<Composer*>("composerInput");composer->setPlainText("SUCCESS");window.findChild<QToolButton*>("sendButton")->click();
    QTRY_COMPARE(provider.requests.size(),1);provider.finish(0);QTRY_VERIFY(!engine(window,"parent")->isBusy());QTest::qWait(30);QVERIFY(!retryShown(view));
    composer->setPlainText("STOPPED");window.findChild<QToolButton*>("sendButton")->click();QTRY_COMPARE(provider.requests.size(),2);
    window.findChild<QToolButton*>("stopButton")->click();QTRY_VERIFY(!engine(window,"parent")->isBusy());QTest::qWait(30);
    QVERIFY(!retryShown(view));QVERIFY(engine(window,"parent")->turnFailure().isEmpty());QVERIFY(saved(data.path(),"parent").turnFailure.isEmpty());
}
void restartRetainsRetryAndCapturedAttachment(){
    QTemporaryDir data,workspace;Provider provider;QVERIFY(provider.seed(data.path(),workspace.path()));
    QVERIFY(put(workspace.filePath("a.txt"),"ORIGINAL_BYTES_4821"));
    {
        SessionStore store(data.path());auto chats=store.load();QString error;
        const auto ref=AttachmentStore(data.path()).importFile("parent",workspace.filePath("a.txt"),{{"ws1",workspace.path()}},&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));
        for(auto& chat:chats){if(chat.id=="parent")chat.draftAttachments={ref.toJson()};}
        QVERIFY(store.save(chats));
    }
    {
        MainWindow window(data.path());expose(window);QVERIFY(failMain(window,provider,"ATTACHED_PROMPT",0));
        QVERIFY(QJsonDocument(provider.requests[0]).toJson().contains("ORIGINAL_BYTES_4821"));
        QTRY_VERIFY(!saved(data.path(),"parent").turnFailure.isEmpty());
    }
    QVERIFY(put(workspace.filePath("a.txt"),"REWRITTEN_BYTES_9134"));
    MainWindow restored(data.path());expose(restored);auto* view=mainView(restored);
    QTRY_VERIFY(retryShown(view));
    // No error part survives a restart for an empty-output failure.
    QVERIFY(view->findChild<QLabel*>("transcriptRuntimeErrorText")->isVisibleTo(view));
    QVERIFY(!view->findChild<QLabel*>("transcriptRuntimeErrorText")->text().isEmpty());
    retryButton(view)->click();
    QTRY_COMPARE(provider.requests.size(),2);
    const auto body=QJsonDocument(provider.requests[1]).toJson();
    QVERIFY(body.contains("ORIGINAL_BYTES_4821"));QVERIFY(!body.contains("REWRITTEN_BYTES_9134"));QCOMPARE(body.count("ATTACHED_PROMPT"),1);
    provider.finish(1);QTRY_VERIFY(!engine(restored,"parent")->isBusy());
    QVERIFY(saved(data.path(),"parent").turnFailure.isEmpty());
}
void casConflictRefusesRetry(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* view=mainView(window);
    QVERIFY(failMain(window,provider,"CAS_PROMPT",0));QTRY_VERIFY(retryShown(view));
    auto* composer=window.findChild<Composer*>("composerInput");composer->setPlainText("Unsent draft");QTest::qWait(400);
    auto* active=engine(window,"parent");const auto history=active->messages();const auto queue=active->queuedMessages();const auto failure=active->turnFailure();
    SessionStore external(data.path());auto changed=external.load();for(auto& chat:changed)if(chat.id=="other")chat.title="External edit";QVERIFY(external.save(changed));
    retryButton(view)->click();
    QTest::qWait(50);
    QVERIFY2(statusText(window).contains("another window"),qPrintable(statusText(window)));
    QVERIFY(!statusText(window).startsWith("Queue:"));
    QCOMPARE(provider.requests.size(),1);QCOMPARE(active->messages(),history);QCOMPARE(active->queuedMessages(),queue);QCOMPARE(active->turnFailure(),failure);
    QCOMPARE(composer->toPlainText(),QString("Unsent draft"));
    QVERIFY(retryShown(view));QVERIFY(retryButton(view)->isEnabled());
}
void queueNoteAndAtomicClear(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* view=mainView(window);
    auto* composer=window.findChild<Composer*>("composerInput");composer->setPlainText("QUEUE_FAILS");window.findChild<QToolButton*>("sendButton")->click();QTRY_COMPARE(provider.requests.size(),1);
    composer->setPlainText("QUEUED_LATER");window.findChild<QToolButton*>("sendButton")->click();QTRY_COMPARE(engine(window,"parent")->queuedMessages().size(),1);
    provider.fail(0);QTRY_VERIFY(retryShown(view));
    auto* note=view->findChild<QLabel*>("transcriptRetryQueueNote");QVERIFY(note->isVisibleTo(view));QVERIFY2(note->text().contains("1 queued message"),qPrintable(note->text()));
    QCOMPARE(saved(data.path(),"parent").queuedMessages.size(),1);
    // An idle queue edit alone refreshes the note; nothing re-renders the transcript.
    auto* active=engine(window,"parent");const auto queued=active->queuedMessages();
    QVERIFY(active->setQueuedMessages(QJsonArray{queued[0],QJsonObject{{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"role","user"},{"content","QUEUED_SECOND"}}}));
    QVERIFY2(note->text().contains("2 queued messages"),qPrintable(note->text()));
    QVERIFY(active->setQueuedMessages(queued));
    QVERIFY2(note->text().contains("1 queued message."),qPrintable(note->text()));QVERIFY(note->isVisibleTo(view));
    retryButton(view)->click();QTRY_COMPARE(provider.requests.size(),2);
    const auto chat=saved(data.path(),"parent");
    QVERIFY(chat.queuedMessages.isEmpty());QVERIFY(chat.turnFailure.isEmpty());QCOMPARE(users(chat.messages),(QStringList{"Inherited question","QUEUE_FAILS"}));
    QVERIFY(!QJsonDocument(provider.requests[1]).toJson().contains("QUEUED_LATER"));
    provider.finish(1);QTRY_VERIFY(!engine(window,"parent")->isBusy());QTest::qWait(50);QCOMPARE(provider.requests.size(),2);
}
void sideChatRetryIsOwnerScoped(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);
    auto* pane=create(window);QVERIFY(pane);const auto id=pane->chatId();
    pane->composer()->setPlainText("SIDE_FAILS");pane->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_COMPARE(provider.requests.size(),1);
    provider.fail(0);QTRY_VERIFY(retryShown(pane->transcript()));QVERIFY(!retryShown(mainView(window)));
    const auto row=engine(window,id)->turnFailure()["userRowId"].toString();QVERIFY(!row.isEmpty());
    emit pane->transcript()->failedTurnRetryRequested("parent",row);
    emit mainView(window)->failedTurnRetryRequested(id,row);
    QTest::qWait(50);QCOMPARE(provider.requests.size(),1);QVERIFY(!engine(window,id)->turnFailure().isEmpty());
    pane->composer()->setPlainText("Side draft");
    retryButton(pane->transcript())->click();QTRY_COMPARE(provider.requests.size(),2);
    QVERIFY(QJsonDocument(provider.requests[1]).toJson().contains("SIDE_FAILS"));
    QCOMPARE(pane->composer()->toPlainText(),QString("Side draft"));
    QVERIFY(saved(data.path(),id).turnFailure.isEmpty());QCOMPARE(saved(data.path(),"parent").messages.size(),2);
    provider.finish(1);QTRY_VERIFY(!engine(window,id)->isBusy());
}
void staleSignalAfterArchiveOrDeleteIgnored(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* view=mainView(window);
    QVERIFY(failMain(window,provider,"ARCHIVED_PROMPT",0));QTRY_VERIFY(retryShown(view));
    const auto row=engine(window,"parent")->turnFailure()["userRowId"].toString();
    emit view->failedTurnRetryRequested("missing-chat",row);
    QVERIFY(action(window,"parent","archiveConversation"));QTRY_VERIFY(saved(data.path(),"parent").archived);
    emit view->failedTurnRetryRequested("parent",row);
    QTest::qWait(50);QCOMPARE(provider.requests.size(),1);QVERIFY(!retryShown(view)||view->conversationId()!="parent");
}
void duplicateClickSendsOnce(){
    QTemporaryDir data;Provider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());expose(window);auto* view=mainView(window);
    QVERIFY(failMain(window,provider,"ONCE_PROMPT",0));QTRY_VERIFY(retryShown(view));
    auto* retry=retryButton(view);const auto row=engine(window,"parent")->turnFailure()["userRowId"].toString();
    retry->click();retry->click();emit view->failedTurnRetryRequested("parent",row);
    QTRY_COMPARE(provider.requests.size(),2);QTest::qWait(80);QCOMPARE(provider.requests.size(),2);
    QCOMPARE(users(engine(window,"parent")->messages()),(QStringList{"Inherited question","ONCE_PROMPT"}));
    provider.finish(1);QTRY_VERIFY(!engine(window,"parent")->isBusy());
}
};
QTEST_MAIN(HistoryActionsHostTest)
#include "test_history_actions_host.moc"
