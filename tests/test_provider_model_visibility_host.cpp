#include "ui/MainWindow.h"
#include "ui/SettingsPage.h"
#include "ui/SideChatPane.h"
#include "ui/ProviderModelsDialog.h"
#include "ui/ModelSelection.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "core/ProviderStore.h"
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(const QString& id){ProviderConfig p;p.id=id;p.name="Instance "+id;p.baseUrl="https://fixture.invalid/v1";p.apiKey="synthetic-fixture-only";p.model="default";return p;}
bool seed(const QString& directory){
 const auto a=provider("deepseek"),b=provider("second");if(!ProviderStore::save(directory+"/providers.json",{a,b}))return false;
 Conversation parent;parent.id="parent";parent.title="Visibility fixture";parent.titleLocked=true;parent.providerId=a.id;parent.model="fast";
 auto child=parent;child.id="child";child.sideChatParentId=parent.id;child.title="Side visibility fixture";child.providerId=b.id;
 if(!SessionStore(directory).save({parent,child}))return false;
 SettingsPage settings(directory);const QJsonArray models{QJsonObject{{"id","default"}},QJsonObject{{"id","fast"}}};return settings.setPreference("modelCatalogs",QJsonObject{{a.id,cleanModelCatalog(a,models)},{b.id,cleanModelCatalog(b,models)}});
}
SettingsPage* showSettings(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,750);window.show();QCoreApplication::processEvents();auto* button=window.findChild<QToolButton*>("settingsButton");if(!button)return nullptr;button->click();auto* settings=window.findChild<SettingsPage*>();if(settings)settings->selectSection("models-providers");return settings;}
ProviderModelsDialog* openModels(SettingsPage* settings,const QString& id){auto* button=settings->findChild<QPushButton*>("manageProviderModels_"+id);if(!button)return nullptr;button->click();for(auto* dialog:settings->findChildren<ProviderModelsDialog*>())if(dialog->isVisible()&&dialog->providerId()==id)return dialog;return nullptr;}
bool hide(ProviderModelsDialog* dialog,const QString& id){auto* model=dialog->findChild<QListView*>("providerModelList")->model();for(int row=0;row<model->rowCount();++row){const auto index=model->index(row,0);if(index.data(Qt::UserRole+1).toString()==id)return model->setData(index,Qt::Unchecked,Qt::CheckStateRole);}return false;}
int indexOf(QComboBox* combo,const QString& providerId,const QString& modelId){for(int i=0;i<combo->count();++i){const auto entry=combo->itemData(i).toJsonObject();if(entry["providerInstanceId"]==providerId&&entry["modelId"]==modelId)return i;}return -1;}
bool enabled(QComboBox* combo,int index){return index>=0&&(combo->model()->flags(combo->model()->index(index,0))&Qt::ItemIsEnabled);}
QByteArray bytes(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
}
class ProviderModelVisibilityHostTest:public QObject{
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void settingsExactInstanceRefreshesMainAndSideWithoutRewritingSavedChoice(){
    QTemporaryDir directory;QVERIFY(seed(directory.path()));MainWindow window(directory.path());auto* settings=showSettings(window);QVERIFY(settings);auto* main=window.findChild<QComboBox*>("modelSelector");auto* side=window.findChild<SideChatPane*>();QVERIFY(main&&side);auto* sideModels=side->findChild<QComboBox*>("sideChatModel");QVERIFY(sideModels);
    auto* first=openModels(settings,"deepseek");QVERIFY(first);QVERIFY(hide(first,"fast"));QCOMPARE(main->currentData().toJsonObject()["providerInstanceId"].toString(),QString("deepseek"));QCOMPARE(main->currentData().toJsonObject()["modelId"].toString(),QString("fast"));QVERIFY(!enabled(main,main->currentIndex()));QVERIFY(enabled(main,indexOf(main,"second","fast")));QVERIFY(enabled(sideModels,sideModels->currentIndex()));first->reject();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    auto* second=openModels(settings,"second");QVERIFY(second);QVERIFY(hide(second,"fast"));QCOMPARE(sideModels->currentData().toJsonObject()["providerInstanceId"].toString(),QString("second"));QCOMPARE(sideModels->currentData().toJsonObject()["modelId"].toString(),QString("fast"));QVERIFY(!enabled(sideModels,sideModels->currentIndex()));QVERIFY(!sideModels->currentText().contains("unavailable"));
    const auto saved=SessionStore(directory.path()).load();QCOMPARE(saved.size(),2);for(const auto& chat:saved)QCOMPARE(chat.model,QString("fast"));second->reject();QVERIFY(window.close());
    MainWindow reopened(directory.path());reopened.setAttribute(Qt::WA_DontShowOnScreen);reopened.show();QCoreApplication::processEvents();auto* restored=reopened.findChild<QComboBox*>("modelSelector");QVERIFY(restored);QCOMPARE(restored->currentData().toJsonObject()["modelId"].toString(),QString("fast"));QVERIFY(!enabled(restored,restored->currentIndex()));auto* child=reopened.findChild<SideChatPane*>();QVERIFY(child);auto* restoredSide=child->findChild<QComboBox*>("sideChatModel");QCOMPARE(restoredSide->currentData().toJsonObject()["modelId"].toString(),QString("fast"));QVERIFY(!enabled(restoredSide,restoredSide->currentIndex()));
 }
 void conflictingProviderEditKeepsSelectionsAndStoredBytes(){QTemporaryDir directory;QVERIFY(seed(directory.path()));MainWindow window(directory.path());auto* settings=showSettings(window);QVERIFY(settings);auto* dialog=openModels(settings,"second");QVERIFY(dialog);auto outside=ProviderStore::load(directory.filePath("providers.json"));outside[1].name="External owner";QVERIFY(ProviderStore::save(directory.filePath("providers.json"),outside));const auto original=bytes(directory.filePath("providers.json"));QSignalSpy changes(settings,&SettingsPage::providerVisibilityChanged);QVERIFY(hide(dialog,"fast"));QCOMPARE(changes.size(),0);QCOMPARE(bytes(directory.filePath("providers.json")),original);QVERIFY(dialog->findChild<QLabel*>("providerModelsStatus")->text().contains("another window"));auto* main=window.findChild<QComboBox*>("modelSelector");QVERIFY(enabled(main,indexOf(main,"second","fast")));dialog->reject();}
 void reimportThroughActualWindowKeepsVisibility(){QTemporaryDir directory;QVERIFY(seed(directory.path()));auto providers=ProviderStore::load(directory.filePath("providers.json"));providers[0].disabledModelIds={"fast"};QVERIFY(ProviderStore::save(directory.filePath("providers.json"),providers));MainWindow window(directory.path());QVERIFY(showSettings(window));const auto file=directory.filePath("keys.env");{QFile keys(file);QVERIFY(keys.open(QIODevice::WriteOnly));keys.write("DEEPSEEK_API_KEY=synthetic-replacement-key\n");}window.importKeys(file);const auto saved=ProviderStore::load(directory.filePath("providers.json"));QCOMPARE(saved[0].disabledModelIds,QStringList{"fast"});QCOMPARE(saved[0].apiKey,QString("synthetic-replacement-key"));QCOMPARE(saved[1].apiKey,providers[1].apiKey);QCOMPARE(SessionStore(directory.path()).load()[0].model,QString("fast"));}
 void durableVisibilityNotificationCanDeleteOwner(){QTemporaryDir directory;QVERIFY(seed(directory.path()));auto* window=new MainWindow(directory.path());QPointer<MainWindow> guard(window);auto* settings=showSettings(*window);QVERIFY(settings);auto* dialog=openModels(settings,"second");QVERIFY(dialog);connect(settings,&SettingsPage::providerVisibilityChanged,settings,[window]{delete window;});hide(dialog,"fast");QVERIFY(!guard);const auto saved=ProviderStore::load(directory.filePath("providers.json"));QCOMPARE(saved[1].disabledModelIds,QStringList{"fast"});}
};
QTEST_MAIN(ProviderModelVisibilityHostTest)
#include "test_provider_model_visibility_host.moc"
