#include "ui/MainWindow.h"
#include "ui/ProviderDialog.h"
#include "ui/AddProviderDialog.h"
#include "ui/ProviderCatalog.h"
#include "ui/SettingsPage.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <functional>
using namespace ade;
namespace {
ProviderConfig configuredProvider(){auto p=ProviderCatalog::instantiate("deepseek-api");p.model="fixture-model";p.apiKey="fixture-only-existing-key";return p;}
bool seed(const QString& directory,const QList<ProviderConfig>& providers={}){
    Conversation chat;chat.id="fixture";chat.title="Provider setup fixture";chat.titleLocked=true;
    return SessionStore(directory).save({chat})&&(providers.isEmpty()||ProviderStore::save(directory+"/providers.json",providers));
}
QByteArray bytes(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
SettingsPage* showSettings(MainWindow& window){
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1100,800);window.show();QCoreApplication::processEvents();
    auto* button=window.findChild<QToolButton*>("settingsButton");if(!button)return nullptr;button->click();
    auto* page=window.findChild<SettingsPage*>();if(page)page->selectSection("models-providers");return page;
}
struct ModalResult {bool handled=false,sawPicker=false;QString error;};
// Drives only dialogs owned by this hidden fixture window. The timeout closes
// those dialogs and reports failure instead of leaving a nested event loop.
ModalResult drive(MainWindow* window,bool expectPicker,const std::function<void()>& trigger,
                  const std::function<void(ProviderDialog*,AddProviderDialog*,QString&)>& action){
    ModalResult result;QPointer<MainWindow> owner(window);QTimer timer;QElapsedTimer elapsed;elapsed.start();qint64 editorSeen=-1;
    QObject::connect(&timer,&QTimer::timeout,&timer,[&]{
        if(!owner){timer.stop();return;}
        QPointer<ProviderDialog> editor;for(auto* dialog:owner->findChildren<ProviderDialog*>())if(dialog->isVisible()){editor=dialog;break;}
        QPointer<AddProviderDialog> picker;if(editor)for(auto* dialog:editor->findChildren<AddProviderDialog*>())if(dialog->isVisible()){picker=dialog;break;}
        if(elapsed.elapsed()>4000){result.error="Timed out waiting for the owned provider setup dialog.";timer.stop();if(picker)picker->reject();if(editor)editor->reject();return;}
        if(!editor)return;
        if(editorSeen<0)editorSeen=elapsed.elapsed();
        if(expectPicker&&!picker)return;
        if(!expectPicker&&elapsed.elapsed()-editorSeen<50)return;
        timer.stop();result.handled=true;result.sawPicker=!picker.isNull();action(editor,picker,result.error);
        if(editor&&editor->isVisible())editor->reject();
    });
    timer.start(5);trigger();timer.stop();return result;
}
bool chooseGoogle(ProviderDialog* editor,AddProviderDialog* picker,QString& error){
    if(!picker){error="The Add action did not open the picker.";return false;}
    auto* card=picker->findChild<QPushButton*>("providerChoice_google-api");if(!card){error="The Google API template is missing.";return false;}card->click();
    auto* model=editor->findChild<QLineEdit*>("providerModel");auto* key=editor->findChild<QLineEdit*>("providerApiKey");
    if(!model||!key||!model->text().isEmpty()||!key->text().isEmpty()){error="A new template inherited an existing model or key.";return false;}
    model->setText("fixture-google-model");key->setText("fixture-only-new-key");return true;
}
}
class ProviderPickerWindowTest:public QObject {
    Q_OBJECT
private slots:
    void settingsAddOpensPickerAndCancelPreservesProviders_data(){QTest::addColumn<bool>("existing");QTest::newRow("empty")<<false;QTest::newRow("existing")<<true;}
    void settingsAddOpensPickerAndCancelPreservesProviders(){
        QFETCH(bool,existing);QTemporaryDir data;QVERIFY(seed(data.path(),existing?QList<ProviderConfig>{configuredProvider()}:QList<ProviderConfig>{}));
        const auto before=bytes(data.filePath("providers.json"));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings&&settings->isVisible());auto* add=settings->findChild<QPushButton*>("settingsAddProvider");QVERIFY(add);
        const auto result=drive(&window,true,[add]{add->click();},[](ProviderDialog*,AddProviderDialog* picker,QString&){picker->reject();});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(result.sawPicker);QCOMPARE(bytes(data.filePath("providers.json")),before);QVERIFY(settings->isVisible());
    }
    void settingsEditKeepsEditorWithoutOpeningPicker(){
        QTemporaryDir data;QVERIFY(seed(data.path(),{configuredProvider()}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);QPushButton* edit=nullptr;for(auto* button:settings->findChildren<QPushButton*>())if(button->accessibleName()=="Configure DeepSeek API"&&button->isVisible()){edit=button;break;}QVERIFY(edit);
        const auto result=drive(&window,false,[edit]{edit->click();},[](ProviderDialog* editor,AddProviderDialog* picker,QString& error){if(picker)error="Edit incorrectly opened Add Provider.";if(editor->findChild<QLineEdit*>("providerName")->text()!="DeepSeek API")error="Edit did not retain the configured provider.";});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(!result.sawPicker);
    }
    void settingsSecondCardEditsExactProvider(){
        QTemporaryDir data;const auto first=configuredProvider();auto second=ProviderCatalog::instantiate("google-api");second.model="fixture-google-model";second.apiKey="fixture-only-second-key";
        QVERIFY(seed(data.path(),{first,second}));const auto before=bytes(data.filePath("providers.json"));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);
        QPushButton* edit=nullptr;for(auto* button:settings->findChildren<QPushButton*>())if(button->accessibleName()=="Configure Google API"&&button->isVisible()){edit=button;break;}QVERIFY(edit);
        const auto result=drive(&window,false,[edit]{edit->click();},[&](ProviderDialog* editor,AddProviderDialog* picker,QString& error){
            if(picker){error="Edit incorrectly opened Add Provider.";return;}
            const auto name=editor->findChild<QLineEdit*>("providerName")->text();
            const auto model=editor->findChild<QLineEdit*>("providerModel")->text();
            if(name!=second.name||model!=second.model)error=QString("Second card selected name '%1' and model '%2' instead of '%3' / '%4'.").arg(name,model,second.name,second.model);
        });
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(!result.sawPicker);QCOMPARE(bytes(data.filePath("providers.json")),before);
    }
    void settingsSecondCardSaveChangesOnlyThatInstance(){
        QTemporaryDir data;const auto first=configuredProvider();auto second=ProviderCatalog::instantiate("google-api");second.model="fixture-google-model";second.apiKey="fixture-only-second-key";
        QVERIFY(seed(data.path(),{first,second}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);
        QPushButton* edit=nullptr;for(auto* button:settings->findChildren<QPushButton*>())if(button->accessibleName()=="Configure Google API"&&button->isVisible()){edit=button;break;}QVERIFY(edit);
        const auto result=drive(&window,false,[edit]{edit->click();},[](ProviderDialog* editor,AddProviderDialog* picker,QString& error){
            if(picker||editor->findChild<QLineEdit*>("providerName")->text()!="Google API"){error="The wrong provider was opened.";return;}
            editor->findChild<QLineEdit*>("providerModel")->setText("fixture-google-updated");editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        });
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QString error;const auto saved=ProviderStore::load(data.filePath("providers.json"),&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(saved.size(),2);
        QCOMPARE(saved[0].publicJson(),first.publicJson());QCOMPARE(saved[0].apiKey,first.apiKey);QCOMPARE(saved[1].id,second.id);QCOMPARE(saved[1].model,QString("fixture-google-updated"));QCOMPARE(saved[1].apiKey,second.apiKey);
    }
    void staleProviderCardNeverOpensAnotherInstance(){
        QTemporaryDir data;const auto first=configuredProvider();auto second=ProviderCatalog::instantiate("google-api");second.model="fixture-google-model";second.apiKey="fixture-only-second-key";
        QVERIFY(seed(data.path(),{first,second}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);
        QPushButton* edit=nullptr;for(auto* button:settings->findChildren<QPushButton*>())if(button->accessibleName()=="Configure Google API"&&button->isVisible()){edit=button;break;}QVERIFY(edit);
        QVERIFY(ProviderStore::save(data.filePath("providers.json"),{first}));const auto outside=bytes(data.filePath("providers.json"));
        const auto result=drive(&window,false,[edit]{edit->click();},[](ProviderDialog*,AddProviderDialog*,QString& error){error="A stale card opened a different provider.";});
        QVERIFY2(!result.handled&&result.error.isEmpty(),qPrintable(result.error));QCOMPARE(bytes(data.filePath("providers.json")),outside);
        auto* status=settings->findChild<QLabel*>("settingsStatus");QVERIFY(status&&status->isVisible());QVERIFY(status->text().contains("no longer configured"));
    }
    void emptyGenericProviderRequestStartsPicker(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);
        const auto result=drive(&window,true,[settings]{emit settings->providersRequested();},[](ProviderDialog*,AddProviderDialog*,QString&){});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(result.sawPicker);QVERIFY(!QFile::exists(data.filePath("providers.json")));
    }
    void templateSavePersistsExactNewInstanceAndKeepsExistingKey(){
        QTemporaryDir data;const auto original=configuredProvider();QVERIFY(seed(data.path(),{original}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);auto* add=settings->findChild<QPushButton*>("settingsAddProvider");QVERIFY(add);
        const auto result=drive(&window,true,[add]{add->click();},[](ProviderDialog* editor,AddProviderDialog* picker,QString& error){if(chooseGoogle(editor,picker,error))editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QString error;const auto saved=ProviderStore::load(data.filePath("providers.json"),&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(saved.size(),2);QCOMPARE(saved.first().id,original.id);QCOMPARE(saved.first().apiKey,original.apiKey);const auto& added=saved.last();QVERIFY(added.id!=original.id);QCOMPARE(added.templateId,QString("google-api"));QCOMPARE(added.protocol,QString("google"));QCOMPARE(added.baseUrl,QString("https://generativelanguage.googleapis.com/v1beta"));QCOMPARE(added.model,QString("fixture-google-model"));QCOMPARE(added.apiKey,QString("fixture-only-new-key"));QVERIFY(!bytes(data.filePath("providers.json")).contains("fixture-only-new-key"));
    }
    void concurrentProviderChangeIsPreservedAndReportedInSettings(){
        QTemporaryDir data;const auto original=configuredProvider();QVERIFY(seed(data.path(),{original}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);auto* add=settings->findChild<QPushButton*>("settingsAddProvider");QVERIFY(add);QByteArray externalBytes;
        const auto result=drive(&window,true,[add]{add->click();},[&](ProviderDialog* editor,AddProviderDialog* picker,QString& error){if(!chooseGoogle(editor,picker,error))return;auto external=original;external.name="External update";if(!ProviderStore::save(data.filePath("providers.json"),{external},&error))return;externalBytes=bytes(data.filePath("providers.json"));editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(!externalBytes.isEmpty());QCOMPARE(bytes(data.filePath("providers.json")),externalBytes);auto* status=settings->findChild<QLabel*>("settingsStatus");QVERIFY(status&&status->isVisible());QVERIFY(status->text().contains("another window"));
    }
    void providerModalOwnerCanBeDestroyedWithoutReloadingDeletedSettings(){
        QTemporaryDir data;QVERIFY(seed(data.path(),{configuredProvider()}));QPointer<MainWindow> window=new MainWindow(data.path());auto* settings=showSettings(*window);QVERIFY(settings);auto* add=settings->findChild<QPushButton*>("settingsAddProvider");QVERIFY(add);
        const auto result=drive(window,true,[add]{add->click();},[&](ProviderDialog*,AddProviderDialog*,QString&){delete window.data();});
        QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(!window);
    }
};
int main(int argc,char** argv){QApplication app(argc,argv);initializeAppStyle(app);ProviderPickerWindowTest test;return QTest::qExec(&test,argc,argv);}
#include "test_provider_picker_window.moc"
