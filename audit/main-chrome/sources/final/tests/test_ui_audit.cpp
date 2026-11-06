#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/ModelSelection.h"
#include "ui/ProviderRepository.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "ui/SettingsPage.h"
#include "editor/CodeEditor.h"
#include "core/ProviderStore.h"
#include "attachments/AttachmentStore.h"
#include "runtime/AgentToolHost.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"

#include <QtTest>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QListWidget>
#include <QToolButton>
#include <QMimeData>
#include <QTextBrowser>
#include <QFontInfo>
#include <QTabWidget>
#include <QRegularExpression>

using namespace ade;
namespace {
ProviderConfig provider(const QString &id){
    ProviderConfig p;p.id=id;p.name="Provider "+id;p.baseUrl="https://example.test/v1";p.model="model-"+id;p.apiKey="fixture-not-real";return p;
}
QByteArray bytes(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
}

class UiAuditTest : public QObject {
    Q_OBJECT
private slots:
    void blockedDraftRestoreStillUpdatesComposerHeight();
    void newDraftRetainsInheritedModelAfterRestart();
    void unmatchedModelSearchCannotSendPreviousSelection();
    void malformedCredentialRecordCannotBeOverwritten();
    void failedDraftSavePreventsSilentWindowClose();
    void attachmentDraftsStayWithChatAndSurviveRestart();
    void clipboardAndDropMimeBecomeAttachments();
    void invalidAttachmentDraftStorageIsPreserved();
    void failedSendSaveKeepsDraftAndDoesNotRequestProvider();
    void attachmentOnlyMessageReachesProviderAndKeepsReferences();
    void missingAttachmentKeepsDraftWithoutSending();
    void changingWorkspacePreservesSelectedDraftFile();
    void applicationStylePreservesCodeFontAndZoom();
    void appThemeAndManualClosureFollowNativeWindow();
    void liveAppWindowTeardownPreservesOwner();
};

void UiAuditTest::blockedDraftRestoreStillUpdatesComposerHeight(){
    Composer input;input.resize(500,74);input.setPlainText(QString("line\n").repeated(20));
    QVERIFY(input.height()>100);
    {
        // MainWindow blocks Composer signals while restoring another chat's draft.
        QSignalBlocker blocker(&input);input.setPlainText("");
    }
    QTRY_VERIFY(input.height()<=100);
    {
        QSignalBlocker blocker(&input);input.setPlainText(QString("line\n").repeated(20));
    }
    QTRY_VERIFY(input.height()>100);QVERIFY(input.height()<=256);
}

void UiAuditTest::newDraftRetainsInheritedModelAfterRestart(){
    QTemporaryDir directory;const auto a=provider("a"),b=provider("b");
    QVERIFY(ProviderStore::save(directory.filePath("providers.json"),{a,b}));
    Conversation original;original.id="original";original.title="Original";original.providerId=b.id;original.model=b.model;
    QVERIFY(SessionStore(directory.path()).save({original}));
    QString freshId;
    {
        MainWindow window(directory.path());auto *models=window.findChild<QComboBox *>("modelSelector");QVERIFY(models);
        QCOMPARE(ModelSelection::fromJson(models->currentData().toJsonObject()).providerId,b.id);
        window.findChild<QPushButton *>("newAgentButton")->click();freshId=window.currentSessionId();QVERIFY(freshId!=original.id);
        QCOMPARE(ModelSelection::fromJson(models->currentData().toJsonObject()).providerId,b.id);
        window.findChild<Composer *>("composerInput")->setPlainText("Draft for the visible B model");QVERIFY(window.close());
    }
    MainWindow reopened(directory.path());QCOMPARE(reopened.currentSessionId(),freshId);
    auto *models=reopened.findChild<QComboBox *>("modelSelector");
    const auto selected=ModelSelection::fromJson(models->currentData().toJsonObject());
    QCOMPARE(selected.providerId,b.id);QCOMPARE(selected.modelId,b.model);
    QCOMPARE(reopened.findChild<Composer *>("composerInput")->toPlainText(),QString("Draft for the visible B model"));
}

void UiAuditTest::unmatchedModelSearchCannotSendPreviousSelection(){
    QTemporaryDir directory;QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
    auto p=provider("fixture");p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
    QVERIFY(ProviderStore::save(directory.filePath("providers.json"),{p}));
    MainWindow window(directory.path());auto *models=window.findChild<QComboBox *>("modelSelector");QVERIFY(models);
    models->setEditText("nonexistent search result");QSignalSpy requests(&server,&QTcpServer::newConnection);
    window.submitPrompt("Retain this draft until I select a model");QTest::qWait(150);
    QCOMPARE(requests.count(),0);
    QCOMPARE(window.findChild<Composer *>("composerInput")->toPlainText(),QString("Retain this draft until I select a model"));
    QVERIFY(SessionStore(directory.path()).load().first().messages.isEmpty());
}

void UiAuditTest::malformedCredentialRecordCannotBeOverwritten(){
    QTemporaryDir directory;const auto path=directory.filePath("providers.json");
    QJsonObject record=provider("fixture").publicJson();record["encryptedApiKey"]="not valid base64!";
    const auto original=QJsonDocument(QJsonObject{{"version",1},{"providers",QJsonArray{record}}}).toJson();
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),original.size());file.close();
    ProviderRepository repository(path);QString error;QVERIFY(repository.load(&error).isEmpty());QVERIFY(!error.isEmpty());
    QVERIFY(!repository.save({provider("replacement")},&error));QCOMPARE(bytes(path),original);
}

