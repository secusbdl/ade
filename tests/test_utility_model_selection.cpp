#include "core/ModelDiscoveryPolicy.h"
#include "core/ProviderStore.h"
#include "core/UtilityService.h"
#include "ui/AppStyle.h"
#include "ui/SettingsPage.h"
#include "ui/UtilityModelSelection.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig api(const QString& id){ProviderConfig p;p.id=id;p.name=id;p.model="shared-model";p.baseUrl="https://fixture.invalid/v1";return p;}
ProviderConfig cli(const QString& id,const QString& kind){auto p=api(id);p.kind=kind;p.baseUrl.clear();p.protocol=kind=="codex"?"codex-app-server":kind=="claude-code"?"claude-control":"opencode-acp";p.harnessExecutable="fixture.exe";return p;}
QJsonObject e(const QString& id,const QString& model="shared-model"){return {{"providerInstanceId",id},{"modelId",model}};}
QList<ProviderConfig> providers(){return {api("a"),api("b"),api("global"),cli("c","codex"),cli("o","opencode"),cli("l","claude-code")};}
QStringList keys(const QList<ModelConfig>& models){QStringList out;for(const auto& m:models)out.append(m.provider.id+"/"+m.provider.model);return out;}
QJsonObject globals(){return {{"titleGeneration",QJsonArray{e("global")}},{"contextCompression",QJsonArray{e("global")}}};}
QJsonObject preset(QJsonArray models){auto row=models.isEmpty()?QJsonObject{}:models.first().toObject();row["id"]="preset";row["name"]="Fixture preset";row["models"]=models;return row;}
QString labels(QWidget& parent){QStringList text;for(const auto* label:parent.findChildren<QLabel*>())text.append(label->text());return text.join("\n");}
class WireServer:public QTcpServer {
public:
 QJsonArray requests;int status=200;QString answer="A sufficiently long summary for the isolated utility request fixture.";
 WireServer(){connect(this,&QTcpServer::newConnection,this,[this]{while(hasPendingConnections()){auto* socket=nextPendingConnection();connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QTcpSocket::readyRead,this,[this,socket]{auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);auto end=bytes.indexOf("\r\n\r\n");if(end<0||socket->property("done").toBool())return;int length=0;for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();if(bytes.size()<end+4+length)return;socket->setProperty("done",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());auto body=QJsonDocument(QJsonObject{{"choices",QJsonArray{QJsonObject{{"finish_reason","stop"},{"message",QJsonObject{{"content",answer}}}}}}}).toJson(QJsonDocument::Compact);socket->write("HTTP/1.1 "+QByteArray::number(status)+" Fixture\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();});}});if(!listen(QHostAddress::LocalHost))qFatal("loopback unavailable");}
 ProviderConfig config(const QString& id){auto p=api(id);p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(serverPort());return p;}
};
}
class UtilityModelsTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void sourceOrder_data(){
  QTest::addColumn<QString>("activeId");QTest::addColumn<QString>("presetId");QTest::addColumn<QJsonObject>("record");QTest::addColumn<QJsonObject>("global");QTest::addColumn<QStringList>("title");QTest::addColumn<QStringList>("compression");
  auto row=[](const char* name,QString active,QString id,QJsonObject p,QJsonObject g,QStringList title,QStringList compact){QTest::newRow(name)<<active<<id<<p<<g<<title<<compact;};
  row("no-preset-global-title-stops", "a",{}, {},globals(),{"global/shared-model"},{"global/shared-model","a/shared-model"});
  row("deleted-preset-global", "a","deleted",preset({e("b")}),globals(),{"global/shared-model"},{"global/shared-model","a/shared-model"});
  row("empty-global-active", "a",{}, {},{}, {"a/shared-model"},{"a/shared-model"});
  row("native-preset-empty-override", "a","preset",preset({e("a"),e("b")}),globals(),{"a/shared-model","b/shared-model"},{"a/shared-model","b/shared-model"});
  auto empty=preset({e("a"),e("b")});empty["titleGeneration"]=QJsonArray{};empty["contextCompression"]=QJsonArray{};
  row("explicit-empty-uses-chat-chain", "a","preset",empty,globals(),{"a/shared-model","b/shared-model"},{"a/shared-model","b/shared-model"});
  auto legacy=e("a");legacy["id"]="preset";legacy["name"]="Legacy primary";
  row("legacy-native-single-primary", "a","preset",legacy,globals(),{"a/shared-model"},{"a/shared-model"});
  row("active-fallback-precedes-preset", "b","preset",preset({e("a"),e("b")}),globals(),{"b/shared-model","a/shared-model"},{"b/shared-model","a/shared-model"});
  row("mixed-preset-skips-external", "c","preset",preset({e("c"),e("a"),e("b")}),globals(),{"a/shared-model","b/shared-model"},{"a/shared-model","b/shared-model"});
  row("external-only-global", "c","preset",preset({e("c"),e("o"),e("l")}),globals(),{"global/shared-model"},{"global/shared-model"});
  row("external-only-no-global", "c","preset",preset({e("c")}),{}, {},{});
  auto explicitPreset=preset({e("a"),e("b")});explicitPreset["titleGeneration"]=QJsonArray{e("global"),e("b")};explicitPreset["contextCompression"]=QJsonArray{e("b")};
  row("independent-overrides", "a","preset",explicitPreset,globals(),{"global/shared-model","b/shared-model","a/shared-model"},{"b/shared-model","a/shared-model"});
  auto externalOverride=preset({e("a"),e("b")});externalOverride["titleGeneration"]=QJsonArray{e("c"),e("o"),e("l")};externalOverride["contextCompression"]=externalOverride.value("titleGeneration");
  row("excluded-override-native-main", "a","preset",externalOverride,globals(),{"a/shared-model","b/shared-model"},{"a/shared-model","b/shared-model"});
  externalOverride["models"]=QJsonArray{e("c")};row("excluded-override-external-main", "c","preset",externalOverride,globals(),{"global/shared-model"},{"global/shared-model"});
  auto missing=preset({e("c")});missing["titleGeneration"]=QJsonArray{e("deleted-instance")};missing["contextCompression"]=missing.value("titleGeneration");
  row("missing-override-does-not-route-global", "c","preset",missing,globals(),{},{});
  row("missing-primary-is-eligible-before-resolution", "c","preset",preset({e("deleted-instance"),e("c")}),globals(),{},{});
  auto g=globals();g["titleGeneration"]=QJsonArray{e("c"),e("a"),e("o"),e("b"),e("l")};g["contextCompression"]=g.value("titleGeneration");
  row("all-three-harness-kinds-filtered", "a",{}, {},g,{"a/shared-model","b/shared-model"},{"a/shared-model","b/shared-model"});
 }
 void sourceOrder(){QFETCH(QString,activeId);QFETCH(QString,presetId);QFETCH(QJsonObject,record);QFETCH(QJsonObject,global);QFETCH(QStringList,title);QFETCH(QStringList,compression);const auto plan=resolveUtilityModelPlan({activeId,"shared-model",presetId,{},{}},providers(),record.isEmpty()?QJsonArray{}:QJsonArray{record},global);QCOMPARE(keys(plan.titleModels),title);QCOMPARE(keys(plan.compressionModels),compression);}
 void overridesContextAndExactInstancesSurvive(){
  auto a=api("a"),b=api("b");a.contextWindowTokens=4000;b.contextWindowTokens=8000;a.disabledModelIds={"other"};
  a.discoverySnapshot=makeModelDiscoverySnapshot(a,QJsonArray{QJsonObject{{"id","other"},{"name","Other model"},{"inputTokenLimit",64000},{"outputTokenLimit",1000}}},true);
  auto first=e("a","other");first["overrides"]=QJsonObject{{"reasoningEffort","high"},{"thinkingMode","enabled"},{"thinkingBudget",2048},{"maxOutputTokens",2049}};
  auto duplicate=first;duplicate["presetId"]="ignored";duplicate["overrides"]=QJsonObject{{"thinkingMode","disabled"}};
  const auto plan=resolveUtilityModelPlan({"b","shared-model",{},{},{}},{a,b},{},{{"titleGeneration",QJsonArray{first,duplicate,e("b"),e("a")}}});
  QCOMPARE(keys(plan.titleModels),(QStringList{"a/other","b/shared-model","a/shared-model"}));
  const auto& found=plan.titleModels.first();QVERIFY(found.hasThinkingOverride);QCOMPARE(found.provider.thinkingMode,QString("enabled"));QCOMPARE(found.provider.reasoningEffort,QString("high"));QCOMPARE(found.provider.thinkingBudget,2048);QCOMPARE(found.provider.maxOutputTokens,2049);QCOMPARE(found.provider.contextWindowTokens,64000);
  QCOMPARE(plan.titleModels[1].provider.contextWindowTokens,8000);QCOMPARE(plan.titleModels[2].provider.contextWindowTokens,4000);
 }
 void choicesAndPersistedLabels(){auto list=providers();list[0].disabledModelIds={"shared-model"};const auto choices=availableUtilityModelSelections(list,{});QCOMPARE(choices.size(),2);QCOMPARE(choices[0].providerId,QString("b"));QCOMPARE(choices[1].providerId,QString("global"));QVERIFY(utilityModelEntryLabel(e("a"),list).contains("disabled in model picker"));for(const auto& id:{"c","o","l"})QVERIFY(utilityModelEntryLabel(e(id),list).contains("not supported for utility requests"));QVERIFY(utilityModelEntryLabel(e("gone"),list).contains("unavailable"));QCOMPARE(availableModelSelections(list,{},{}).size(),5);}
 void directServiceSkipsExternalAndKeepsExactFailover(){
  WireServer first,second,external;first.status=503;auto a=first.config("a"),b=second.config("b"),bad=external.config("external");bad.kind="codex";
  UtilityService service;service.setTitleModels({{bad},{a},{a},{b}});service.setMainModels({{a},{bad}});QSignalSpy done(&service,&UtilityService::titleReady),failed(&service,&UtilityService::failed),attempts(&service,&UtilityService::modelAttempted);
  service.generateTitle({QJsonObject{{"role","user"},{"content","Fixture task"}}});QTRY_COMPARE(done.size(),1);QCOMPARE(failed.size(),0);QCOMPARE(attempts.size(),2);QCOMPARE(attempts[0][0].toString(),QString("a"));QCOMPARE(attempts[1][0].toString(),QString("b"));QCOMPARE(external.requests.size(),0);QCOMPARE(first.requests.size(),1);QCOMPARE(second.requests.size(),1);QVERIFY(!first.requests[0].toObject()["stream"].toBool(true));
 }
 void onlyExternalServiceFailsWithoutRequest(){WireServer server;auto p=server.config("external");p.kind="opencode";UtilityService service;service.setMainModels({{p}});QSignalSpy failed(&service,&UtilityService::failed),attempts(&service,&UtilityService::modelAttempted);service.compressHistory({QJsonObject{{"role","user"},{"content","Fixture"}}});QCOMPARE(failed.size(),1);QCOMPARE(attempts.size(),0);QCOMPARE(server.requests.size(),0);QVERIFY(!service.isBusy());}
 void globalPickerKeepsSecondIdenticalLabelInstance(){
  QTemporaryDir dir;auto a=api("a"),b=api("b");a.name=b.name="Identical";QVERIFY(ProviderStore::save(dir.path()+"/providers.json",{a,b,cli("c","codex"),cli("o","opencode"),cli("l","claude-code")}));SettingsPage page(dir.path());
  bool seen=false;int count=-1;QTimer::singleShot(0,&page,[&]{auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto* combo=dialog->findChild<QComboBox*>();if(combo){seen=true;count=combo->count();combo->setCurrentIndex(1);}dialog->accept();});
  page.findChild<QPushButton*>("utilityModels.titleGenerationAdd")->click();QVERIFY(seen);QCOMPARE(count,2);const auto entries=page.preference("utilityModels.titleGeneration").toArray();QCOMPARE(entries.size(),1);QCOMPARE(entries.first().toObject()["providerInstanceId"].toString(),QString("b"));SettingsPage reopened(dir.path());QCOMPARE(reopened.preference("utilityModels.titleGeneration"),page.preference("utilityModels.titleGeneration"));
 }
 void onlyExternalNewUtilityShowsExplanation(){QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",{cli("c","codex")}));SettingsPage page(dir.path());page.findChild<QPushButton*>("utilityModels.contextCompressionAdd")->click();QVERIFY(labels(page).contains("No eligible API models"));QVERIFY(!QApplication::activeModalWidget());QVERIFY(page.preference("utilityModels.contextCompression").toArray().isEmpty());}
 void existingUnsupportedRowsRemainVisibleAndUnchanged(){QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",providers()));SettingsPage page(dir.path());QJsonArray entries{e("c"),e("gone"),e("a")};QVERIFY(page.setPreference("utilityModels.titleGeneration",entries));page.selectSection("models-providers");QVERIFY(labels(page).contains("not supported for utility requests"));QVERIFY(labels(page).contains("unavailable"));QCOMPARE(page.preference("utilityModels.titleGeneration").toArray(),entries);SettingsPage reopened(dir.path());QCOMPARE(reopened.preference("utilityModels.titleGeneration").toArray(),entries);}
 void globalPickerSaveConflictPreservesPreferences(){QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",providers()));SettingsPage page(dir.path());QVERIFY(page.setPreference("agent.keepAwake",false));const auto before=page.preferences();bool changed=false;QTimer::singleShot(0,&page,[&]{auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(!dialog)return;SettingsPage other(dir.path());changed=other.setPreference("agent.keepAwake",true);dialog->accept();});page.findChild<QPushButton*>("utilityModels.titleGenerationAdd")->click();QVERIFY(changed);QCOMPARE(page.preferences(),before);SettingsPage reopened(dir.path());QVERIFY(reopened.preference("utilityModels.titleGeneration").toArray().isEmpty());QVERIFY(reopened.preference("agent.keepAwake").toBool());}
 void presetUtilityPickersExcludeExternalAndPreserveSavedRows(){
  QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",providers()));SettingsPage page(dir.path());auto record=preset({e("c"),e("a")});record["titleGeneration"]=QJsonArray{e("c"),e("gone")};record["contextCompression"]=QJsonArray{e("o")};QVERIFY(page.setPreference("modelPresets",QJsonArray{record}));page.selectSection("models-providers");
  QStringList titleRows;int primaryCount=0,pickerCount=0;QString addedId;bool dialogSeen=false;
  QTimer::singleShot(0,&page,[&]{
   auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;dialogSeen=true;
   if(auto* model=dialog->findChild<QComboBox*>("presetModel"))primaryCount=model->count();
   auto* list=dialog->findChild<QListWidget*>("presetTitleModels");
   if(list){for(int i=0;i<list->count();++i)titleRows.append(list->item(i)->text());auto* tabs=dialog->findChild<QTabWidget*>();if(tabs)tabs->setCurrentIndex(1);
    auto* pane=list->parentWidget();QPushButton* add=nullptr;for(auto* button:pane->findChildren<QPushButton*>())if(button->text()=="Add model")add=button;
    if(add){QTimer::singleShot(0,dialog,[&]{auto* choice=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());if(!choice)return;auto* combo=choice->findChild<QComboBox*>();if(combo){pickerCount=combo->count();combo->setCurrentIndex(1);}choice->accept();});add->click();if(list->count()==3)addedId=list->item(2)->data(Qt::UserRole).toJsonObject()["providerInstanceId"].toString();}
   }
   dialog->reject();
  });
  auto* edit=page.findChild<QPushButton*>("editModelPreset_preset");QVERIFY(edit);edit->click();QVERIFY(dialogSeen);QCOMPARE(primaryCount,6);QCOMPARE(pickerCount,3);QCOMPARE(addedId,QString("b"));QVERIFY(titleRows.value(0).contains("not supported"));QVERIFY(titleRows.value(1).contains("unavailable"));QCOMPARE(page.preference("modelPresets").toArray(),QJsonArray{record});
 }
 void globalPickerOwnerDeletionIsSafe(){
  QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",providers()));QPointer<SettingsPage> page=new SettingsPage(dir.path());
  QTimer::singleShot(0,page,[&]{delete page.data();});
  auto* button=page->findChild<QPushButton*>("utilityModels.titleGenerationAdd");QVERIFY(button);button->click();QVERIFY(!page);SettingsPage reopened(dir.path());QVERIFY(reopened.preference("utilityModels.titleGeneration").toArray().isEmpty());
 }
 void emptyUtilityPickerCanCancelWithoutWrite(){QTemporaryDir dir;QVERIFY(ProviderStore::save(dir.path()+"/providers.json",providers()));SettingsPage page(dir.path());const auto original=page.preferences();QTimer::singleShot(0,&page,[]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});page.findChild<QPushButton*>("utilityModels.contextCompressionAdd")->click();QCOMPARE(page.preferences(),original);QVERIFY(!QFile::exists(dir.path()+"/preferences.json"));}

};
QTEST_MAIN(UtilityModelsTest)
#include "test_utility_model_selection.moc"
