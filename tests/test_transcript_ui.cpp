#include <QtTest>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "core/ProviderStore.h"
#include "ui/SessionStore.h"
#include "core/AgentEngine.h"

using namespace ade;
namespace {
QByteArray chunk(const QJsonObject& delta,const QString& finish={}) {
    QJsonObject choice{{"delta",delta},{"finish_reason",finish.isEmpty()?QJsonValue():QJsonValue(finish)}};
    return "data: "+QJsonDocument(QJsonObject{{"choices",QJsonArray{choice}}}).toJson(QJsonDocument::Compact)+"\n\n";
}
class StreamProvider:public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> requests;
    QList<QPointer<QTcpSocket>> sockets;
    StreamProvider(){
        server.listen(QHostAddress::LocalHost);
        connect(&server,&QTcpServer::newConnection,this,[this]{while(auto* socket=server.nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
                const int end=bytes.indexOf("\r\n\r\n");if(end<0)return;int length=0;
                for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<end+4+length)return;
                socket->setProperty("handled",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());sockets.append(socket);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
            });
        }});
    }
    void send(int index,const QJsonObject& delta){if(index<sockets.size()&&sockets[index]){sockets[index]->write(chunk(delta));sockets[index]->flush();}}
    void finish(int index,const QString& reason="stop"){if(index<sockets.size()&&sockets[index]){sockets[index]->write(chunk({},reason)+"data: [DONE]\n\n");sockets[index]->disconnectFromHost();}}
    void tool(int index,const QString& script){
        const QString args=QString::fromUtf8(QJsonDocument(QJsonObject{{"explanation","Native transcript fixture"},{"script",script}}).toJson(QJsonDocument::Compact));
        send(index,{{"tool_calls",QJsonArray{QJsonObject{{"index",0},{"id","reused-call"},{"type","function"},{"function",QJsonObject{{"name","executeSandboxJs"},{"arguments",args}}}}}}});finish(index,"tool_calls");
    }
    bool seed(const QString& directory) {
        ProviderConfig provider;provider.id="fixture";provider.name="Fixture";provider.model="fixture-model";provider.apiKey="synthetic-transcript-token";provider.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        if(!ProviderStore::save(directory+"/providers.json",{provider}))return false;
        QList<Conversation> chats;
        for(const auto& id:{"first","second"}){Conversation chat;chat.id=id;chat.title=id;chat.titleLocked=true;chat.providerId=provider.id;chat.model=provider.model;chats.append(chat);}
        return SessionStore(directory).save(chats);
    }
};
bool select(MainWindow& window,const QString& id){auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){list->setCurrentItem(list->item(i));return true;}return false;}
QWidget* partWidget(ConversationView* view,const QString& type){for(auto* widget:view->findChildren<QWidget*>())if(widget->property("partType")==type)return widget;return nullptr;}
QJsonObject toolPart(const QJsonArray& rows,QString* rowId=nullptr){for(const auto& value:rows){const auto row=value.toObject();for(const auto& entry:row["parts"].toArray()){const auto part=entry.toObject();if(part["type"]=="tool"&&part["state"]=="approval-requested"){if(rowId)*rowId=row["id"].toString();return part;}}}return {};}
}
class TranscriptUiTest:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
    void canceledReasoningSurvivesChatSwitchAndRestartWithoutProviderLeak(){
        QTemporaryDir data;StreamProvider provider;QVERIFY(provider.seed(data.path()));
        auto window=std::make_unique<MainWindow>(data.path());QVERIFY(select(*window,"first"));
        auto* view=window->findChild<ConversationView*>("conversationView");QVERIFY(view);
        window->submitPrompt("Show partial reasoning");QTRY_COMPARE(provider.requests.size(),1);
        provider.send(0,{{"reasoning_content","Retained reasoning fixture"}});
        QTRY_VERIFY(view->toPlainText().contains("Retained reasoning fixture"));
        QPointer<QWidget> firstPart=partWidget(view,"reasoning");QVERIFY(firstPart);
        provider.send(0,{{"reasoning_content"," continued"}});
        QTRY_VERIFY(view->toPlainText().contains("fixture continued"));QCOMPARE(partWidget(view,"reasoning"),firstPart.data());
        QVERIFY(select(*window,"second"));QVERIFY(!view->toPlainText().contains("Retained reasoning fixture"));
        QVERIFY(select(*window,"first"));QVERIFY(view->toPlainText().contains("fixture continued"));
        window->findChild<QToolButton*>("stopButton")->click();
        QTRY_VERIFY(window->findChild<QToolButton*>("stopButton")->isHidden());
        QVERIFY(view->toPlainText().contains("fixture continued"));
        auto saved=SessionStore(data.path()).load();QJsonArray history;
        for(const auto& chat:saved)if(chat.id=="first")history=chat.messages;
        QVERIFY(QJsonDocument(history).toJson().contains("Retained reasoning fixture"));
        window.reset();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        window=std::make_unique<MainWindow>(data.path());QVERIFY(select(*window,"first"));
        view=window->findChild<ConversationView*>("conversationView");QVERIFY(view->toPlainText().contains("fixture continued"));
        window->submitPrompt("Continue without resending partial reasoning");QTRY_COMPARE(provider.requests.size(),2);
        QVERIFY(!QJsonDocument(provider.requests.last()).toJson().contains("Retained reasoning fixture"));
        provider.send(1,{{"content","Finished after restart"}});provider.finish(1);
        QTRY_VERIFY(view->toPlainText().contains("Finished after restart"));
        QTRY_VERIFY(window->findChild<QToolButton*>("stopButton")->isHidden());
        QVERIFY(!window->findChild<QToolButton*>("sendButton")->isEnabled());
    }
    void inlineDecisionsRejectStaleChatAndPartAndShowRealToolOutput(){
        QTemporaryDir data;StreamProvider provider;QVERIFY(provider.seed(data.path()));MainWindow window(data.path());QVERIFY(select(window,"first"));
        auto* view=window.findChild<ConversationView*>("conversationView");window.submitPrompt("Require an inline decision");QTRY_COMPARE(provider.requests.size(),1);
        provider.tool(0,"return 'denied script must never run';");
        QTRY_VERIFY(view->findChild<QPushButton*>("denyTool"));auto* engine=window.findChild<AgentEngine*>();QVERIFY(engine);
        QString oldRow;const auto oldPart=toolPart(engine->displayMessages(),&oldRow);QVERIFY(!oldPart.isEmpty());
        QVERIFY(window.findChildren<QMessageBox*>().isEmpty());
        QVERIFY(select(window,"second"));
        emit view->approvalDecided("first",oldRow,oldPart["id"].toString(),"reused-call",true);
        QVERIFY(!toolPart(engine->displayMessages()).isEmpty());QCOMPARE(provider.requests.size(),1);
        QVERIFY(select(window,"first"));view->findChild<QPushButton*>("denyTool")->click();
        QTRY_COMPARE(provider.requests.size(),2);provider.send(1,{{"content","Denied correctly"}});provider.finish(1);
        QTRY_VERIFY(window.findChild<QToolButton*>("stopButton")->isHidden());
        QVERIFY(QJsonDocument(provider.requests[1]).toJson().contains("denied"));
        window.submitPrompt("Reuse the provider tool ID safely");QTRY_COMPARE(provider.requests.size(),3);
        provider.tool(2,"API.output('Visible running output');await new Promise(r=>setTimeout(r,600));return 'FINAL_'+'TOOL_OUTPUT';");
        QTRY_VERIFY(!toolPart(engine->displayMessages()).isEmpty());QTRY_VERIFY(view->findChild<QPushButton*>("approveTool"));
        emit view->approvalDecided("first",oldRow,oldPart["id"].toString(),"reused-call",true);
        QVERIFY(!toolPart(engine->displayMessages()).isEmpty());QCOMPARE(provider.requests.size(),3);
        QPushButton* approve=nullptr;for(auto* button:view->findChildren<QPushButton*>("approveTool"))if(button->isEnabled())approve=button;QVERIFY(approve);approve->click();
        QTRY_VERIFY_WITH_TIMEOUT(view->toPlainText().contains("Visible running output"),5000);
        QTRY_COMPARE_WITH_TIMEOUT(provider.requests.size(),4,8000);
        provider.send(3,{{"content","Tool finished"}});provider.finish(3);
        QTRY_VERIFY(window.findChild<QToolButton*>("stopButton")->isHidden());
        QVERIFY(view->toPlainText().contains("FINAL_TOOL_OUTPUT"));
        QCOMPARE(view->toPlainText().count("FINAL_TOOL_OUTPUT"),1);
        QVERIFY(window.findChildren<QMessageBox*>().isEmpty());
    }
};
QTEST_MAIN(TranscriptUiTest)
#include "test_transcript_ui.moc"
