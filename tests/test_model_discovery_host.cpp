#include "ui/ProviderDiscovery.h"
#include "ui/MainWindow.h"
#include "ui/SettingsPage.h"
#include "ui/ProviderDialog.h"
#include "ui/ProviderModelsDialog.h"
#include "ui/ProviderCatalog.h"
#include "ui/ModelSelection.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "ui/AppFonts.h"
#include "ui/SideChatPane.h"
#include "core/ProviderService.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QNetworkProxy>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
QJsonArray models(std::initializer_list<QString> ids){QJsonArray out;for(const auto& id:ids)out.append(QJsonObject{{"id",id},{"name",id}});return out;}
ProviderConfig provider(QString id="first"){auto p=ProviderCatalog::instantiate("openai-api");p.id=id;p.model="gpt-5.5";return p;}
QByteArray bytes(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
bool seed(const QString& directory,bool legacy=false){auto a=provider(),b=provider("second");b.model="small";if(!ProviderStore::save(directory+"/providers.json",{a,b}))return false;Conversation main;main.id="parent";main.title="Discovery fixture";main.titleLocked=true;main.providerId=a.id;main.model="small";auto child=main;child.id="child";child.title="Independent side chat";child.sideChatParentId=main.id;child.providerId=b.id;if(!SessionStore(directory).save({main,child}))return false;SettingsPage settings(directory);return !legacy||settings.setPreference("modelCatalogs",QJsonObject{{a.id,cleanModelCatalog(a,models({"gpt-5.5","legacy-manual"}))}});}
SettingsPage* settings(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,800);window.show();QCoreApplication::processEvents();auto* button=window.findChild<QToolButton*>("settingsButton");if(!button)return nullptr;button->click();auto* page=window.findChild<SettingsPage*>();if(page)page->selectSection("models-providers");return page;}
ProviderModelsDialog* manage(SettingsPage* page,const QString& id){auto* button=page->findChild<QPushButton*>("manageProviderModels_"+id);if(!button)return nullptr;button->click();for(auto* dialog:page->findChildren<ProviderModelsDialog*>())if(dialog->isVisible()&&dialog->providerId()==id)return dialog;return nullptr;}
bool checked(ProviderModelsDialog* dialog,const QString& id,bool enabled){auto* model=dialog->findChild<QListView*>("providerModelList")->model();for(int row=0;row<model->rowCount();++row){const auto index=model->index(row,0);if(index.data(Qt::UserRole+1).toString()==id)return model->setData(index,enabled?Qt::Checked:Qt::Unchecked,Qt::CheckStateRole);}return false;}
int indexOf(QComboBox* combo,const QString& providerId,const QString& modelId){for(int i=0;i<combo->count();++i){const auto e=combo->itemData(i).toJsonObject();if(e["providerInstanceId"]==providerId&&e["modelId"]==modelId)return i;}return -1;}
bool enabled(QComboBox* combo,int row){return row>=0&&(combo->model()->flags(combo->model()->index(row,0))&Qt::ItemIsEnabled);}
// Captured transport replies only. The mandatory localhost HTTP proxy cannot
// forward anything; no official provider connection or credential is used.
struct OfflineProxy {QNetworkProxy previous=QNetworkProxy::applicationProxy();QTcpServer server;OfflineProxy(){server.listen(QHostAddress::LocalHost);QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy,"127.0.0.1",server.serverPort()));QObject::connect(&server,&QTcpServer::newConnection,&server,[this]{while(server.hasPendingConnections()){auto* socket=server.nextPendingConnection();QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);}});}~OfflineProxy(){QNetworkProxy::setApplicationProxy(previous);}};
QString edit(MainWindow& owner,SettingsPage* page,const QString& id,const std::function<void(ProviderDialog*)>& action){
 QString error;bool handled=false;QTimer timer;QElapsedTimer clock;clock.start();QPointer<MainWindow> window(&owner);
 QObject::connect(&timer,&QTimer::timeout,&timer,[&]{if(!window){timer.stop();return;}for(auto* raw:window->findChildren<ProviderDialog*>()){QPointer<ProviderDialog> dialog(raw);if(!dialog->isVisible())continue;timer.stop();handled=true;action(dialog);if(dialog&&dialog->isVisible()){error="Editor did not finish its action.";dialog->reject();}return;}if(clock.elapsed()>4000){error="Timed out waiting for provider editor.";timer.stop();}});
 timer.start(5);emit page->providerEditRequested(id);timer.stop();if(!handled&&error.isEmpty())error="Provider editor did not open.";return error;
}
bool discover(ProviderDialog* dialog,const QJsonArray& reply){
 auto* fetch=dialog->findChild<QPushButton*>("fetchProviderModels");if(!fetch)return false;fetch->click();QPointer<ProviderService> service=dialog->findChild<ProviderService*>();if(!service)return false;
 QTimer picker;QObject::connect(&picker,&QTimer::timeout,&picker,[dialog]{for(auto* input:dialog->findChildren<QInputDialog*>())if(input->isVisible())input->reject();});picker.start(5);emit service->modelsReady(reply);picker.stop();return true;
}
void save(ProviderDialog* dialog){dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();}
}
class ModelDiscoveryHostTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){AppFonts::load();initializeAppStyle(*qApp);}
 void manualEnableSurvivesRefreshWhenLegacyCacheCannotSaveAndRestart(){
  OfflineProxy offline;QVERIFY(offline.server.isListening());QTemporaryDir directory;QVERIFY(seed(directory.path()));
  {MainWindow window(directory.path());auto* page=settings(window);QVERIFY(page);bool got=false;
   QCOMPARE(edit(window,page,"first",[&](ProviderDialog* dialog){got=discover(dialog,models({"gpt-5.5","small"}));save(dialog);}),QString());QVERIFY(got);
   auto initial=ProviderStore::load(directory.filePath("providers.json"));QCOMPARE(initial[0].disabledModelIds,QStringList{"small"});QVERIFY(!initial[0].discoverySnapshot.isEmpty());
   auto* modelDialog=manage(page,"first");QVERIFY(modelDialog);QVERIFY(checked(modelDialog,"small",true));modelDialog->reject();QVERIFY(ProviderStore::load(directory.filePath("providers.json"))[0].disabledModelIds.isEmpty());
   const auto preferences=directory.filePath("preferences.json");{QFile f(preferences);QVERIFY(f.open(QIODevice::WriteOnly));f.write("{broken-external-settings");}const auto original=bytes(preferences);
   QVERIFY(!page->setPreference("modelCatalogs",QJsonObject{}));QCOMPARE(bytes(preferences),original);
   QCOMPARE(edit(window,page,"first",[&](ProviderDialog* dialog){got=discover(dialog,models({"gpt-5.5","small","new-small"}));save(dialog);}),QString());QVERIFY(got);QCOMPARE(bytes(preferences),original);
   const auto current=ProviderStore::load(directory.filePath("providers.json"));QCOMPARE(current[0].disabledModelIds,QStringList{"new-small"});QVERIFY(current[1].discoverySnapshot.isEmpty());
   auto* main=window.findChild<QComboBox*>("modelSelector");QVERIFY(main);QCOMPARE(main->currentData().toJsonObject().value("modelId").toString(),QString("small"));QVERIFY(enabled(main,main->currentIndex()));QVERIFY(!enabled(main,indexOf(main,"first","new-small")));QVERIFY(enabled(main,indexOf(main,"second","small")));
   QVERIFY(window.close());}
  MainWindow reopened(directory.path());auto* page=settings(reopened);QVERIFY(page);auto* main=reopened.findChild<QComboBox*>("modelSelector");QVERIFY(enabled(main,indexOf(main,"first","small")));QVERIFY(!enabled(main,indexOf(main,"first","new-small")));QCOMPARE(main->currentData().toJsonObject().value("modelId").toString(),QString("small"));
  auto* side=reopened.findChild<SideChatPane*>();QVERIFY(side);auto* sideModels=side->findChild<QComboBox*>("sideChatModel");QVERIFY(sideModels);QCOMPARE(sideModels->currentData().toJsonObject().value("providerInstanceId").toString(),QString("second"));QVERIFY(enabled(sideModels,sideModels->currentIndex()));
 }
 void completeAllHiddenInventoryCanSaveThenEnableAndSelect(){OfflineProxy offline;QTemporaryDir dir;auto p=provider();p.model.clear();QVERIFY(ProviderStore::save(dir.filePath("providers.json"),{p}));Conversation chat;chat.id="empty-choice";chat.title="All hidden fixture";chat.titleLocked=true;QVERIFY(SessionStore(dir.path()).save({chat}));MainWindow window(dir.path());auto* page=settings(window);QVERIFY(page);bool got=false;QCOMPARE(edit(window,page,"first",[&](ProviderDialog* dialog){got=discover(dialog,models({"unlisted-only"}));save(dialog);}),QString());QVERIFY(got);auto saved=ProviderStore::load(dir.filePath("providers.json"));QCOMPARE(saved.size(),1);QVERIFY(saved[0].model.isEmpty());QCOMPARE(saved[0].disabledModelIds,QStringList{"unlisted-only"});QVERIFY(saved[0].discoverySnapshot.value("complete").toBool());auto* dialog=manage(page,"first");QVERIFY(dialog);QVERIFY(checked(dialog,"unlisted-only",true));dialog->reject();emit page->backRequested();auto* combo=window.findChild<QComboBox*>("modelSelector");QVERIFY(combo);const int row=indexOf(combo,"first","unlisted-only");QVERIFY(enabled(combo,row));combo->setCurrentIndex(-1);combo->setCurrentIndex(row);auto* composer=window.findChild<QPlainTextEdit*>("composerInput");QVERIFY(composer);composer->setPlainText("Fixture prompt; do not send.");auto* send=window.findChild<QToolButton*>("sendButton");QVERIFY(send&&send->isEnabled());ProviderConfig resolved;QVERIFY(ModelSelection::fromJson(combo->currentData().toJsonObject()).resolve(ProviderStore::load(dir.filePath("providers.json")),&resolved));QCOMPARE(resolved.model,QString("unlisted-only"));}
 void cancelDiscoveryDoesNotPersist(){OfflineProxy offline;QTemporaryDir dir;QVERIFY(seed(dir.path()));const auto before=bytes(dir.filePath("providers.json"));MainWindow window(dir.path());auto* page=settings(window);bool got=false;QCOMPARE(edit(window,page,"first",[&](ProviderDialog* dialog){got=discover(dialog,models({"gpt-5.5","small"}));dialog->reject();}),QString());QVERIFY(got);QCOMPARE(bytes(dir.filePath("providers.json")),before);}
 void legacyReadOnlyUntilExplicitSave(){QTemporaryDir dir;QVERIFY(seed(dir.path(),true));const auto before=bytes(dir.filePath("providers.json"));MainWindow window(dir.path());auto* page=settings(window);QVERIFY(page);QCOMPARE(bytes(dir.filePath("providers.json")),before);QCOMPARE(edit(window,page,"first",[](ProviderDialog* dialog){dialog->reject();}),QString());QCOMPARE(bytes(dir.filePath("providers.json")),before);QCOMPARE(edit(window,page,"first",[](ProviderDialog* dialog){save(dialog);}),QString());const auto saved=ProviderStore::load(dir.filePath("providers.json"));QCOMPARE(effectiveModelCatalog(saved[0]).value("models").toArray().size(),2);QVERIFY(saved[0].disabledModelIds.isEmpty());}
 void conflictingSavePreservesNewerInventoryAndShowsError(){OfflineProxy offline;QTemporaryDir dir;QVERIFY(seed(dir.path()));MainWindow window(dir.path());auto* page=settings(window);QByteArray expected;bool got=false;QCOMPARE(edit(window,page,"first",[&](ProviderDialog* dialog){got=discover(dialog,models({"gpt-5.5","small"}));auto outside=ProviderStore::load(dir.filePath("providers.json"));outside[0].name="Changed externally";ProviderStore::save(dir.filePath("providers.json"),outside);expected=bytes(dir.filePath("providers.json"));save(dialog);}),QString());QVERIFY(got);QCOMPARE(bytes(dir.filePath("providers.json")),expected);bool error=false;for(auto* label:page->findChildren<QLabel*>())error|=label->isVisible()&&label->text().contains("another window");QVERIFY(error);}
 void endpointChangeInvalidatesSnapshotAndDoesNotBorrowLegacy(){QTemporaryDir dir;auto p=provider();QVERIFY(stageModelDiscovery(p,models({"gpt-5.5","old"})));QVERIFY(seed(dir.path()));QVERIFY(ProviderStore::save(dir.filePath("providers.json"),{p}));MainWindow window(dir.path());auto* page=settings(window);QCOMPARE(edit(window,page,"first",[](ProviderDialog* dialog){dialog->findChild<QLineEdit*>("providerBaseUrl")->setText("https://replacement.invalid/v1");save(dialog);}),QString());const auto saved=ProviderStore::load(dir.filePath("providers.json"));QCOMPARE(saved.size(),1);QVERIFY(!saved[0].discoverySnapshot.value("complete").toBool());QVERIFY(effectiveModelCatalog(saved[0],cleanModelCatalog(saved[0],models({"foreign"}))).isEmpty());}
 void renderInventory_data(){QTest::addColumn<bool>("dark");QTest::addColumn<int>("width");for(bool dark:{false,true})for(int width:{620,350})QTest::newRow(qPrintable(QString("%1-%2").arg(dark?"dark":"light").arg(width)))<<dark<<width;}
 void renderInventory(){QFETCH(bool,dark);QFETCH(int,width);QTemporaryDir dir;auto p=provider();p.name="OpenAI API instance";QVERIFY(stageModelDiscovery(p,models({"gpt-5.5","gpt-5.5-pro","small-long-model-identifier-for-eliding-and-a-full-value-tooltip"})));QVERIFY(ProviderStore::save(dir.filePath("providers.json"),{p}));QPalette palette=qApp->palette();palette.setColor(QPalette::Window,dark?QColor("#21242b"):QColor("#fdfdfd"));palette.setColor(QPalette::Base,palette.color(QPalette::Window));palette.setColor(QPalette::Text,dark?Qt::white:Qt::black);palette.setColor(QPalette::WindowText,palette.color(QPalette::Text));QWidget parent;parent.setPalette(palette);ProviderModelsDialog dialog(dir.path(),p.id,{},&parent);dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.resize(width,500);dialog.show();QTest::qWait(40);auto* list=dialog.findChild<QListView*>("providerModelList");QVERIFY(list);QCOMPARE(list->model()->rowCount(),3);QVERIFY(list->width()>200);for(auto* button:dialog.findChildren<QPushButton*>())if(button->isVisible())QVERIFY(dialog.rect().contains(button->mapTo(&dialog,QPoint(button->width()-1,button->height()-1))));const auto output=qEnvironmentVariable("ADE_DISCOVERY_RENDER_DIR");if(!output.isEmpty()){QDir().mkpath(output);QVERIFY(dialog.grab().save(output+QString("/models-%1-%2.png").arg(dark?"dark":"light").arg(width)));}}
};
QTEST_MAIN(ModelDiscoveryHostTest)
#include "test_model_discovery_host.moc"