void UiAuditTest::failedDraftSavePreventsSilentWindowClose(){
    QTemporaryDir directory;MainWindow window(directory.path());
    auto *input=window.findChild<Composer *>("composerInput");QVERIFY(input);input->setPlainText("Unsaved draft must remain recoverable");
    const auto path=directory.filePath("sessions.json");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
    const QByteArray external="{external-corruption-fixture";file.write(external);file.close();
    QVERIFY(!window.close());
    QCOMPARE(input->toPlainText(),QString("Unsaved draft must remain recoverable"));QCOMPARE(bytes(path),external);
}

void UiAuditTest::attachmentDraftsStayWithChatAndSurviveRestart(){
    QTemporaryDir data,files;QFile source(files.filePath("notes & context.txt"));
    QVERIFY(source.open(QIODevice::WriteOnly));source.write("attachment-draft-marker");source.close();
    QString firstId;QJsonArray refs;
    {
        MainWindow window(data.path());firstId=window.currentSessionId();auto* input=window.findChild<Composer*>();
        emit input->filesAttached({source.fileName()});
        refs=SessionStore(data.path()).load().first().draftAttachments;QCOMPARE(refs.size(),1);
        QVERIFY(refs.first().toObject()["path"].toString().startsWith("att/"));
        QCOMPARE(input->toPlainText(),QString());
        window.findChild<QPushButton*>("newAgentButton")->click();
        QVERIFY(SessionStore(data.path()).load().first().draftAttachments.isEmpty());
        window.findChild<QListWidget*>("agentList")->setCurrentRow(1);QCOMPARE(window.currentSessionId(),firstId);
        QVERIFY(window.close());
    }
    MainWindow reopened(data.path());reopened.findChild<QListWidget*>("agentList")->setCurrentRow(1);
    QCOMPARE(reopened.currentSessionId(),firstId);
    QString error;const auto ref=AttachmentRef::fromJson(refs.first().toObject(),&error);
    QCOMPARE(AttachmentStore(data.path()).read(firstId,ref,{},&error),QByteArray("attachment-draft-marker"));QVERIFY(error.isEmpty());
    QToolButton* remove=nullptr;for(auto* button:reopened.findChildren<QToolButton*>("removeDraftAttachment"))if(button->isEnabled())remove=button;
    QVERIFY(remove);remove->click();
    const auto sessions=SessionStore(data.path()).load();for(const auto& session:sessions)if(session.id==firstId)QVERIFY(session.draftAttachments.isEmpty());
}

void UiAuditTest::clipboardAndDropMimeBecomeAttachments(){
    class TestComposer:public Composer{public:using Composer::insertFromMimeData;};
    TestComposer input;QSignalSpy files(&input,&Composer::filesAttached),images(&input,&Composer::imageAttached);
    QMimeData urls;urls.setUrls({QUrl::fromLocalFile("C:/fixture/image.png")});input.insertFromMimeData(&urls);
    QCOMPARE(files.size(),1);QCOMPARE(files.first().first().toStringList(),QStringList{"C:/fixture/image.png"});
    QImage image(2,2,QImage::Format_ARGB32);image.fill(Qt::red);QMimeData picture;picture.setImageData(image);input.insertFromMimeData(&picture);
    QCOMPARE(images.size(),1);QCOMPARE(qvariant_cast<QImage>(images.first().first()),image);QVERIFY(input.toPlainText().isEmpty());
    QMimeData text;text.setText("ordinary pasted text");input.insertFromMimeData(&text);QCOMPARE(input.toPlainText(),QString("ordinary pasted text"));
}

