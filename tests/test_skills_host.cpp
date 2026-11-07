#include "ui/MainWindow.h"
#include "ui/Composer.h"
#include "ui/SessionStore.h"
#include "ui/SideChatPane.h"
#include "ui/AppStyle.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "core/ContextLoader.h"
#include <memory>
#include <QFrame>
#include <functional>
#include "attachments/AttachmentStore.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QtTest>
#include <QApplication>
#include <QJsonDocument>
#include <QListWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
using namespace ade;
namespace {
bool put(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray eventData(const QJsonObject& object){return "data: "+QJsonDocument(object).toJson(QJsonDocument::Compact)+"\n\n";}
QByteArray response(const QString& protocol){
    if(protocol=="responses")return eventData({{"type","response.completed"},{"response",QJsonObject{{"status","completed"},{"output",QJsonArray{QJsonObject{{"type","message"},{"role","assistant"},{"content",QJsonArray{QJsonObject{{"type","output_text"},{"text","DONE"}}}}}}}}}});
    if(protocol=="google")return eventData({{"candidates",QJsonArray{QJsonObject{{"content",QJsonObject{{"role","model"},{"parts",QJsonArray{QJsonObject{{"text","DONE"}}}}}},{"finishReason","STOP"}}}}});
    if(protocol=="anthropic")return eventData({{"type","content_block_start"},{"index",0},{"content_block",QJsonObject{{"type","text"},{"text","DONE"}}}})+eventData({{"type","message_delta"},{"delta",QJsonObject{{"stop_reason","end_turn"}}}})+eventData({{"type","message_stop"}});
    return eventData({{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"content","DONE"}}},{"finish_reason","stop"}}}}})+"data: [DONE]\n\n";
}

