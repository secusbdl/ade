#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "runtime/AgentToolHost.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/UtilityModelSelection.h"
#include <QDir>
#include <QComboBox>
#include <QInputDialog>
#include <QPushButton>
#include <QJsonDocument>
#include <QLabel>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
class UtilityServer : public QTcpServer {
public:
    QJsonArray requests;
    int responseStatus=200;bool anthropic=false;QString tag;QStringList* order=nullptr;
    QString answer = "## Earlier work\nThe fixture completed eight turns and preserved the full conversation for verification.";
    UtilityServer() {
        connect(this,&QTcpServer::newConnection,this,[this]{
            while(hasPendingConnections()) {
                auto* socket=nextPendingConnection();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                    auto bytes=socket->property("requestBytes").toByteArray()+socket->readAll();
                    socket->setProperty("requestBytes",bytes);
                    const auto boundary=bytes.indexOf("\r\n\r\n"); if(boundary<0||socket->property("answered").toBool())return;
                    qint64 length=0;
                    for(auto line:bytes.left(boundary).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toLongLong();
                    if(bytes.size()<boundary+4+length)return;
                    socket->setProperty("answered",true);requests.append(QJsonDocument::fromJson(bytes.mid(boundary+4,length)).object());if(order)order->append(tag);
                    const QJsonObject response=anthropic?QJsonObject{{"content",QJsonArray{QJsonObject{{"type","text"},{"text",answer}}}},{"stop_reason","end_turn"}}:QJsonObject{{"choices",QJsonArray{QJsonObject{{"finish_reason","stop"},{"message",QJsonObject{{"content",answer}}}}}}};
                    const auto body=QJsonDocument(response).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 "+QByteArray::number(responseStatus)+" Fixture\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
                });
            }
        });
        if(!listen(QHostAddress::LocalHost))qFatal("Cannot start loopback utility fixture");
    }
    ProviderConfig provider() const {ProviderConfig p;p.id="utility-http";p.name="Loopback utilities";p.model="utility-model";p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(serverPort());p.apiKey="synthetic-local-only";return p;}
};
ProviderConfig external(const QString& kind) {
    ProviderConfig p;p.id=kind;p.name=kind+" fixture";p.kind=kind;p.model="model-a";
    p.protocol=kind=="codex"?"codex-app-server":kind=="claude-code"?"claude-control":"opencode-acp";
    p.harnessExecutable=QDir(QCoreApplication::applicationDirPath()).filePath("harness_fixture.exe");return p;
}
QJsonObject entry(const ProviderConfig& p){return {{"providerInstanceId",p.id},{"modelId",p.model}};}
QJsonArray history(){QJsonArray rows;for(int i=0;i<8;++i){rows.append(QJsonObject{{"role","user"},{"content",QString("Saved request %1").arg(i)}});rows.append(QJsonObject{{"role","assistant"},{"content",QString("Saved answer %1").arg(i)}});}return rows;}
bool seed(const QString& profile,const QString& workspace,const ProviderConfig& cli,const ProviderConfig& http,bool title) {
    if(!ProviderStore::save(profile+"/providers.json",{cli,http}))return false;
    SettingsPage settings(profile);
    QJsonObject preset=entry(cli);preset["id"]="external-preset";preset["name"]="External preset";preset["models"]=QJsonArray{entry(cli)};
    if(!settings.setPreference("modelPresets",QJsonArray{preset})||!settings.setPreference("utilityModels",QJsonObject{{"titleGeneration",QJsonArray{entry(http)}},{"contextCompression",QJsonArray{entry(http)}}}))return false;
    Conversation chat;chat.id="utility-chat";chat.title="Original title";chat.titleLocked=!title;chat.providerId=cli.id;chat.model=cli.model;chat.presetId="external-preset";chat.workspace=workspace;if(!title)chat.messages=history();
    return SessionStore(profile).save({chat});
}
Conversation saved(const QString& profile){const auto chats=SessionStore(profile).load();return chats.isEmpty()?Conversation{}:chats.first();}
QString briefing(const Conversation& chat){for(const auto& value:chat.messages){const auto text=value.toObject()["ade_compressedHistory"].toString();if(!text.isEmpty())return text;}return {};}
QString status(MainWindow& window){auto* label=window.findChild<QLabel*>("statusLabel");if(label)return label->text();QStringList values;for(auto* label:window.findChildren<QLabel*>())values.append(label->text());return values.join("\n");}
void send(MainWindow& window,const QString& prompt){auto* composer=window.findChild<Composer*>("composerInput");auto* button=window.findChild<QToolButton*>("sendButton");QVERIFY(composer);QVERIFY(button);composer->setPlainText(prompt);QVERIFY(button->isEnabled());button->click();}
}
class UtilityHostTest:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){initializeAppStyle(*qApp);QVERIFY(QFileInfo::exists(external("codex").harnessExecutable));}
    void cleanup(){qunsetenv("ADE_HARNESS_FIXTURE_SCENARIO");}
    void externalPresetCompactUsesGlobal_data(){QTest::addColumn<QString>("kind");for(const auto& kind:{"codex","opencode","claude-code"})QTest::newRow(kind)<<QString(kind);}
    void externalPresetCompactUsesGlobal(){
        QFETCH(QString,kind);QTemporaryDir profile,workspace;UtilityServer server;QVERIFY(seed(profile.path(),workspace.path(),external(kind),server.provider(),false));
        {
            MainWindow window(profile.path());send(window,"/compact");
            QTRY_VERIFY_WITH_TIMEOUT(!server.requests.isEmpty()||status(window).contains("compression failed"),3000);
            QCOMPARE(server.requests.size(),1);QCOMPARE(server.requests.first().toObject()["model"].toString(),QString("utility-model"));
            QTRY_VERIFY_WITH_TIMEOUT(status(window).contains("Context compressed."),3000);
            const auto chat=saved(profile.path());QCOMPARE(chat.messages.size(),history().size());QCOMPARE(chat.messages.first(),history().first());
            QCOMPARE(briefing(chat),server.answer);
        }
        MainWindow reopened(profile.path());QCOMPARE(saved(profile.path()).messages.size(),history().size());QCOMPARE(briefing(saved(profile.path())),server.answer);
    }
    void externalPresetTitleUsesGlobal(){
        QTemporaryDir profile,workspace;UtilityServer server;server.answer="Verified utility title";QVERIFY(seed(profile.path(),workspace.path(),external("codex"),server.provider(),true));
        MainWindow window(profile.path());send(window,"Finish the isolated fixture task");
        QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument(saved(profile.path()).messages).toJson().contains("HARNESS_OK"),10000);
        QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(),1,3000);
        QTRY_COMPARE_WITH_TIMEOUT(saved(profile.path()).title,server.answer,3000);
        QCOMPARE(server.requests.first().toObject()["model"].toString(),QString("utility-model"));
        QVERIFY(QJsonDocument(saved(profile.path()).messages).toJson().contains("HARNESS_OK"));
    }
    void globalPickerHonorsExactSecondInstance(){
        QTemporaryDir profile;UtilityServer server;auto first=server.provider(),second=first;first.id="first";second.id="second";QVERIFY(ProviderStore::save(profile.path()+"/providers.json",{first,second}));SettingsPage page(profile.path());
        bool opened=false;QTimer::singleShot(0,&page,[&]{auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto* combo=dialog->findChild<QComboBox*>();if(combo){opened=true;combo->setCurrentIndex(1);}dialog->accept();});
        auto* add=page.findChild<QPushButton*>("utilityModels.titleGenerationAdd");QVERIFY(add);add->click();QVERIFY(opened);const auto selected=page.preference("utilityModels.titleGeneration").toArray();QCOMPARE(selected.size(),1);QCOMPARE(selected.first().toObject()["providerInstanceId"].toString(),QString("second"));
    }

    void explicitPresetOverridesKeepOrderIdentityThinkingAndContext(){
        QTemporaryDir profile,workspace;UtilityServer first,second,global;QStringList order;first.order=second.order=&order;first.tag="first";second.tag="second";first.responseStatus=503;second.anthropic=true;
        auto a=first.provider(),b=second.provider(),g=global.provider(),agent=external("codex");a.id="first";b.id="second";g.id="global";a.name=b.name="Identical provider";a.contextWindowTokens=8000;b.contextWindowTokens=64000;b.protocol="anthropic";
        QVERIFY(ProviderStore::save(profile.path()+"/providers.json",{agent,a,b,g}));SettingsPage settings(profile.path());auto override=entry(b);override["overrides"]=QJsonObject{{"thinkingMode","enabled"},{"thinkingBudget",2048}};
        auto record=entry(agent);record["id"]="external-preset";record["name"]="External preset";record["models"]=QJsonArray{entry(agent)};record["contextCompression"]=QJsonArray{QJsonObject{{"providerInstanceId","removed-instance"},{"modelId",b.model}},entry(a),override};
        QVERIFY(settings.setPreference("modelPresets",QJsonArray{record}));QVERIFY(settings.setPreference("utilityModels",QJsonObject{{"titleGeneration",QJsonArray{entry(g)}},{"contextCompression",QJsonArray{entry(g)}}}));
        const auto plan=resolveUtilityModelPlan({agent.id,agent.model,"external-preset",{},{}},{agent,a,b,g},{record},settings.preference("utilityModels").toObject());QCOMPARE(plan.compressionModels.size(),2);QCOMPARE(plan.compressionModels[1].provider.id,b.id);QCOMPARE(plan.compressionModels[1].provider.contextWindowTokens,64000);QVERIFY(plan.compressionModels[1].hasThinkingOverride);
        Conversation chat;chat.id="utility-chat";chat.title="Fixture";chat.titleLocked=true;chat.workspace=workspace.path();chat.providerId=agent.id;chat.model=agent.model;chat.presetId="external-preset";chat.messages=history();QVERIFY(SessionStore(profile.path()).save({chat}));
        MainWindow window(profile.path());send(window,"/compact");QTRY_VERIFY_WITH_TIMEOUT(status(window).contains("Context compressed."),5000);QCOMPARE(order,(QStringList{"first","second"}));QCOMPARE(global.requests.size(),0);QCOMPARE(second.requests.size(),1);
        const auto body=second.requests.first().toObject();QCOMPARE(body["model"].toString(),b.model);QCOMPARE(body["thinking"].toObject()["type"].toString(),QString("enabled"));QCOMPARE(body["thinking"].toObject()["budget_tokens"].toInt(),2048);QCOMPARE(briefing(saved(profile.path())),second.answer);
        SettingsPage reopened(profile.path());QCOMPARE(reopened.preference("modelPresets").toArray(),QJsonArray{record});
    }
    void hostPreservesUnavailableSavedUtilityRows(){
        QTemporaryDir profile,workspace;UtilityServer server;const auto agent=external("codex");QVERIFY(seed(profile.path(),workspace.path(),agent,server.provider(),false));
        const QJsonArray entries{entry(agent),QJsonObject{{"providerInstanceId","removed-instance"},{"modelId","saved-model"}}};{SettingsPage settings(profile.path());QVERIFY(settings.setPreference("utilityModels.titleGeneration",entries));}
        MainWindow window(profile.path());auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings);settings->selectSection("models-providers");QStringList text;for(const auto* label:settings->findChildren<QLabel*>())text.append(label->text());QVERIFY(text.join("\n").contains("not supported for utility requests"));QVERIFY(text.join("\n").contains("unavailable"));QCOMPARE(settings->preference("utilityModels.titleGeneration").toArray(),entries);QCOMPARE(server.requests.size(),0);
    }

};
QTEST_MAIN(UtilityHostTest)
#include "test_utility_models_host.moc"