void UiAuditTest::invalidAttachmentDraftStorageIsPreserved(){
    QTemporaryDir data;const auto path=data.filePath("sessions.json");
    const QJsonObject invalid{{"id","draft"},{"title","Draft"},{"messages",QJsonArray{}},{"draftAttachments",QJsonArray{QJsonObject{{"path",17}}}}};
    const auto original=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",QJsonArray{invalid}}}).toJson();
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(original);file.close();
    SessionStore store(data.path());QString error;QVERIFY(store.load(&error).isEmpty());QVERIFY(!error.isEmpty());
    QVERIFY(!store.save({},&error));QCOMPARE(bytes(path),original);
}

void UiAuditTest::failedSendSaveKeepsDraftAndDoesNotRequestProvider(){
    QTemporaryDir data;QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
    auto p=provider("fixture");p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
    QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));MainWindow window(data.path());
    QFile file(data.filePath("sessions.json"));QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write("external-change");file.close();
    QSignalSpy requests(&server,&QTcpServer::newConnection);window.submitPrompt("Keep this unsaved prompt");QTest::qWait(100);
    QCOMPARE(requests.size(),0);QCOMPARE(window.findChild<Composer*>()->toPlainText(),QString("Keep this unsaved prompt"));
    QVERIFY(!window.findChild<QTextBrowser*>("conversationView")->toPlainText().contains("Keep this unsaved prompt"));
}

void UiAuditTest::attachmentOnlyMessageReachesProviderAndKeepsReferences(){
    QTemporaryDir data;QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
    auto p=provider("fixture");p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));
    QString error;const auto ref=AttachmentStore(data.path()).write("attachment-only","notes.txt","COMPOSER_ATTACHMENT_68294",&error);QVERIFY2(error.isEmpty(),qPrintable(error));
    Conversation session;session.id="attachment-only";session.title="Draft";session.providerId=p.id;session.model=p.model;session.titleLocked=true;session.draftAttachments={ref.toJson()};
    QVERIFY(SessionStore(data.path()).save({session}));QJsonObject request;
    connect(&server,&QTcpServer::newConnection,&server,[&]{
        auto* socket=server.nextPendingConnection();connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
            auto input=socket->property("input").toByteArray()+socket->readAll();socket->setProperty("input",input);const auto head=input.indexOf("\r\n\r\n");if(head<0||socket->property("sent").toBool())return;
            int length=0;for(const auto& line:input.left(head).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();if(input.size()<head+4+length)return;
            socket->setProperty("sent",true);request=QJsonDocument::fromJson(input.mid(head+4,length)).object();
            const QByteArray body="data: {\"choices\":[{\"delta\":{\"content\":\"Attachment received\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();
        });
    });
    MainWindow window(data.path());window.findChild<QToolButton*>("sendButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!request.isEmpty(),5000);QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QToolButton*>("stopButton")->isHidden(),5000);
    QVERIFY(!window.findChild<QToolButton*>("sendButton")->isEnabled());
    QVERIFY(QJsonDocument(request).toJson().contains("COMPOSER_ATTACHMENT_68294"));
    const auto saved=SessionStore(data.path()).load().first();QCOMPARE(saved.messages.size(),2);QVERIFY(saved.draftAttachments.isEmpty());
    QCOMPARE(saved.messages.first().toObject()["metadata"].toObject()["attachments"].toArray(),session.draftAttachments);
    const auto captured=saved.messages.first().toObject()["metadata"].toObject()["pathMetadata"].toObject()[ref.path].toObject();
    QCOMPARE(captured["size"].toInteger(),qint64(QByteArray("COMPOSER_ATTACHMENT_68294").size()));
    QVERIFY(QDateTime::fromString(captured["modified"].toString(),Qt::ISODateWithMs).isValid());
    QVERIFY(!QJsonDocument(request).toJson().contains("\"pathMetadata\""));
    QVERIFY(!QJsonDocument(saved.messages).toJson().contains("COMPOSER_ATTACHMENT_68294"));
    QVERIFY(window.findChild<QTextBrowser*>("conversationView")->toPlainText().contains("notes.txt"));
}

void UiAuditTest::missingAttachmentKeepsDraftWithoutSending(){
    QTemporaryDir data;QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));auto p=provider("fixture");
    p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));
    Conversation session;session.id="missing";session.title="Draft";session.draft="Keep missing attachment";session.providerId=p.id;session.model=p.model;session.draftAttachments={QJsonObject{{"path","att/missing.txt"}}};
    QVERIFY(SessionStore(data.path()).save({session}));MainWindow window(data.path());QSignalSpy requests(&server,&QTcpServer::newConnection);window.findChild<QToolButton*>("sendButton")->click();QTest::qWait(100);
    QCOMPARE(requests.size(),0);QCOMPARE(window.findChild<Composer*>()->toPlainText(),session.draft);
    const auto saved=SessionStore(data.path()).load().first();QCOMPARE(saved.draftAttachments,session.draftAttachments);QVERIFY(saved.messages.isEmpty());
}