class HideObserver:public QObject {
public:
    std::function<bool()> action;
    bool eventFilter(QObject*,QEvent* e)override{if(e->type()!=QEvent::Hide||!action)return false;const auto callback=std::move(action);return callback();}
};
class Model : public QTcpServer {
public:
    QList<QJsonObject> requests;
    QString protocol="openai";
    Model() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("done").toBool()) return;
                    const auto bytes = socket->property("bytes").toByteArray() + socket->readAll();
                    socket->setProperty("bytes", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int size = 0;
                    for (const auto &line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + size) return;
                    socket->setProperty("done", true);
                    requests.append(QJsonDocument::fromJson(bytes.mid(end + 4, size)).object());
                    const auto body=response(protocol);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"+body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};
bool seed(const QString &data, const QString &workspace, quint16 port,const QString& protocol="openai") {
    Conversation c;
    c.id = "owner"; c.title = "Owner"; c.titleLocked = true; c.workspace = workspace;
    c.providerId = "fixture"; c.model = "fixture-model";
    ProviderConfig provider;
    provider.id = c.providerId; provider.name = "Fixture"; provider.model = c.model;
    provider.protocol=protocol;
    provider.apiKey = "fixture-only-no-network-key";
    provider.baseUrl = QString("http://127.0.0.1:%1/v1").arg(port);
    return SessionStore(data).save({c}) && ProviderStore::save(QDir(data).filePath("providers.json"), {provider});
}
void expose(MainWindow &window) {window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1000,700); window.show();}
void type(Composer *composer, const QString &text) {composer->setFocus(); composer->setPlainText(text); composer->moveCursor(QTextCursor::End);}
int choice(Composer *composer, const QString &id) {
    auto *list = composer->findChild<QListWidget*>("composerCompletionList");
    if (!list) return -1;
    for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole).toString() == id) return i;
    return -1;
}
void activate(Composer *composer, const QString &id) {
    auto *list = composer->findChild<QListWidget*>("composerCompletionList"); const int row = choice(composer,id);
    if (!list || row < 0) return;
    list->setCurrentRow(row); QTest::keyClick(composer,Qt::Key_Return);
}
bool idle(MainWindow &window) {for (const auto *engine : window.findChildren<AgentEngine*>()) if (engine->isBusy()) return false; return true;}
QByteArray encoded(const QJsonObject &value) {return QJsonDocument(value).toJson(QJsonDocument::Compact);}
}
class SkillsHostTest : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    QByteArray oldHome;
private slots:
    void initTestCase() {oldHome=qgetenv("USERPROFILE");qputenv("USERPROFILE",home.path().toUtf8());initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
    void cleanupTestCase() {qputenv("USERPROFILE",oldHome);}
    void skillResolvesAgainOnSecondTurn_data(){QTest::addColumn<QString>("protocol");for(const auto& protocol:QStringList{"openai","responses","anthropic","google"})QTest::newRow(qPrintable(protocol))<<protocol;}
    void skillResolvesAgainOnSecondTurn() {
        QFETCH(QString,protocol);
        QTemporaryDir data, workspace; Model model; model.protocol=protocol; QVERIFY(model.isListening());
        QVERIFY(put(workspace.filePath(".stagewise/skills/fixture/SKILL.md"),"---\nname: fixture\ndescription: A fixture skill\n---\nSKILL_BODY_SENTINEL\n"));
        QVERIFY(seed(data.path(),workspace.path(),model.serverPort(),protocol));
        MainWindow window(data.path());expose(window);auto *composer=window.findChild<Composer*>();
        type(composer,"/fixture");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"skill:fixture")>=0,5000);activate(composer,"skill:fixture");
        window.findChild<QToolButton*>("sendButton")->click();QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(idle(window),5000);
        const auto first=encoded(model.requests[0]);
        QCOMPARE(first.count("SKILL_BODY_SENTINEL"),1);QVERIFY(first.contains("<slash-command"));QVERIFY(first.contains("<user-msg>"));
        QVERIFY(!first.contains("<invoked_skill"));QVERIFY(!first.contains("slash:skill"));QVERIFY(!first.contains("name: fixture"));
        type(composer,"Continue");window.findChild<QToolButton*>("sendButton")->click();QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),2,5000);QTRY_VERIFY_WITH_TIMEOUT(idle(window),5000);
        QVERIFY2(encoded(model.requests[1]).contains("SKILL_BODY_SENTINEL"),"Earlier accepted skill invocation vanished from second provider request");
        QCOMPARE(encoded(model.requests[1]).count("SKILL_BODY_SENTINEL"),1);
        auto saved=SessionStore(data.path()).load();QCOMPARE(saved.size(),1);
        const auto history=saved.first().messages;QVERIFY(history.first().toObject()["content"].toString().contains("slash:skill%3Afixture"));
        QVERIFY(!QJsonDocument(history).toJson().contains("SKILL_BODY_SENTINEL"));
    }
    void restartAndForkResolveCurrentSkillWithoutSnapshottingBody() {
        QTemporaryDir data,workspace;Model model;QVERIFY(put(workspace.filePath(".stagewise/skills/fixture/SKILL.md"),"---\nname: fixture\ndescription: Fixture\n---\nBEFORE_RESTART_BODY"));QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));
        auto window=std::make_unique<MainWindow>(data.path());expose(*window);window->submitPrompt("[fixture](slash:skill:fixture) initial");QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(idle(*window),5000);window.reset();
        QVERIFY(put(workspace.filePath(".stagewise/skills/fixture/SKILL.md"),"---\nname: fixture\ndescription: Fixture\n---\nAFTER_RESTART_BODY"));
        window=std::make_unique<MainWindow>(data.path());expose(*window);window->submitPrompt("Continue after restart");QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),2,5000);QTRY_VERIFY_WITH_TIMEOUT(idle(*window),5000);
        QVERIFY(encoded(model.requests[1]).contains("AFTER_RESTART_BODY"));QVERIFY(!encoded(model.requests[1]).contains("BEFORE_RESTART_BODY"));
        auto* composer=window->findChild<Composer*>();type(composer,"/side");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);activate(composer,"command:side");QTRY_COMPARE_WITH_TIMEOUT(window->findChildren<SideChatPane*>().size(),1,5000);
        auto* side=window->findChild<SideChatPane*>();type(side->composer(),"Fork continuation");side->findChild<QToolButton*>("sideChatSendButton")->click();QTRY_VERIFY_WITH_TIMEOUT(model.requests.size()>=3,5000);QTRY_VERIFY_WITH_TIMEOUT(idle(*window),5000);
        QJsonObject forkRequest;int streamed=0;for(const auto& request:model.requests)if(request.value("stream").toBool()){++streamed;if(encoded(request).contains("Fork continuation"))forkRequest=request;}
        QCOMPARE(streamed,3);QVERIFY(!forkRequest.isEmpty());QCOMPARE(encoded(forkRequest).count("AFTER_RESTART_BODY"),1);
        type(side->composer(),"/side");QTest::qWait(180);QCOMPARE(choice(side->composer(),"command:side"),-1);
    }
    void compressionPreservesMemoryThenSkillThenUserOrder() {
        QTemporaryDir data,workspace;Model model;QVERIFY(put(workspace.filePath(".stagewise/skills/old/SKILL.md"),"---\nname: old\ndescription: Old\n---\nOLD_SKILL_MUST_STAY_BEFORE_BOUNDARY"));QVERIFY(put(workspace.filePath(".stagewise/skills/current/SKILL.md"),"---\nname: current\ndescription: Current\n---\nCURRENT_SKILL_BODY"));
        AgentEngine engine;AgentToolHost host(&engine,nullptr,"owner",data.path());QVERIFY(host.setWorkspace(workspace.path()));ContextOptions options;options.workspace=workspace.path();host.configureContext(options);
        ProviderConfig provider;provider.id="fixture";provider.model="fixture-model";provider.baseUrl=QString("http://127.0.0.1:%1/v1").arg(model.serverPort());provider.apiKey="fixture-only";engine.setProvider(provider);
        engine.setMessages({QJsonObject{{"role","user"},{"content","[old](slash:skill:old)"}},QJsonObject{{"role","assistant"},{"content","old answer"}},QJsonObject{{"role","user"},{"content","[current](slash:skill:current) boundary"},{"ade_compressedHistory","SUMMARY_BODY"}},QJsonObject{{"role","assistant"},{"content","boundary answer"}}});
        engine.submit("Continue");QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(),5000);const auto raw=encoded(model.requests.first());
        QVERIFY(!raw.contains("OLD_SKILL_MUST_STAY_BEFORE_BOUNDARY"));QVERIFY(raw.contains("CURRENT_SKILL_BODY"));QVERIFY(raw.indexOf("SUMMARY_BODY")<raw.indexOf("CURRENT_SKILL_BODY"));QVERIFY(raw.indexOf("CURRENT_SKILL_BODY")<raw.indexOf("/current boundary"));QVERIFY(!raw.contains("<user-msg><![CDATA[<memory>"));
        QCOMPARE(engine.messages().first().toObject()["content"].toString(),QString("[old](slash:skill:old)"));
    }
    void sideCommandRejectedAfterChatSwitchAndSaveConflict() {
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));SessionStore seedStore(data.path());auto sessions=seedStore.load();auto other=sessions.first();other.id="other";other.title="Other";sessions.append(other);QVERIFY(seedStore.save(sessions));
        MainWindow window(data.path());expose(window);auto* composer=window.findChild<Composer*>();QSignalSpy activated(composer,&Composer::completionActivated);type(composer,"preserve /si");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);
        // First establish a valid offered descriptor, but an external CAS edit
        // must reject the action before removing the typed query or forking.
        SessionStore external(data.path());auto changed=external.load();changed.last().title="External edit";QVERIFY(external.save(changed));activate(composer,"command:side");QCOMPARE(composer->toPlainText(),QString("preserve /si"));QVERIFY(window.findChildren<SideChatPane*>().isEmpty());QCOMPARE(SessionStore(data.path()).load().size(),2);QVERIFY(!activated.isEmpty());const auto stale=activated.last();
        auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()=="other")list->setCurrentRow(i);
        emit composer->completionActivated(stale[0].toString(),stale[1].toULongLong(),stale[2].toJsonObject());QVERIFY(window.findChildren<SideChatPane*>().isEmpty());QVERIFY(model.requests.isEmpty());
    }
    void sideCommandObserverCanDeleteOwner() {
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));QPointer<MainWindow> window=new MainWindow(data.path());expose(*window);auto* composer=window->findChild<Composer*>();type(composer,"keep /si");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);
        connect(composer,&Composer::textChanged,composer,[&]{if(window&&composer->toPlainText()=="keep ")delete window.data();});activate(composer,"command:side");QVERIFY(window.isNull());QCOMPARE(SessionStore(data.path()).load().size(),1);QVERIFY(model.requests.isEmpty());
    }
    void actionHideReplacementDoesNotEditNewContext(){
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));MainWindow window(data.path());expose(window);auto* composer=window.findChild<Composer*>();QSignalSpy requested(composer,&Composer::completionRequested);
        type(composer,"keep /si");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);QVERIFY(!requested.isEmpty());const auto request=requested.last()[1].toULongLong();auto* popup=composer->findChild<QFrame*>("composerCompletionPopup");QVERIFY(popup&&popup->isVisible());HideObserver observer;observer.action=[&]{composer->setCompletionContext("replacement");composer->setPlainText("unrelated draft");return false;};popup->installEventFilter(&observer);
        QVERIFY(!composer->commitCompletionAction(request,"command:side"));QCOMPARE(composer->toPlainText(),QString("unrelated draft"));QVERIFY(window.findChildren<SideChatPane*>().isEmpty());
    }
    void actionHideObserverCanCloseOwner(){
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));QPointer<MainWindow> window=new MainWindow(data.path());expose(*window);auto* composer=window->findChild<Composer*>();QSignalSpy requested(composer,&Composer::completionRequested);type(composer,"keep /si");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);const auto request=requested.last()[1].toULongLong();auto* popup=composer->findChild<QFrame*>("composerCompletionPopup");HideObserver observer;observer.action=[&]{window->close();window->deleteLater();return false;};popup->installEventFilter(&observer);QVERIFY(!composer->commitCompletionAction(request,"command:side"));QTRY_VERIFY(window.isNull());QVERIFY(model.requests.isEmpty());
    }
    void conversionFailureAndCancelDoNotPublishRequests(){
        QTemporaryDir data,workspace;Model model;QVERIFY(put(workspace.filePath(".stagewise/skills/big/SKILL.md"),QByteArray("---\nname: Big\ndescription: Large fixture\n---\n")+QByteArray(600000,'z')));AgentEngine engine;AgentToolHost host(&engine,nullptr,"owner",data.path());QVERIFY(host.setWorkspace(workspace.path()));ContextOptions options;options.workspace=workspace.path();host.configureContext(options);ProviderConfig provider;provider.id="fixture";provider.model="fixture-model";provider.baseUrl=QString("http://127.0.0.1:%1/v1").arg(model.serverPort());provider.apiKey="fixture-only";engine.setProvider(provider);const QJsonArray original{QJsonObject{{"role","user"},{"content","[Big](slash:skill:Big)"}},QJsonObject{{"role","assistant"},{"content","previous"}}};engine.setMessages(original);QSignalSpy failed(&engine,&AgentEngine::failed);engine.submit("[Big](slash:skill:Big)");QTRY_COMPARE(failed.size(),1);QVERIFY(!engine.isBusy());QVERIFY(model.requests.isEmpty());QCOMPARE(engine.messages().first(),original.first());
        options.disabledSkills={"Big"};host.configureContext(options);engine.submit("Continue without disabled skill");QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(),1,5000);QTRY_VERIFY(!engine.isBusy());
        ContextLoader loader;loader.load(options);engine.setMessagePreparer([&](const QJsonArray& history,const ProviderConfig&,QString* error){const auto converted=loader.prepareMessages(history,error);engine.cancel();return converted;});engine.submit("Canceled before dispatch");QVERIFY(!engine.isBusy());QTest::qWait(30);QCOMPARE(model.requests.size(),1);
        auto* dying=new AgentEngine;dying->setProvider(provider);QPointer<AgentEngine> guard(dying);dying->setMessagePreparer([&](const QJsonArray& history,const ProviderConfig&,QString* error){const auto converted=loader.prepareMessages(history,error);delete dying;return converted;});dying->submit("Destroyed before dispatch");QVERIFY(guard.isNull());QTest::qWait(30);QCOMPARE(model.requests.size(),1);
    }
    void staleActionCannotForkAnotherChat(){
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));SessionStore store(data.path());auto rows=store.load();auto other=rows.first();other.id="other";other.title="Other";rows.append(other);QVERIFY(store.save(rows));MainWindow window(data.path());expose(window);auto* composer=window.findChild<Composer*>();QSignalSpy requested(composer,&Composer::completionRequested);type(composer,"/side");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);const auto request=requested.last();
        auto* list=window.findChild<QListWidget*>("agentList");for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()=="other")list->setCurrentRow(i);QCOMPARE(window.currentSessionId(),QString("other"));
        const QJsonObject descriptor{{"id","command:side"},{"kind","side-chat"},{"label","Side"},{"description","Fork this chat into an independent side chat"},{"group","builtin"}};
        emit composer->completionActivated(request[0].toString(),request[1].toULongLong(),descriptor);QVERIFY(window.findChildren<SideChatPane*>().isEmpty());QCOMPARE(SessionStore(data.path()).load().size(),2);QVERIFY(model.requests.isEmpty());
    }
    void failedForkRestoresOnlyTheOriginalDraft(){
#ifdef Q_OS_WIN
        QTemporaryDir data,workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));QString error;AttachmentStore store(data.path());const auto ref=store.write("owner","held.txt","owned fixture bytes",&error);QVERIFY2(!ref.isEmpty(),qPrintable(error));const auto path=store.resolve("owner",ref,{},&error);QVERIFY(!path.isEmpty());MainWindow window(data.path());expose(window);auto* composer=window.findChild<Composer*>();type(composer,"preserved /side");QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,5000);
        const auto native=QDir::toNativeSeparators(path).toStdWString();HANDLE file=CreateFileW(native.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);QVERIFY(file!=INVALID_HANDLE_VALUE);struct Close{HANDLE file;~Close(){CloseHandle(file);}}close{file};
        activate(composer,"command:side");QCOMPARE(composer->toPlainText(),QString("preserved /side"));QVERIFY(window.findChildren<SideChatPane*>().isEmpty());const auto saved=SessionStore(data.path()).load();QCOMPARE(saved.size(),1);QCOMPARE(saved.first().draft,QString("preserved /side"));QVERIFY(model.requests.isEmpty());
#else
        QSKIP("Windows exclusive-handle fixture");
#endif
    }
    void sideSlashIsAnOwnedAction() {
        QTemporaryDir data, workspace;Model model;QVERIFY(seed(data.path(),workspace.path(),model.serverPort()));
        MainWindow window(data.path());expose(window);auto *composer=window.findChild<Composer*>();type(composer,"Keep this draft /si");
        QTRY_VERIFY_WITH_TIMEOUT(choice(composer,"command:side")>=0,2000);activate(composer,"command:side");
        QTRY_COMPARE_WITH_TIMEOUT(window.findChildren<SideChatPane*>().size(),1,5000);
        QCOMPARE(composer->toPlainText(),QString("Keep this draft "));
        const auto chats=SessionStore(data.path()).load();QCOMPARE(chats.size(),2);
        for(const auto &chat:chats)if(chat.id=="owner")QCOMPARE(chat.draft,QString("Keep this draft "));
        QVERIFY(model.requests.isEmpty());
    }
};
QTEST_MAIN(SkillsHostTest)
#include "test_skills_host.moc"
