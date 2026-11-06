#include "ui/ProviderDialog.h"
#include "core/ProviderService.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>
using namespace ade;
namespace {
struct Server:QObject {
 QTcpServer server;QPointer<QTcpSocket> pending;
 Server(){server.listen(QHostAddress::LocalHost,0);connect(&server,&QTcpServer::newConnection,this,[this]{pending=server.nextPendingConnection();connect(pending,&QTcpSocket::readyRead,this,[this]{if(pending)pending->readAll();});});}
 ProviderConfig api()const{ProviderConfig p;p.id="api";p.name="API";p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());p.model="api-old";return p;}
 void reply(){if(!pending)return;const QByteArray body=R"({"data":[{"id":"api-new"}]})";pending->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);pending->disconnectFromHost();}
};
ProviderConfig codex(){ProviderConfig p;p.id="codex";p.kind="codex";p.name="Codex";p.protocol="codex-app-server";p.model="codex-old";p.harnessExecutable=QDir(QCoreApplication::applicationDirPath()).filePath("harness_fixture.exe");return p;}
QPushButton* button(ProviderDialog& dialog,const QString& text){for(auto* b:dialog.findChildren<QPushButton*>())if(b->text()==text)return b;return nullptr;}
void closeModelPicker(){for(auto* w:QApplication::topLevelWidgets())if(auto* picker=qobject_cast<QInputDialog*>(w))picker->accept();}
}
class ProviderOperationTests:public QObject {
 Q_OBJECT
private slots:
 void apiRequestLocksAddCodex(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);QVERIFY(!dialog.findChild<QPushButton*>("addCodexProvider")->isEnabled());QVERIFY(!dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
 }
 void switchingProviderInvalidatesPendingApiModels(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);dialog.findChild<QListWidget*>()->setCurrentRow(1);
  QTimer accept;connect(&accept,&QTimer::timeout,&dialog,closeModelPicker);accept.start(5);server.reply();QTest::qWait(100);QCOMPARE(dialog.findChild<QLineEdit*>("providerModel")->text(),QString("codex-old"));QVERIFY(!dialog.modelCatalogs().contains("codex"));
 }
 void completionUnlockObserverCannotRetargetApiModels(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);auto* service=dialog.findChild<ProviderService*>();QVERIFY(service);connect(service,&ProviderService::busyChanged,&dialog,[&](bool busy){if(!busy)dialog.findChild<QListWidget*>()->setCurrentRow(1);});
  QTimer accept;connect(&accept,&QTimer::timeout,&dialog,closeModelPicker);accept.start(5);server.reply();QTest::qWait(100);QCOMPARE(dialog.findChild<QLineEdit*>("providerModel")->text(),QString("codex-old"));
 }
 void harnessRequestLocksAllProviderCreation(){
  ProviderDialog dialog({codex()});dialog.show();dialog.findChild<QPushButton*>("connectCodexSubscription")->click();QVERIFY(!button(dialog,"+ Add Provider")->isEnabled());QVERIFY(!button(dialog,"Import API key file…")->isEnabled());QVERIFY(!dialog.findChild<QPushButton*>("addCodexProvider")->isEnabled());QVERIFY(!dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
 }
 void pendingOperationOwnerCanBeDestroyed_data(){QTest::addColumn<bool>("parent");QTest::newRow("dialog")<<false;QTest::newRow("window-owner")<<true;}
 void pendingOperationOwnerCanBeDestroyed(){
  QFETCH(bool,parent);Server server;auto* owner=new QWidget;auto* dialog=new ProviderDialog({server.api(),codex()},{},owner);QPointer<ProviderDialog> guard(dialog);dialog->show();button(*dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);if(parent)delete owner;else{delete dialog;delete owner;}QVERIFY(guard.isNull());server.reply();QTest::qWait(25);
 }
 void modalPickerCannotWriteIntoChangedProvider(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);bool opened=false;
  QTimer act;connect(&act,&QTimer::timeout,&dialog,[&]{for(auto* w:QApplication::topLevelWidgets())if(auto* picker=qobject_cast<QInputDialog*>(w)){opened=true;dialog.findChild<QListWidget*>()->setCurrentRow(1);picker->accept();}});act.start(5);server.reply();QTRY_VERIFY(opened);QCOMPARE(dialog.findChild<QLineEdit*>("providerModel")->text(),QString("codex-old"));QVERIFY(!dialog.modelCatalogs().contains("codex"));
 }
 void modalPickerOwnerCanBeDestroyed(){
  Server server;auto* owner=new QWidget;auto* dialog=new ProviderDialog({server.api(),codex()},{},owner);QPointer<ProviderDialog> guard(dialog);dialog->show();button(*dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);
  QTimer act;connect(&act,&QTimer::timeout,this,[&]{for(auto* w:QApplication::topLevelWidgets())if(qobject_cast<QInputDialog*>(w)){act.stop();delete owner;owner=nullptr;return;}});act.start(5);server.reply();QTRY_VERIFY(guard.isNull());
 }
 void rejectedDialogDisconnectsStaleFailures(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Fetch models")->click();QTRY_VERIFY(server.pending);QPointer<ProviderService> service=dialog.findChild<ProviderService*>();dialog.reject();QVERIFY(service);QVERIFY(!service->isBusy());
  const auto before=dialog.modelCatalogs();emit service->modelsReady({QJsonObject{{"id","stale"}}});emit service->validationFinished(true,"stale-validation");emit service->discoveryFailed("stale-error");QCOMPARE(dialog.modelCatalogs(),before);QCOMPARE(dialog.findChild<QLineEdit*>("providerModel")->text(),QString("api-old"));
 }
 void validationLocksAddCodexAndReleasesOnSwitch(){
  Server server;ProviderDialog dialog({server.api(),codex()});dialog.show();button(dialog,"Test connection")->click();QTRY_VERIFY(server.pending);QVERIFY(!dialog.findChild<QPushButton*>("addCodexProvider")->isEnabled());dialog.findChild<QListWidget*>()->setCurrentRow(1);QVERIFY(dialog.findChild<QPushButton*>("addCodexProvider")->isEnabled());QVERIFY(dialog.findChild<QPushButton*>("connectCodexSubscription")->isEnabled());QVERIFY(!dialog.findChild<QLineEdit*>("providerApiKey")->isEnabled());
 }
};
QTEST_MAIN(ProviderOperationTests)
#include "test_provider_operations.moc"