void UiAuditTest::changingWorkspacePreservesSelectedDraftFile(){
    QTemporaryDir data,a,b;QFile original(a.filePath("selected.txt")),other(b.filePath("selected.txt"));
    QVERIFY(original.open(QIODevice::WriteOnly));original.write("FILE_A_SELECTED");original.close();
    QVERIFY(other.open(QIODevice::WriteOnly));other.write("FILE_B_NOT_SELECTED");other.close();
    MainWindow window(data.path());window.connectWorkspace(a.path());emit window.findChild<Composer*>()->filesAttached({original.fileName()});
    const auto before=SessionStore(data.path()).load().first();QVERIFY(before.draftAttachments.first().toObject()["path"].toString().startsWith("ws1/"));
    window.connectWorkspace(b.path());const auto after=SessionStore(data.path()).load().first();
    const auto ref=AttachmentRef::fromJson(after.draftAttachments.first().toObject());QVERIFY(ref.path.startsWith("att/"));QString error;
    QCOMPARE(AttachmentStore(data.path()).read(after.id,ref,{},&error),QByteArray("FILE_A_SELECTED"));QVERIFY(error.isEmpty());
}

void UiAuditTest::applicationStylePreservesCodeFontAndZoom(){
    const auto oldStyle=qApp->styleSheet();const auto oldFont=qApp->font();
    initializeAppStyle(*qApp);CodeEditor editor;editor.ensurePolished();
    const bool monospace=QFontInfo(editor.font()).family()=="Roboto Mono";
    const int initial=editor.font().pixelSize();editor.setEditorZoom(150);const int enlarged=editor.font().pixelSize();
    qApp->setStyleSheet(oldStyle);qApp->setFont(oldFont);
    QVERIFY(monospace);QCOMPARE(initial,12);QCOMPARE(enlarged,18);
}

