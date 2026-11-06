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
#include <QMessageBox>
#include <QComboBox>
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

}
class ProviderRemovalWindowTest:public QObject {
Q_OBJECT
private slots:
 void saveRemovesOnlySelectedInstanceAndRetainsUnavailableChatBinding(){
    QTemporaryDir data;auto first=configuredProvider();auto second=configuredProvider();second.name="Second instance";second.apiKey="fixture-only-second-key";QVERIFY(seed(data.path(),{first,second}));
    Conversation chat;chat.id="fixture";chat.title="Provider removal fixture";chat.titleLocked=true;chat.providerId=second.id;chat.model=second.model;QVERIFY(SessionStore(data.path()).save({chat}));
    MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);QPushButton* edit=nullptr;for(auto* b:settings->findChildren<QPushButton*>())if(b->accessibleName()=="Configure Second instance"&&b->isVisible()){edit=b;break;}QVERIFY(edit);
    const auto result=drive(&window,false,[edit]{edit->click();},[](ProviderDialog* editor,AddProviderDialog*,QString& error){auto* remove=editor->findChild<QPushButton*>("removeProvider");if(!remove){error="Remove provider is absent.";return;}remove->click();auto* prompt=editor->findChild<QMessageBox*>("removeProviderConfirmation");if(!prompt){error="Confirmation is absent.";return;}prompt->button(QMessageBox::Yes)->click();editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});
    QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QString error;const auto providers=ProviderStore::load(data.filePath("providers.json"),&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(providers.size(),1);QCOMPARE(providers[0].id,first.id);QCOMPARE(providers[0].apiKey,first.apiKey);
    auto* models=window.findChild<QComboBox*>("modelSelector");QVERIFY(models);QCOMPARE(models->currentText(),QString("Saved provider unavailable"));QCOMPARE(models->currentData().toJsonObject().value("providerInstanceId").toString(),second.id);
    const auto saved=SessionStore(data.path()).load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(saved[0].providerId,second.id);
 }
 void cancelAfterRemovalPreservesEncryptedProviders(){
    QTemporaryDir data;const auto original=configuredProvider();QVERIFY(seed(data.path(),{original}));const auto before=bytes(data.filePath("providers.json"));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);
    const auto result=drive(&window,false,[settings,id=original.id]{emit settings->providerEditRequested(id);},[](ProviderDialog* editor,AddProviderDialog*,QString& error){auto* remove=editor->findChild<QPushButton*>("removeProvider");if(!remove){error="Remove provider is absent.";return;}remove->click();auto* prompt=editor->findChild<QMessageBox*>("removeProviderConfirmation");if(!prompt){error="Confirmation is absent.";return;}prompt->button(QMessageBox::Yes)->click();});
    QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QCOMPARE(bytes(data.filePath("providers.json")),before);
 }
 void removalCasConflictPreservesExternalFileAndVisibleError(){
    QTemporaryDir data;const auto original=configuredProvider();QVERIFY(seed(data.path(),{original}));MainWindow window(data.path());auto* settings=showSettings(window);QVERIFY(settings);QByteArray externalBytes;
    const auto result=drive(&window,false,[settings,id=original.id]{emit settings->providerEditRequested(id);},[&](ProviderDialog* editor,AddProviderDialog*,QString& error){auto* remove=editor->findChild<QPushButton*>("removeProvider");if(!remove){error="Remove provider is absent.";return;}remove->click();auto* prompt=editor->findChild<QMessageBox*>("removeProviderConfirmation");if(!prompt){error="Confirmation is absent.";return;}prompt->button(QMessageBox::Yes)->click();auto external=original;external.name="Outside update";if(!ProviderStore::save(data.filePath("providers.json"),{external},&error))return;externalBytes=bytes(data.filePath("providers.json"));editor->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();});
    QVERIFY2(result.handled&&result.error.isEmpty(),qPrintable(result.error));QVERIFY(!externalBytes.isEmpty());QCOMPARE(bytes(data.filePath("providers.json")),externalBytes);auto* status=settings->findChild<QLabel*>("settingsStatus");QVERIFY(status&&status->isVisible());QVERIFY(status->text().contains("another window"));
 }
};
int main(int argc,char** argv){QApplication app(argc,argv);initializeAppStyle(app);ProviderRemovalWindowTest test;return QTest::qExec(&test,argc,argv);}
#include "test_provider_removal_window.moc"
