#include "ui/ProviderDialog.h"
#include "ui/ProviderCatalog.h"
#include "ui/ProviderRepository.h"
#include "core/ProviderStore.h"
#include "harness/CodexAppServer.h"
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig configured(QString name="Fixture provider"){auto p=ProviderCatalog::instantiate("deepseek-api");p.name=name;p.model="fixture-model";p.apiKey="fixture-only-"+p.id;return p;}
QByteArray bytes(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray{};}
QPushButton* button(ProviderDialog& dialog,const QString& text){for(auto* b:dialog.findChildren<QPushButton*>())if(b->text()==text)return b;return nullptr;}
void expose(ProviderDialog& dialog){dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.show();}
QMessageBox* beginRemove(ProviderDialog& dialog){dialog.findChild<QPushButton*>("removeProvider")->click();return dialog.findChild<QMessageBox*>("removeProviderConfirmation");}
void answer(QMessageBox* prompt,QMessageBox::StandardButton result){prompt->button(result)->click();}
}
class ProviderRemovalTest:public QObject {
Q_OBJECT
private slots:
 void noSelectionCannotRemove(){ProviderDialog dialog({});expose(dialog);QVERIFY(!dialog.findChild<QPushButton*>("removeProvider")->isEnabled());QVERIFY(!dialog.findChild<QLineEdit*>("providerApiKey")->isEnabled());QVERIFY(dialog.findChild<QPushButton*>("addProvider")->isEnabled());}
 void cancelConfirmationPreservesSelectedDraft(){auto p=configured();ProviderDialog dialog({p});expose(dialog);auto* name=dialog.findChild<QLineEdit*>("providerName");name->setText("Unsaved name");auto* prompt=beginRemove(dialog);QVERIFY(prompt);QCOMPARE(prompt->defaultButton(),prompt->button(QMessageBox::Cancel));QCOMPARE(prompt->textFormat(),Qt::PlainText);answer(prompt,QMessageBox::Cancel);QCOMPARE(dialog.providers().size(),1);QCOMPARE(dialog.providers()[0].id,p.id);QCOMPARE(name->text(),QString("Unsaved name"));}
 void exactSameVendorInstanceAndCatalogRemoved(){auto a=configured("First"),b=configured("Second");QJsonObject catalogs{{a.id,QJsonArray{QJsonObject{{"id","a-model"}}}},{b.id,QJsonArray{QJsonObject{{"id","b-model"}}}}};ProviderDialog dialog({a,b},catalogs);expose(dialog);QVERIFY(dialog.selectProviderId(b.id));auto* prompt=beginRemove(dialog);QVERIFY(prompt);answer(prompt,QMessageBox::Yes);QCOMPARE(dialog.providers().size(),1);QCOMPARE(dialog.providers()[0].publicJson(),a.publicJson());QCOMPARE(dialog.providers()[0].apiKey,a.apiKey);QVERIFY(dialog.modelCatalogs().contains(a.id));QVERIFY(!dialog.modelCatalogs().contains(b.id));QCOMPARE(dialog.findChild<QLineEdit*>("providerName")->text(),a.name);}
 void lastRemovalClearsSecretsAndCanSaveEmpty(){ProviderDialog dialog({configured()});expose(dialog);auto* prompt=beginRemove(dialog);QVERIFY(prompt);answer(prompt,QMessageBox::Yes);QVERIFY(dialog.providers().isEmpty());for(const auto* name:{"providerName","providerBaseUrl","providerApiKey","providerModel"})QVERIFY(dialog.findChild<QLineEdit*>(name)->text().isEmpty());QVERIFY(!dialog.findChild<QPushButton*>("removeProvider")->isEnabled());dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));}
 void outerCancelPreservesEncryptedFile(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto p=configured();QVERIFY(ProviderStore::save(path,{p}));const auto original=bytes(path);ProviderRepository repo(path);ProviderDialog dialog(repo.load());expose(dialog);answer(beginRemove(dialog),QMessageBox::Yes);QVERIFY(dialog.providers().isEmpty());dialog.reject();QCOMPARE(dialog.result(),int(QDialog::Rejected));QCOMPARE(bytes(path),original);}
 void acceptedDraftPersistsOnlyChosenRemoval(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto a=configured("First"),b=configured("Second");QVERIFY(ProviderStore::save(path,{a,b}));ProviderRepository repo(path);ProviderDialog dialog(repo.load());expose(dialog);QVERIFY(dialog.selectProviderId(b.id));answer(beginRemove(dialog),QMessageBox::Yes);dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));QString error;QVERIFY2(repo.save(dialog.providers(),&error),qPrintable(error));const auto saved=ProviderStore::load(path,&error);QCOMPARE(saved.size(),1);QCOMPARE(saved[0].publicJson(),a.publicJson());QCOMPARE(saved[0].apiKey,a.apiKey);QVERIFY(!bytes(path).contains(a.apiKey.toUtf8()));}
 void concurrentWriteBlocksRemovalCommit(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto a=configured("First"),b=configured("Second");QVERIFY(ProviderStore::save(path,{a,b}));ProviderRepository repo(path);ProviderDialog dialog(repo.load());expose(dialog);QVERIFY(dialog.selectProviderId(b.id));answer(beginRemove(dialog),QMessageBox::Yes);auto external=b;external.name="Outside update";QVERIFY(ProviderStore::save(path,{a,external}));const auto outside=bytes(path);dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));QString error;QVERIFY(!repo.save(dialog.providers(),&error));QVERIFY(error.contains("another window"));QCOMPARE(bytes(path),outside);}
 void codexRemovalDoesNotStartCliOrLogOut(){auto p=ProviderCatalog::instantiate("codex");p.harnessExecutable="Z:/fixture-not-installed-codex.exe";ProviderDialog dialog({p});expose(dialog);auto* harness=dialog.findChild<CodexAppServer*>();QVERIFY(harness);QSignalSpy ready(harness,&CodexAppServer::ready),failed(harness,&CodexAppServer::failed);auto* prompt=beginRemove(dialog);QVERIFY(prompt);QVERIFY(prompt->informativeText().contains("sign-in and subscription remain unchanged"));answer(prompt,QMessageBox::Yes);dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));QVERIFY(harness->executable().isEmpty());QCOMPARE(ready.count(),0);QCOMPARE(failed.count(),0);}
 void changedSelectionInvalidatesConfirmation(){auto a=configured("First"),b=configured("Second");ProviderDialog dialog({a,b});expose(dialog);QVERIFY(dialog.selectProviderId(b.id));auto* prompt=beginRemove(dialog);QVERIFY(prompt);dialog.findChild<QListWidget*>()->setCurrentRow(0);answer(prompt,QMessageBox::Yes);QCOMPARE(dialog.providers().size(),2);QCOMPARE(dialog.providers()[1].id,b.id);QCOMPARE(dialog.findChild<QLineEdit*>("providerName")->text(),a.name);}
 void confirmationBlocksOtherMutationsAndSave(){ProviderDialog dialog({configured()});expose(dialog);auto* prompt=beginRemove(dialog);QVERIFY(prompt);QVERIFY(!dialog.findChild<QPushButton*>("addProvider")->isEnabled());QVERIFY(!dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());emit dialog.findChild<QDialogButtonBox*>()->accepted();QVERIFY(dialog.isVisible());QCOMPARE(dialog.providers().size(),1);answer(prompt,QMessageBox::Cancel);}
 void apiRequestBlocksRemoval(){QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));auto p=configured();p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());ProviderDialog dialog({p});expose(dialog);button(dialog,"Fetch models")->click();QTRY_VERIFY(server.hasPendingConnections());auto* socket=server.nextPendingConnection();QVERIFY(!dialog.findChild<QPushButton*>("removeProvider")->isEnabled());QVERIFY(!beginRemove(dialog));QCOMPARE(dialog.providers().size(),1);socket->deleteLater();}
 void closingEditorCancelsPendingRemoval(){auto p=configured();ProviderDialog dialog({p});expose(dialog);QPointer<QMessageBox> prompt=beginRemove(dialog);QVERIFY(prompt);dialog.reject();QCOMPARE(dialog.providers().size(),1);QVERIFY(!prompt||!prompt->isVisible());}
 void rowsRemovedSelectionReplacementWinsAndRestoresSignals(){
    auto a=configured("First"),b=configured("Second"),c=configured("Third");ProviderDialog dialog({a,b,c});expose(dialog);auto* list=dialog.findChild<QListWidget*>();bool selected=false;
    connect(list->model(),&QAbstractItemModel::rowsRemoved,&dialog,[&]{selected=dialog.selectProviderId(c.id);});
    answer(beginRemove(dialog),QMessageBox::Yes);QCOMPARE(dialog.providers().size(),2);QCOMPARE(dialog.findChild<QLineEdit*>("providerName")->text(),c.name);QVERIFY(selected);QVERIFY(!list->signalsBlocked());QCOMPARE(list->currentRow(),1);
 }
 void rowsRemovedCloseStopsObsoleteFormUpdates(){
    auto a=configured("First"),b=configured("Second");ProviderDialog dialog({a,b});expose(dialog);auto* list=dialog.findChild<QListWidget*>();auto* name=dialog.findChild<QLineEdit*>("providerName");QSignalSpy changed(name,&QLineEdit::textChanged);
    connect(list->model(),&QAbstractItemModel::rowsRemoved,&dialog,[&]{dialog.reject();changed.clear();});
    answer(beginRemove(dialog),QMessageBox::Yes);QVERIFY(!dialog.isVisible());QCOMPARE(changed.count(),0);QVERIFY(!list->signalsBlocked());
 }
 void secretClearObserverCanDeleteEditor(){auto* dialog=new ProviderDialog({configured()});QPointer<ProviderDialog> guard(dialog);expose(*dialog);auto* prompt=beginRemove(*dialog);QVERIFY(prompt);connect(dialog->findChild<QLineEdit*>("providerApiKey"),&QLineEdit::textChanged,dialog,[dialog](const QString& value){if(value.isEmpty())delete dialog;});answer(prompt,QMessageBox::Yes);QVERIFY(!guard);}
};
QTEST_MAIN(ProviderRemovalTest)
#include "test_provider_removal.moc"