void UiAuditTest::appThemeAndManualClosureFollowNativeWindow(){
    QTemporaryDir data,apps;QVERIFY(QDir().mkpath(apps.filePath("demo")));
    QFile html(apps.filePath("demo/index.html"));QVERIFY(html.open(QIODevice::WriteOnly));
    html.write(R"HTML(<!doctype html><style>body{background:var(--color-background);color:var(--color-foreground)}</style><body>Theme fixture<script>
      addEventListener('message',e=>{if(e.data?.type!=='__stagewise_theme')return;
        for(const [name,value] of Object.entries(e.data.variables))document.documentElement.style.setProperty(name,value);
        parent.postMessage({background:getComputedStyle(document.body).backgroundColor,foreground:getComputedStyle(document.body).color,accent:getComputedStyle(document.documentElement).getPropertyValue('--color-primary-solid').trim()},'*');
      });</script></body>)HTML");html.close();
    auto p=provider("ui-app-fixture");p.baseUrl="http://127.0.0.1:1/v1";QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));
    Conversation session;session.id="ui-app";session.title="App fixture";session.providerId=p.id;session.model=p.model;
    // A missing draft attachment creates the actual per-chat host but stops the
    // send before any network request; no API key or provider service is used.
    session.draftAttachments={QJsonObject{{"path","att/missing-fixture.txt"}}};QVERIFY(SessionStore(data.path()).save({session}));
    MainWindow window(data.path());window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,720);window.show();
    auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings);QVERIFY(settings->setPreference("personalization.appearance","dark"));
    window.submitPrompt("Initialize the fixture host");
    auto* host=window.findChild<AgentToolHost*>();QVERIFY(host);
    const auto appRoot=QDir(host->apps()->agentAppsDirectory()).filePath("demo");QVERIFY(QDir().mkpath(appRoot));QVERIFY(QFile::copy(html.fileName(),appRoot+"/index.html"));
    QSignalSpy messages(host->apps(),&MiniAppHost::messageReceived);
    host->apps()->execute("ui-open","open",{{"appId","demo"}});QVERIFY(host->apps()->activePage());
    const QPointer<MiniAppPage> page=host->apps()->activePage();
    const auto tabId=host->apps()->activeTabId();QVERIFY(!tabId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(messages.size(),1,15000);
    const auto colors=[&]{return messages.last()[2].toJsonValue().toObject();};
    const auto cssColor=[](const QJsonValue& value){const auto match=QRegularExpression("^rgb\\((\\d+),\\s*(\\d+),\\s*(\\d+)\\)$").match(value.toString());return match.hasMatch()?QColor(match.captured(1).toInt(),match.captured(2).toInt(),match.captured(3).toInt()):QColor();};
    QCOMPARE(cssColor(colors()["background"]),qApp->palette().color(QPalette::Base));
    QCOMPARE(cssColor(colors()["foreground"]),qApp->palette().color(QPalette::Text));
    QVERIFY(cssColor(colors()["background"]).lightness()<128);
    QVERIFY(settings->setPreference("personalization.appearance","light"));QTRY_COMPARE_WITH_TIMEOUT(messages.size(),2,4000);
    QCOMPARE(cssColor(colors()["background"]),qApp->palette().color(QPalette::Base));
    QVERIFY(cssColor(colors()["background"]).lightness()>128);
    QVERIFY(settings->setPreference("personalization.theme","green"));QTRY_COMPARE_WITH_TIMEOUT(messages.size(),3,4000);
    QCOMPARE(colors()["accent"].toString(),"#348a67");
    host->apps()->execute("ui-reload","open",{{"appId","demo"}});QCOMPARE(host->apps()->activeTabId(),tabId);
    QTRY_COMPARE_WITH_TIMEOUT(messages.size(),4,15000);QCOMPARE(colors()["accent"].toString(),"#348a67");
    auto* tabs=window.findChild<QTabWidget*>();auto* bridge=window.findChild<BrowserToolBridge*>();QVERIFY(tabs);QVERIFY(bridge);
    const int index=tabs->indexOf(page);QVERIFY(index>=0);QVERIFY(bridge->canAccessTab(tabId,host->agentId()));
    emit tabs->tabCloseRequested(index);
    QVERIFY(!host->apps()->activePage());QVERIFY(!bridge->canAccessTab(tabId,host->agentId()));
    QVERIFY(page); // Logical ownership is gone before deferred widget deletion.
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!page);
    const int tabsBefore=tabs->count();bool revoke=true;
    connect(bridge,&BrowserToolBridge::tabsChanged,&window,[&]{
        if(revoke&&host->apps()->activePage()){revoke=false;host->apps()->reset();}
    });
    host->apps()->execute("ui-revoked-open","open",{{"appId","demo"}});
    QVERIFY(!revoke);QVERIFY(!host->apps()->activePage());
    QCOMPARE(tabs->count(),tabsBefore); // No stale tab is added after ownership is revoked during registration.
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    int changes=0;bool revokeActivation=true;
    connect(bridge,&BrowserToolBridge::tabsChanged,&window,[&]{
        if(revokeActivation&&host->apps()->activePage()&&++changes==2){revokeActivation=false;host->apps()->reset();}
    });
    host->apps()->execute("ui-revoked-activation","open",{{"appId","demo"}});
    QVERIFY(!revokeActivation);QVERIFY(!host->apps()->activePage());QCOMPARE(tabs->count(),tabsBefore);
}

void UiAuditTest::liveAppWindowTeardownPreservesOwner(){
    for(int iteration=0;iteration<3;++iteration){
        QTemporaryDir data;auto p=provider("teardown-fixture");p.baseUrl="http://127.0.0.1:1/v1";
        QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));
        Conversation session;session.id="teardown";session.title="App teardown";session.providerId=p.id;session.model=p.model;
        session.draftAttachments={QJsonObject{{"path","att/missing-fixture.txt"}}};QVERIFY(SessionStore(data.path()).save({session}));
        QPointer<MiniAppPage> page;QPointer<BrowserWidget> browser;
        {
            MainWindow window(data.path());window.setAttribute(Qt::WA_DontShowOnScreen);window.show();window.submitPrompt("Initialize local host");
            auto* host=window.findChild<AgentToolHost*>();QVERIFY(host);
            const auto root=QDir(host->apps()->agentAppsDirectory()).filePath("demo");QVERIFY(QDir().mkpath(root));
            QFile file(root+"/index.html");QVERIFY(file.open(QIODevice::WriteOnly));file.write("<!doctype html><title>Owned fixture</title><p>Native teardown fixture</p>");file.close();
            host->apps()->execute("open","open",{{"appId","demo"}});page=host->apps()->activePage();QVERIFY(page);browser=page->browser();QVERIFY(browser);
            QTRY_VERIFY_WITH_TIMEOUT(page->isReady(),15000);
            // Leave the actual app open. MainWindow must destroy the owner
            // before its explicitly owned browser, retaining destroyed hooks.
        }
        QVERIFY(!page);QVERIFY(!browser);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
}

QTEST_MAIN(UiAuditTest)
#include "test_ui_audit.moc"
