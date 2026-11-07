#include "ui/MainWindow.h"
#include "ui/ConversationView.h"
#include "ui/Composer.h"
#include "ui/SessionStore.h"
#include "ui/ModelSelection.h"
#include "ui/SettingsPage.h"
#include "ui/DownloadsPage.h"
#include "browser/browserwidget.h"
#include "apps/MiniAppPage.h"
#include "core/ProviderStore.h"
#include <QtTest>
#include <QFile>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolButton>
#include <QTabWidget>
#include <QScrollBar>
using namespace ade;
namespace {
int agentCount(QListWidget* list) {
    int count=0;
    for(int i=0;i<list->count();++i)if(!list->item(i)->data(Qt::UserRole).toString().isEmpty())++count;
    return count;
}
bool selectAgent(QListWidget* list,const QString& id) {
    for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==id){list->setCurrentItem(list->item(i));return true;}
    return false;
}
}
class UiTest:public QObject {
    Q_OBJECT
private slots:
    void storeRoundTripAndInvalidFile();
    void storeRejectsStaleOrCorruptOverwrite();
    void conversationSelectionPreservesWorkspaces();
    void draftsStayWithTheirConversations();
    void selectionsKeepInstanceAndModelIdentity();
    void encryptedProviderStoreRejectsStaleAndCorruptWrites();
    void emptyComposerStaysCompact();
    void sendStreamsAndPersists();
    void browserPreferencesAndDownloadsReachNativeHost();
};
void UiTest::storeRoundTripAndInvalidFile(){
    QTemporaryDir dir;QVERIFY(dir.isValid());SessionStore store(dir.path());
    Conversation s;s.id="unicode-session";s.title=QString::fromUtf8("اختبار 日本語");s.workspace=dir.path();s.created=123;s.updated=456;
    s.messages.append(QJsonObject{{"role","user"},{"content","hello"}});s.archived=true;
    QString error;QVERIFY2(store.save({s},&error),qPrintable(error));const auto result=store.load(&error);
    QCOMPARE(result.size(),1);QCOMPARE(result[0].title,s.title);QCOMPARE(result[0].messages,s.messages);QVERIFY(result[0].archived);
    QFile broken(dir.path()+"/sessions.json");QVERIFY(broken.open(QIODevice::WriteOnly));broken.write("{invalid");broken.close();
    QVERIFY(store.load(&error).isEmpty());QVERIFY(!error.isEmpty());
    QVERIFY(!store.save({s},&error));QVERIFY(broken.open(QIODevice::ReadOnly));QCOMPARE(broken.readAll(),QByteArray("{invalid"));
}
void UiTest::storeRejectsStaleOrCorruptOverwrite(){
    QTemporaryDir data;SessionStore a(data.path()),b(data.path());
    Conversation first;first.id="first";first.title="First";QString error;
    QVERIFY(a.save({first},&error));QCOMPARE(b.load(&error).size(),1);
    first.title="Updated by A";QVERIFY(a.save({first},&error));
    first.title="Stale B";QVERIFY(!b.save({first},&error));QVERIFY(error.contains("changed"));
    QCOMPARE(SessionStore(data.path()).load().first().title,QString("Updated by A"));
}
void UiTest::conversationSelectionPreservesWorkspaces(){
    QTemporaryDir data,workspace1,workspace2;QVERIFY(data.isValid());
    {
        MainWindow window(data.path());window.connectWorkspace(workspace1.path());
        const auto first=window.currentSessionId();
        auto* create=window.findChild<QPushButton*>("newAgentButton");QVERIFY(create);create->click();
        window.connectWorkspace(workspace2.path());QVERIFY(window.currentSessionId()!=first);
        auto* list=window.findChild<QListWidget*>("agentList");QCOMPARE(agentCount(list),2);
        QVERIFY(selectAgent(list,first));QCOMPARE(window.currentSessionId(),first);
        const auto stored=SessionStore(data.path()).load();QCOMPARE(stored.size(),2);
        QCOMPARE(stored[0].workspace,QFileInfo(workspace2.path()).canonicalFilePath());
        QCOMPARE(stored[1].workspace,QFileInfo(workspace1.path()).canonicalFilePath());
    }
    MainWindow reopened(data.path());QCOMPARE(agentCount(reopened.findChild<QListWidget*>("agentList")),2);
}
void UiTest::draftsStayWithTheirConversations(){
    QTemporaryDir data;QString firstId,secondId;
    {
        MainWindow window(data.path());firstId=window.currentSessionId();
        auto* input=window.findChild<Composer*>("composerInput");input->setPlainText("first draft\n日本語");
        window.findChild<QPushButton*>("newAgentButton")->click();secondId=window.currentSessionId();
        QCOMPARE(input->toPlainText(),QString());input->setPlainText("second draft");
        QVERIFY(selectAgent(window.findChild<QListWidget*>("agentList"),firstId));
        QCOMPARE(window.currentSessionId(),firstId);QCOMPARE(input->toPlainText(),QString("first draft\n日本語"));
        window.close();
    }
    MainWindow reopened(data.path());auto* list=reopened.findChild<QListWidget*>("agentList");
    auto* input=reopened.findChild<Composer*>("composerInput");
    QCOMPARE(reopened.currentSessionId(),firstId);QCOMPARE(input->toPlainText(),QString("first draft\n日本語"));
    QVERIFY(selectAgent(list,secondId));QCOMPARE(reopened.currentSessionId(),secondId);QCOMPARE(input->toPlainText(),QString("second draft"));
}
void UiTest::emptyComposerStaysCompact(){
    Composer composer;composer.resize(500,74);composer.show();composer.setPlainText("");
    QTRY_VERIFY(composer.height()<=100);
    // Pinned chat-input.tsx uses max-h-64 (256px at the default UI scale).
    // Long input must stop growing and remain scrollable, then shrink again.
    composer.setPlainText(QString("line\n").repeated(40));QTRY_COMPARE(composer.height(),256);QTRY_VERIFY(composer.verticalScrollBar()->maximum()>0);
    composer.setUiScale(1.3);QTRY_COMPARE(composer.height(),qRound(256*1.3));QTRY_VERIFY(composer.verticalScrollBar()->maximum()>0);
    composer.setUiScale(1);QTRY_COMPARE(composer.height(),256);
    composer.clear();QTRY_VERIFY(composer.height()<=100);
    QTRY_COMPARE(composer.verticalScrollBar()->maximum(),0);
}
void UiTest::encryptedProviderStoreRejectsStaleAndCorruptWrites(){
    QTemporaryDir directory;const auto path=directory.path()+"/providers.json";
    ProviderConfig p;p.id="fixture";p.name="Fixture";p.model="a";p.apiKey="fake-key";
    ProviderRepository a(path),b(path);QString error;QVERIFY(a.save({p},&error));QCOMPARE(b.load(&error).size(),1);
    p.model="updated";QVERIFY(a.save({p},&error));p.model="stale";QVERIFY(!b.save({p},&error));QVERIFY(error.contains("changed"));
    QCOMPARE(ProviderStore::load(path).first().model,QString("updated"));
    QFile corrupt(path);QVERIFY(corrupt.open(QIODevice::WriteOnly));corrupt.write("{broken encrypted providers");corrupt.close();
    QVERIFY(a.load(&error).isEmpty());QVERIFY(!error.isEmpty());QVERIFY(!a.save({p},&error));
    QVERIFY(corrupt.open(QIODevice::ReadOnly));QCOMPARE(corrupt.readAll(),QByteArray("{broken encrypted providers"));
}
void UiTest::selectionsKeepInstanceAndModelIdentity(){
    ProviderConfig a;a.id="a";a.name="Provider A";a.model="default";a.baseUrl="https://example.com/v1";a.apiKey="private-value";
    auto b=a;b.id="b";b.name="Provider B";
    const QJsonArray models{QJsonObject{{"id","shared"},{"name","Shared model"},{"metadata",QJsonObject{{"apiKey","never-copy"}}}},QJsonObject{{"id","shared"}},QJsonObject{{"id","new-model"}}};
    const auto catalog=cleanModelCatalog(a,models);QCOMPARE(catalog["models"].toArray().size(),2);
    QVERIFY(!QJsonDocument(catalog).toJson().contains("never-copy"));QVERIFY(!QJsonDocument(catalog).toJson().contains("private-value"));
    const QJsonObject preset{{"id","preset"},{"name","Careful"},{"providerInstanceId","b"},{"modelId","shared"},{"overrides",QJsonObject{{"reasoningEffort","high"}}}};
    const auto selections=availableModelSelections({a,b},{{"a",catalog},{"b",catalog}},{preset});
    QCOMPARE(selections.size(),7);QCOMPARE(selections.first().presetId,QString("preset"));
    ProviderConfig resolved;QVERIFY(selections.first().resolve({a,b},&resolved));QCOMPARE(resolved.id,QString("b"));QCOMPARE(resolved.model,QString("shared"));QCOMPARE(resolved.reasoningEffort,QString("high"));
    QCOMPARE(b.model,QString("default"));QCOMPARE(b.reasoningEffort,QString());
    b.baseUrl="https://other.example/v1";
    QCOMPARE(availableModelSelections({a,b},{{"a",catalog},{"b",catalog}},{}).size(),4);
}
void UiTest::sendStreamsAndPersists(){
    QTemporaryDir data,workspace;QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
    bool requestObserved=false;QJsonObject request;
    connect(&server,&QTcpServer::newConnection,&server,[&]{while(server.hasPendingConnections()){
        auto* socket=server.nextPendingConnection();auto buffer=std::make_shared<QByteArray>();
        connect(socket,&QTcpSocket::readyRead,socket,[&,socket,buffer]{
            *buffer+=socket->readAll();const int head=buffer->indexOf("\r\n\r\n");if(head<0)return;
            int length=0;for(const auto& line:buffer->left(head).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
            if(buffer->size()<head+4+length||socket->property("replied").toBool())return;
            socket->setProperty("replied",true);if(!requestObserved)request=QJsonDocument::fromJson(buffer->mid(head+4,length)).object();requestObserved=true;
            const QByteArray body="data: {\"choices\":[{\"delta\":{\"content\":\"Native UI response\"},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
        });connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    }});
    ProviderConfig config;config.id="fixture";config.name="Local fixture";config.model="provider-default-changed";config.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());config.apiKey="fixture-key-not-real";
    QString error;QVERIFY2(ProviderStore::save(data.path()+"/providers.json",{config},&error),qPrintable(error));
    Conversation session;session.id="saved-model-session";session.title="Saved model";session.providerId=config.id;session.model="fixture-model";session.presetId="careful";
    QVERIFY(SessionStore(data.path()).save({session},&error));
    SettingsPage settings(data.path());
    QVERIFY(settings.setPreference("modelPresets",QJsonArray{QJsonObject{{"id","careful"},{"name","Careful"},{"providerInstanceId","fixture"},{"modelId","fixture-model"},{"overrides",QJsonObject{{"reasoningEffort","high"}}}}}));
    QFile instructions(workspace.filePath("AGENTS.md"));QVERIFY(instructions.open(QIODevice::WriteOnly));instructions.write("Workspace fixture instructions: CONTEXT_WIRED_8371");instructions.close();
    MainWindow window(data.path());window.connectWorkspace(workspace.path());
    auto* input=window.findChild<Composer*>("composerInput");QVERIFY(input);input->setPlainText("Verify native UI");
    QTest::keyClick(input,Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(requestObserved,5000);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ConversationView*>("conversationView")->toPlainText().contains("Native UI response"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QToolButton*>("stopButton")->isHidden(),5000);
    QVERIFY(!window.findChild<QToolButton*>("sendButton")->isEnabled());
    auto stored=SessionStore(data.path()).load();QCOMPARE(stored.size(),1);QCOMPARE(stored[0].messages.size(),2);
    QCOMPARE(stored[0].messages.last().toObject()["content"].toString(),QString("Native UI response"));
    QCOMPARE(request["model"].toString(),QString("fixture-model"));
    QCOMPARE(request["reasoning_effort"].toString(),QString("high"));
    QCOMPARE(stored[0].presetId,QString("careful"));
    const auto outgoing=request["messages"].toArray();
    // Upstream message-conversion.ts wraps user text in <user-msg> CDATA; the OpenAI-compatible
    // chat converter sends a single text part as plain string content.
    QCOMPARE(outgoing.last().toObject()["role"].toString(),QString("user"));
    QVERIFY2(outgoing.last().toObject()["content"].isString(),QJsonDocument(QJsonArray{outgoing.last()}).toJson(QJsonDocument::Compact).right(3000).constData());
    QCOMPARE(outgoing.last().toObject()["content"].toString(),QString("<user-msg><![CDATA[Verify native UI]]></user-msg>"));
    QVERIFY(QJsonDocument(outgoing).toJson().contains("CONTEXT_WIRED_8371"));
    QCOMPARE(input->toPlainText(),QString());
}
void UiTest::browserPreferencesAndDownloadsReachNativeHost(){
    QTemporaryDir data;MainWindow window(data.path());auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings);
    auto* openBrowser=window.findChild<QToolButton*>("openBrowserButton");QVERIFY(openBrowser);openBrowser->click();
    auto* browser=window.findChild<BrowserWidget*>();QVERIFY(browser);
    QTemporaryDir appRoot;QFile appHtml(appRoot.filePath("index.html"));QVERIFY(appHtml.open(QIODevice::WriteOnly));appHtml.write("<!doctype html><p>Local app</p>");appHtml.close();
    auto* miniApp=new MiniAppPage(QUrl("app://agents/fixture/demo"),appRoot.path(),data.filePath("app-profile"),&window);QVERIFY(miniApp->configured());
    miniApp->browser()->setProperty("miniAppBrowser",true);miniApp->browser()->setProperty("adeOwnerAgentId","fixture");
    QCOMPARE(miniApp->browser()->permissionSetting(QUrl("app://agents/fixture/demo"),"notifications"),QString("block"));
    QVERIFY(settings->setSitePermission("location",QUrl("https://example.test"),"block"));
    QCOMPARE(browser->permissionSetting(QUrl("https://example.test"),"geolocation"),QString("block"));
    QVERIFY(settings->setPreference("browsing.permissions.notifications","allow"));
    QCOMPARE(browser->permissionSetting(QUrl("https://another.test"),"notifications"),QString("allow"));
    QCOMPARE(miniApp->browser()->permissionSetting(QUrl("app://agents/fixture/demo"),"notifications"),QString("block"));
    auto* tabs=window.findChild<QTabWidget*>("contentTabs");QVERIFY(tabs);
    emit settings->openUrlRequested(QUrl("https://example.test/from-history"));QCOMPARE(tabs->count(),2);
    auto* historyBrowser=tabs->currentWidget()->findChild<BrowserWidget*>();QVERIFY(historyBrowser);
    QCOMPARE(historyBrowser->permissionSetting(QUrl("https://example.test"),"geolocation"),QString("block"));
    QToolButton* downloadsButton=nullptr;for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Downloads (Ctrl+J)")downloadsButton=button;QVERIFY(downloadsButton);
    downloadsButton->click();QPointer<DownloadsPage> downloads=window.findChild<DownloadsPage*>();QVERIFY(downloads);QCOMPARE(tabs->currentWidget(),downloads.data());
    emit tabs->tabCloseRequested(tabs->currentIndex());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(downloads);
    downloadsButton->click();QCOMPARE(tabs->currentWidget(),downloads.data());
}

QTEST_MAIN(UiTest)
#include "test_ui.moc"
