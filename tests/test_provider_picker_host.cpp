#include "ui/ProviderDialog.h"
#include "ui/AddProviderDialog.h"
#include "ui/ProviderCatalog.h"
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig existing(){auto p=ProviderCatalog::instantiate("deepseek-api");p.apiKey="fixture-only-existing-secret";p.model="fixture-model";return p;}
QPushButton* button(ProviderDialog& dialog,const QString& text){for(auto* b:dialog.findChildren<QPushButton*>())if(b->text()==text)return b;return nullptr;}
void expose(ProviderDialog& dialog){dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.show();}
}
class ProviderPickerHostTest:public QObject {
 Q_OBJECT
private slots:
 void cancelPreservesCurrentDraftAndProviderCount(){ProviderDialog dialog({existing()});expose(dialog);auto* name=dialog.findChild<QLineEdit*>("providerName");name->setText("Unapplied draft");dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();QVERIFY(picker);QCOMPARE(dialog.providers().size(),1);picker->reject();QCOMPARE(name->text(),QString("Unapplied draft"));QCOMPARE(dialog.providers().size(),1);}
 void choiceFillsExactApiFormWithoutCopyingSecret(){ProviderDialog dialog({existing()});expose(dialog);dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();QVERIFY(picker);picker->findChild<QPushButton*>("providerChoice_google-api")->click();QCOMPARE(dialog.providers().size(),2);QCOMPARE(dialog.findChild<QLineEdit*>("providerName")->text(),QString("Google API"));QCOMPARE(dialog.findChild<QLineEdit*>("providerBaseUrl")->text(),QString("https://generativelanguage.googleapis.com/v1beta"));QVERIFY(dialog.findChild<QLineEdit*>("providerApiKey")->text().isEmpty());QVERIFY(dialog.findChild<QLineEdit*>("providerModel")->text().isEmpty());QCOMPARE(dialog.providers().last().templateId,QString("google-api"));QVERIFY(dialog.findChild<QPushButton*>("providerKeyHelp")->isVisible());QVERIFY(!dialog.findChild<QPushButton*>("providerPlanHelp")->isVisible());}
 void plansShowQuotaHonestyAndDedicatedRoutes(){ProviderDialog dialog({});expose(dialog);dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();picker->findChild<QPushButton*>("providerChoice_minimax-plan")->click();QCOMPARE(dialog.findChild<QLineEdit*>("providerBaseUrl")->text(),QString("https://api.minimax.io/v1"));QVERIFY(dialog.findChild<QLabel*>("providerTemplateHelp")->text().contains("does not check remaining plan quota"));QVERIFY(dialog.findChild<QPushButton*>("providerPlanHelp")->isVisible());}
 void customStartsBlankAndSelectsNoFabricatedModel(){ProviderDialog dialog({existing()});expose(dialog);dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();picker->findChild<QPushButton*>("providerChoice_custom")->click();QVERIFY(dialog.findChild<QLineEdit*>("providerBaseUrl")->text().isEmpty());QVERIFY(dialog.findChild<QLineEdit*>("providerApiKey")->text().isEmpty());QVERIFY(dialog.findChild<QLineEdit*>("providerModel")->text().isEmpty());QVERIFY(!dialog.findChild<QPushButton*>("providerKeyHelp")->isVisible());}
 void codexChoiceShowsActualHarnessControls(){ProviderDialog dialog({existing()});expose(dialog);dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();picker->findChild<QPushButton*>("providerChoice_codex")->click();QVERIFY(dialog.providers().last().isCodexHarness());QVERIFY(dialog.findChild<QPushButton*>("connectCodexSubscription")->isVisible());QVERIFY(!dialog.findChild<QLineEdit*>("providerApiKey")->isVisible());QVERIFY(dialog.providers().last().apiKey.isEmpty());}
 void routingChangeClearsSavedTemplateAndHelp(){ProviderDialog dialog({existing()});expose(dialog);auto* url=dialog.findChild<QLineEdit*>("providerBaseUrl");url->setText("https://different.invalid/v1");QVERIFY(!dialog.findChild<QPushButton*>("providerKeyHelp")->isVisible());button(dialog,"Apply provider")->click();QVERIFY(dialog.providers().first().templateId.isEmpty());QCOMPARE(dialog.providers().first().baseUrl,QString("https://different.invalid/v1"));}
 void unsafeRouteCannotBeApplied(){ProviderDialog dialog({existing()});expose(dialog);const auto before=dialog.providers().first().baseUrl;for(const auto& url:QStringList{"https://user:fixture@example.invalid/v1","https://example.invalid/v1?key=fixture","https://example.invalid/v1#fragment"}){dialog.findChild<QLineEdit*>("providerBaseUrl")->setText(url);button(dialog,"Apply provider")->click();QCOMPARE(dialog.providers().first().baseUrl,before);QCOMPARE(dialog.providers().first().templateId,QString("deepseek-api"));}}
 void stalePickerCannotAddAfterSelectionChanges(){auto first=existing(),second=existing();ProviderDialog dialog({first,second});expose(dialog);dialog.startAddProvider();auto* picker=dialog.findChild<AddProviderDialog*>();QVERIFY(picker);dialog.findChild<QListWidget*>()->setCurrentRow(1);picker->findChild<QPushButton*>("providerChoice_google-api")->click();QCOMPARE(dialog.providers().size(),2);QCOMPARE(dialog.providers().last().id,second.id);}
 void dialogCloseClosesOwnedPicker(){auto* dialog=new ProviderDialog({existing()});expose(*dialog);dialog->startAddProvider();QPointer<AddProviderDialog> picker=dialog->findChild<AddProviderDialog*>();QVERIFY(picker&&picker->isVisible());dialog->reject();QVERIFY(!picker||!picker->isVisible());delete dialog;QVERIFY(!picker);}
 void activeRequestBlocksPickerWithoutAnotherRequest(){QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));auto p=existing();p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());ProviderDialog dialog({p});expose(dialog);button(dialog,"Fetch models")->click();QTRY_VERIFY(server.hasPendingConnections());auto* socket=server.nextPendingConnection();dialog.startAddProvider();QVERIFY(!dialog.findChild<AddProviderDialog*>());QCOMPARE(dialog.providers().size(),1);QVERIFY(!server.hasPendingConnections());socket->deleteLater();}
 void acceptedObserverCanDeleteOwner(){auto* dialog=new ProviderDialog({existing()});QPointer<ProviderDialog> guard(dialog);expose(*dialog);dialog->startAddProvider();auto* picker=dialog->findChild<AddProviderDialog*>();connect(picker,&QDialog::accepted,picker,[dialog]{delete dialog;});picker->findChild<QPushButton*>("providerChoice_google-api")->click();QVERIFY(!guard);}
};
QTEST_MAIN(ProviderPickerHostTest)
#include "test_provider_picker_host.moc"
