#include "ui/MainWindow.h"
#include "ui/AppStyle.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QCompleter>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QDir>
#include "ui/Composer.h"
#include "ui/ModelSelection.h"
#include "core/ProviderStore.h"
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(QString id){ProviderConfig p;p.id=id;p.name=id+" managed continuity smoke with a deliberately long instance label";p.model="gpt-6.1-sol";p.baseUrl="http://127.0.0.1:1/v1";return p;}
bool seed(const QString& path){const auto a=provider("Codex"),b=provider("Second");if(!ProviderStore::save(path+"/providers.json",{a,b}))return false;Conversation first;first.id="first";first.title="First";first.titleLocked=true;first.providerId=a.id;first.model=a.model;Conversation second=first;second.id="second";second.title="Second";second.providerId=b.id;QSettings settings(path+"/window.ini",QSettings::IniFormat);settings.setValue("lastConversationId","first");settings.sync();return SessionStore(path).save({first,second});}
void settle(){QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();}
}
class ModelSelectorDisplayTest:public QObject {
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void idleSelectedLabelDoesNotScrollWithCaret_data(){QTest::addColumn<QString>("theme");QTest::addColumn<int>("width");QTest::newRow("light1100")<<QString("light")<<1100;QTest::newRow("dark1100")<<QString("dark")<<1100;QTest::newRow("light350")<<QString("light")<<350;QTest::newRow("dark350")<<QString("dark")<<350;}
 void idleSelectedLabelDoesNotScrollWithCaret(){
  QFETCH(QString,theme);QFETCH(int,width);QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());QVERIFY(window.findChild<SettingsPage*>()->setPreference("personalization.appearance",theme));window.resize(width,750);window.show();settle();
  auto* combo=window.findChild<QComboBox*>("modelSelector");QVERIFY(combo);auto* edit=combo->lineEdit();QVERIFY(edit);auto* composer=window.findChild<Composer*>("composerInput");QVERIFY(composer);composer->setFocus();settle();QVERIFY(!edit->hasFocus());
  const auto text=edit->text();const auto identity=combo->currentData();QVERIFY(edit->fontMetrics().horizontalAdvance(text)>edit->width());
  edit->setCursorPosition(0);settle();const auto beginning=edit->grab().toImage();edit->setCursorPosition(text.size());settle();const auto end=edit->grab().toImage();
  const auto base=qEnvironmentVariable("ADE_MODEL_SELECTOR_RENDER_DIRECTORY");const auto capture=base.isEmpty()?QString{}:base+"/"+theme+QString::number(width);if(!capture.isEmpty()){QVERIFY(QDir().mkpath(capture));QVERIFY(beginning.save(capture+"/idle-at-beginning.png"));QVERIFY(end.save(capture+"/idle-at-end.png"));QVERIFY(window.grab().save(capture+"/host.png"));}
  QCOMPARE(edit->text(),text);QCOMPARE(combo->currentData(),identity);QCOMPARE(combo->toolTip(),text);QVERIFY(window.rect().contains(QRect(combo->mapTo(&window,QPoint{}),combo->size())));QVERIFY2(beginning==end,"An idle selected model must show the same beginning and right ellipsis regardless of its hidden editing caret.");
 }
 void focusedSearchAndUnmatchedSendKeepExactDraft(){
  QTemporaryDir data;QVERIFY(seed(data.path()));QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));auto p=provider("Local");p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(server.serverPort());QVERIFY(ProviderStore::save(data.filePath("providers.json"),{p}));MainWindow window(data.path());window.resize(1100,750);window.show();settle();auto* combo=window.findChild<QComboBox*>("modelSelector");QVERIFY(combo);combo->setCurrentIndex(0);auto* edit=combo->lineEdit();edit->setFocus();settle();edit->selectAll();QTest::keyClicks(edit,"not a selected model");QCOMPARE(edit->text(),QString("not a selected model"));QCOMPARE(edit->cursorPosition(),edit->text().size());edit->setCursorPosition(3);QTest::keyClicks(edit,"X");QCOMPARE(edit->text(),QString("notX a selected model"));QSignalSpy requests(&server,&QTcpServer::newConnection);window.submitPrompt("  Preserve exact draft  ");QTest::qWait(60);QCOMPARE(requests.size(),0);QCOMPARE(window.findChild<Composer*>("composerInput")->toPlainText(),QString("  Preserve exact draft  "));
 }
 void substringCompletionStillSelectsTheExactInstance(){
  QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());window.resize(1100,750);window.show();settle();auto* combo=window.findChild<QComboBox*>("modelSelector");QVERIFY(combo);auto* edit=combo->lineEdit();edit->setFocus();settle();edit->selectAll();QTest::keyClicks(edit,"SECOND managed");auto* completer=combo->completer();QVERIFY(completer);QTRY_COMPARE(completer->completionCount(),1);auto* popup=completer->popup();QVERIFY(popup);QTRY_VERIFY(popup->isVisible());const auto item=completer->completionModel()->index(0,0);QVERIFY(item.isValid());popup->scrollTo(item);settle();const auto itemRect=popup->visualRect(item);QVERIFY(!itemRect.isEmpty());QTest::mouseClick(popup->viewport(),Qt::LeftButton,Qt::NoModifier,itemRect.center());settle();const auto selected=ModelSelection::fromJson(combo->currentData().toJsonObject());QCOMPARE(selected.providerId,QString("Second"));QCOMPARE(edit->text(),provider("Second").name+" / gpt-6.1-sol");QString error;const auto chats=SessionStore(data.path()).load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));for(const auto& chat:chats)if(chat.id==window.currentSessionId())QCOMPARE(chat.providerId,QString("Second"));
 }
 void restoredSecondInstanceKeepsFullLabelAndIdentity(){
  QTemporaryDir data;QVERIFY(seed(data.path()));
  {MainWindow window(data.path());auto* list=window.findChild<QListWidget*>("agentList");QVERIFY(list);for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole)=="second")list->setCurrentRow(i);QCOMPARE(window.currentSessionId(),QString("second"));QVERIFY(window.close());}
  MainWindow reopened(data.path());QCOMPARE(reopened.currentSessionId(),QString("second"));auto* combo=reopened.findChild<QComboBox*>("modelSelector");const auto selection=ModelSelection::fromJson(combo->currentData().toJsonObject());QCOMPARE(selection.providerId,QString("Second"));QCOMPARE(combo->currentText(),provider("Second").name+" / gpt-6.1-sol");QCOMPARE(combo->lineEdit()->text(),combo->currentText());
 }
};
QTEST_MAIN(ModelSelectorDisplayTest)
#include "test_model_selector_display.moc"
