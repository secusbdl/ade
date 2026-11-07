#include "ui/CustomModelDialog.h"
#include "ui/ProviderModelsDialog.h"
#include "core/ProviderStore.h"
#include "ui/AppFonts.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(QString id="one"){ProviderConfig p;p.id=id;p.name="Same provider";p.baseUrl="http://127.0.0.1:1/v1";p.model="legacy";return p;}
CustomModelConfig model(QString owner="one",QString id="private-test-model"){CustomModelConfig m;m.modelId=id;m.displayName="Test custom";m.providerInstanceId=owner;return m;}
void fill(CustomModelDialog* dialog,const QString& id="private-test-model") {dialog->findChild<QLineEdit*>("customModelId")->setText(id);dialog->findChild<QLineEdit*>("customModelDisplayName")->setText("Test custom");}
QPushButton* save(CustomModelDialog* d){return d->findChild<QPushButton*>("saveCustomModel");}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
bool selectModel(ProviderModelsDialog* d,const QString& id){auto* list=d->findChild<QListView*>("providerModelList");for(int row=0;row<list->model()->rowCount();++row){const auto index=list->model()->index(row,0);if(index.data(Qt::UserRole+1).toString()==id){list->setCurrentIndex(index);return true;}}return false;}
void edit(ProviderModelsDialog* parent,const char* action,std::function<void(CustomModelDialog*)> handler){QPointer<ProviderModelsDialog> owner(parent);QTimer::singleShot(0,parent,[owner,handler]{if(!owner)return;QPointer<CustomModelDialog> child=owner->findChild<CustomModelDialog*>();if(!child)return;handler(child);if(child&&child->isVisible())child->reject();});parent->findChild<QPushButton*>(action)->click();}
void expose(QWidget* widget,int width=620){widget->setAttribute(Qt::WA_DontShowOnScreen);widget->resize(width,720);widget->show();QApplication::processEvents();}
class CloseOnDisable final:public QObject {
public:
 QPointer<ProviderModelsDialog> dialog;
 bool closed=false;
 bool eventFilter(QObject* watched,QEvent* event)override{if(!closed&&event->type()==QEvent::EnabledChange&&!qobject_cast<QWidget*>(watched)->isEnabled()){closed=true;dialog->reject();}return false;}
};
}
class CustomModelUiTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){const auto fonts=AppFonts::load();QFont font(fonts.body);font.setPixelSize(13);QApplication::setFont(font);}
 void sourceDefaultsTrimAndExactOwner(){CustomModelConfig captured;int commits=0;CustomModelDialog d({provider(),provider("two")},{},"two",[&](const auto& m,QString*){captured=m;++commits;return true;});expose(&d);fill(&d,"  private-test-model  ");d.findChild<QLineEdit*>("customModelDisplayName")->setText("  Test custom  ");save(&d)->click();QCOMPARE(commits,1);QCOMPARE(captured.modelId,QString("private-test-model"));QCOMPARE(captured.displayName,QString("Test custom"));QCOMPARE(captured.providerInstanceId,QString("two"));QCOMPARE(captured.contextWindowSize,128000);QCOMPARE(d.result(),int(QDialog::Accepted));}
 void malformedJsonKeepsDraft_data(){QTest::addColumn<QString>("field");QTest::addColumn<QString>("text");QTest::newRow("syntax")<<QString("customModelOptions")<<QString("{bad");QTest::newRow("array")<<QString("customModelHeaders")<<QString("[]");QTest::newRow("unsupported")<<QString("customModelOptions")<<QString("{\"unsupported_option\":true}");QTest::newRow("typed_header")<<QString("customModelHeaders")<<QString("{\"x-fixture\":12}");}
 void malformedJsonKeepsDraft(){QFETCH(QString,field);QFETCH(QString,text);int commits=0;CustomModelDialog d({provider()},{},"one",[&](const auto&,QString*){++commits;return true;});expose(&d);fill(&d);d.findChild<QPlainTextEdit*>(field)->setPlainText(text);save(&d)->click();QCOMPARE(commits,0);QVERIFY(d.isVisible());QCOMPARE(d.findChild<QPlainTextEdit*>(field)->toPlainText(),text);QVERIFY(!d.findChild<QLabel*>("customModelStatus")->text().isEmpty());}
 void unsupportedImportedCapabilityRequiresExplicitClear(){auto m=model();auto input=m.capabilities.value("inputModalities").toObject();input["audio"]=true;m.capabilities["inputModalities"]=input;int commits=0;CustomModelDialog d({provider()},m,"one",[&](const auto&,QString*){++commits;return true;});expose(&d);save(&d)->click();QCOMPARE(commits,0);auto* audio=d.findChild<QCheckBox*>("customModelCapability_inputModalities.audio");QVERIFY(audio->isChecked());audio->click();QVERIFY(!audio->isChecked());QVERIFY(!audio->isEnabled());save(&d)->click();QCOMPARE(commits,1);}
 void failedCommitPreservesDraftAndCloseDuringCommit(){QPointer<CustomModelDialog> pointer;CustomModelDialog d({provider()},{},"one",[&](const auto&,QString* error){pointer->reject();*error="Fixture conflict";return false;});pointer=&d;expose(&d);fill(&d);d.findChild<QPlainTextEdit*>("customModelHeaders")->setPlainText("{\"x-fixture\":\"SYNTHETIC_HEADER\"}");save(&d)->click();QVERIFY(d.isVisible());QVERIFY(save(&d)->isEnabled());QCOMPARE(d.findChild<QLabel*>("customModelStatus")->text(),QString("Fixture conflict"));QVERIFY(d.findChild<QPlainTextEdit*>("customModelHeaders")->toPlainText().contains("SYNTHETIC_HEADER"));}
 void committerCanDeleteEditor(){QPointer<CustomModelDialog> d=new CustomModelDialog({provider()},{},"one",[&](const auto&,QString*){delete d.data();return true;});expose(d);fill(d);save(d)->click();QVERIFY(!d);}
 void advancedIsCollapsedAndRetainsPrivateDraft(){auto m=model();m.headers={{"x-fixture","SYNTHETIC_EXISTING"}};CustomModelConfig captured;CustomModelDialog d({provider()},m,"one",[&](const auto& value,QString*){captured=value;return true;});expose(&d);auto* advanced=d.findChild<QWidget*>("customModelAdvanced");auto* toggle=d.findChild<QToolButton*>("customModelAdvancedToggle");QVERIFY(!advanced->isVisible());toggle->click();QVERIFY(advanced->isVisible());d.findChild<QPlainTextEdit*>("customModelHeaders")->setPlainText("{\"x-fixture\":\"SYNTHETIC_EDITED\"}");toggle->click();QVERIFY(!advanced->isVisible());save(&d)->click();QCOMPARE(captured.headers.value("x-fixture"),QJsonValue("SYNTHETIC_EDITED"));}
 void addEditMoveDeleteAreDurableAndExact(){
  QTemporaryDir dir;const auto path=dir.filePath("providers.json");QString error;QVERIFY(ProviderStore::save(path,{provider(),provider("two")},&error));ProviderModelsDialog d(dir.path(),"one",{});expose(&d);QSignalSpy changes(&d,&ProviderModelsDialog::modelsChanged);
  edit(&d,"addCustomModel",[](CustomModelDialog* child){fill(child);save(child)->click();});QCOMPARE(changes.size(),1);auto loaded=ProviderStore::load(path,&error);QCOMPARE(loaded[0].customModels.size(),1);QVERIFY(loaded[1].customModels.isEmpty());QVERIFY(selectModel(&d,"private-test-model"));
  edit(&d,"editCustomModel",[](CustomModelDialog* child){child->findChild<QLineEdit*>("customModelId")->setText("renamed-private");auto* providers=child->findChild<QComboBox*>("customModelProvider");providers->setCurrentIndex(providers->findData("two"));save(child)->click();});QCOMPARE(changes.size(),2);loaded=ProviderStore::load(path,&error);QVERIFY(loaded[0].customModels.isEmpty());QVERIFY(loaded[0].retiredCustomModelIds.contains("private-test-model"));QCOMPARE(loaded[1].customModels[0].modelId,QString("renamed-private"));QCOMPARE(loaded[0].model,QString("legacy"));
  ProviderModelsDialog second(dir.path(),"two",{});expose(&second);QVERIFY(selectModel(&second,"renamed-private"));second.findChild<QPushButton*>("deleteCustomModel")->click();loaded=ProviderStore::load(path,&error);QVERIFY(loaded[1].customModels.isEmpty());QVERIFY(loaded[1].retiredCustomModelIds.contains("renamed-private"));
 }
 void cancelledDraftAndConcurrentSavePreserveFile(){
  QTemporaryDir dir;const auto path=dir.filePath("providers.json");QString error;QVERIFY(ProviderStore::save(path,{provider()},&error));ProviderModelsDialog d(dir.path(),"one",{});expose(&d);QSignalSpy changes(&d,&ProviderModelsDialog::modelsChanged);const auto initial=read(path);edit(&d,"addCustomModel",[](CustomModelDialog* child){fill(child);child->reject();});QCOMPARE(read(path),initial);QCOMPARE(changes.size(),0);
  auto p=provider();p.name="External committed";QVERIFY(ProviderStore::save(path,{p},&error));const auto external=read(path);QString failure;bool retained=false;edit(&d,"addCustomModel",[&](CustomModelDialog* child){fill(child);save(child)->click();failure=child->findChild<QLabel*>("customModelStatus")->text();retained=child->isVisible()&&child->findChild<QLineEdit*>("customModelId")->text()=="private-test-model";});QVERIFY(retained);QVERIFY(!failure.isEmpty());QCOMPARE(read(path),external);QCOMPARE(changes.size(),0);
 }
 void durableChangedObserverCanDeleteOwner(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");QString error;QVERIFY(ProviderStore::save(path,{provider()},&error));QPointer<ProviderModelsDialog> d=new ProviderModelsDialog(dir.path(),"one",{});expose(d);connect(d,&ProviderModelsDialog::modelsChanged,d,[&]{delete d.data();});edit(d,"addCustomModel",[](CustomModelDialog* child){fill(child);save(child)->click();});QVERIFY(!d);QCOMPARE(ProviderStore::load(path,&error)[0].customModels.size(),1);}
 void closingOwnerDuringActionDisablesMustNotOpenChild(){QTemporaryDir dir;QString error;QVERIFY(ProviderStore::save(dir.filePath("providers.json"),{provider()},&error));ProviderModelsDialog d(dir.path(),"one",{});expose(&d);CloseOnDisable filter;filter.dialog=&d;auto* add=d.findChild<QPushButton*>("addCustomModel");add->installEventFilter(&filter);bool opened=false;QTimer::singleShot(0,&d,[&]{if(auto* child=d.findChild<CustomModelDialog*>()){opened=true;child->reject();}});add->click();QApplication::processEvents();QVERIFY(filter.closed);QVERIFY(!opened);QVERIFY(!d.isVisible());}
 void darkEditorInteriorUsesModelSurface(){QWidget parent;auto palette=parent.palette();palette.setColor(QPalette::Base,QColor("#21242b"));palette.setColor(QPalette::Window,QColor("#21242b"));palette.setColor(QPalette::Text,QColor("#e8e9ec"));palette.setColor(QPalette::WindowText,QColor("#e8e9ec"));parent.setPalette(palette);CustomModelDialog d({provider()},model(),"one",{},&parent);expose(&d,350);const auto image=d.grab().toImage();QCOMPARE(image.pixelColor(5,5),QColor("#21242b"));auto* scroll=d.findChild<QScrollArea*>();QCOMPARE(image.pixelColor(scroll->viewport()->mapTo(&d,QPoint(1,1))),QColor("#21242b"));auto* toggle=d.findChild<QToolButton*>("customModelAdvancedToggle");QVERIFY(toggle->isEnabled());QCOMPARE(image.pixelColor(toggle->mapTo(&d,QPoint(toggle->width()-8,toggle->height()/2))),QColor("#2c3039"));}
 void narrowControlsRemainReachable_data(){QTest::addColumn<bool>("dark");QTest::newRow("light")<<false;QTest::newRow("dark")<<true;}
 void narrowControlsRemainReachable(){QFETCH(bool,dark);QTemporaryDir dir;QString error;auto p=provider();p.customModels={model()};QVERIFY(ProviderStore::save(dir.filePath("providers.json"),{p},&error));ProviderModelsDialog d(dir.path(),"one",{});if(dark){auto palette=d.palette();palette.setColor(QPalette::Base,QColor("#21242b"));palette.setColor(QPalette::Window,QColor("#21242b"));palette.setColor(QPalette::Text,QColor("#e8e9ec"));palette.setColor(QPalette::WindowText,QColor("#e8e9ec"));d.setPalette(palette);}expose(&d,350);QCOMPARE(d.width(),350);QVERIFY(selectModel(&d,"private-test-model"));for(const auto& name:{"addCustomModel","editCustomModel","deleteCustomModel"}){auto* button=d.findChild<QPushButton*>(name);QVERIFY(button);QVERIFY(button->isEnabled());QVERIFY(d.rect().contains(QRect(button->mapTo(&d,QPoint{}),button->size())));}const auto render=qEnvironmentVariable("ADE_CUSTOM_MODEL_RENDER_DIR");if(!render.isEmpty()){QDir().mkpath(render);QVERIFY(d.grab().save(render+QString("/models-%1-350.png").arg(dark?"dark":"light")));}
  edit(&d,"editCustomModel",[&](CustomModelDialog* child){
   child->findChild<QPlainTextEdit*>("customModelDescription")->setPlainText("Fixture definition on the selected HTTP provider instance.");
   child->findChild<QPlainTextEdit*>("customModelOptions")->setPlainText("{\"user\":\"fixture-user\"}");
   child->findChild<QPlainTextEdit*>("customModelHeaders")->setPlainText("{\"x-fixture\":\"synthetic-local-only\"}");
   for(int width:{350,620}){child->resize(width,720);auto* bar=child->findChild<QScrollArea*>()->verticalScrollBar();child->findChild<QToolButton*>("customModelAdvancedToggle")->setChecked(false);bar->setValue(0);QApplication::processEvents();if(!render.isEmpty())child->grab().save(render+QString("/editor-%1-%2-top.png").arg(dark?"dark":"light").arg(width));child->findChild<QToolButton*>("customModelAdvancedToggle")->setChecked(true);QApplication::processEvents();bar->setValue(bar->maximum());QApplication::processEvents();if(!render.isEmpty())child->grab().save(render+QString("/editor-%1-%2-options.png").arg(dark?"dark":"light").arg(width));}
  });if(!render.isEmpty()){d.resize(620,720);QApplication::processEvents();QVERIFY(d.grab().save(render+QString("/models-%1-620.png").arg(dark?"dark":"light")));}
 }
};
QTEST_MAIN(CustomModelUiTest)
#include "test_custom_model_ui.moc"
