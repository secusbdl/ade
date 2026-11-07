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
#include "ui/ModelThinking.h"
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
// Synthetic external harness; never started by these pure-function tests.
ProviderConfig harness(){ProviderConfig p;p.id="harness.instance";p.name="Harness";p.model="agent.model";p.kind="codex";p.protocol="codex-app-server";p.harnessExecutable="missing-fixture.exe";return p;}
QJsonObject catalogs(const ProviderConfig& p){return {{p.id,cleanModelCatalog(p,QJsonArray{QJsonObject{{"id","agent.model"},{"reasoningEfforts",QJsonArray{"low","medium","high"}},{"defaultReasoningEffort","medium"}}})}};}
QJsonObject override(const QString& value,bool enabled=true){return {{"harness.instance",QJsonObject{{"agent.model",QJsonObject{{"enabled",enabled},{"provider","codex"},{"value",value}}}}}};}
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
 // Thinking-effort cycling (S7). These helpers did not exist before; the
 // chain previously ignored agent.modelThinkingOverrides entirely.
 void thinkingOptionsUseOnlyDiscoveredEfforts(){
  const auto p=harness();QJsonObject entry{{"reasoningEfforts",QJsonArray{"low","","low","medium",QString(33,'x'),7,"high"}}};
  QCOMPARE(thinkingOptions(p,entry),QStringList({"low","medium","high"}));
  auto api=p;api.kind="api";api.protocol="openai";QVERIFY(thinkingOptions(api,entry).isEmpty());
  QCOMPARE(thinkingOptions(p,thinkingCatalogEntry(p,"agent.model",catalogs(p))),QStringList({"low","medium","high"}));
  QVERIFY(thinkingCatalogEntry(p,"unknown",catalogs(p)).isEmpty());
 }
 void nextThinkingOptionWrapsAndStartsFromDefault(){
  const QStringList options{"low","medium","high"};
  QCOMPARE(nextThinkingOption(options,"low","medium"),QString("medium"));
  QCOMPARE(nextThinkingOption(options,"high","medium"),QString("low"));
  QCOMPARE(nextThinkingOption(options,"","medium"),QString("high"));
  QCOMPARE(nextThinkingOption(options,"unknown","also-unknown"),QString("low"));
  QCOMPARE(nextThinkingOption({},"low","low"),QString());
 }
 void presetSelectionIgnoresThinkingOverride(){
  const auto p=harness();const ModelSelection preset{p.id,"agent.model","preset-1","Preset",{}};
  QVERIFY(thinkingOverrideValue(preset,{p},catalogs(p),override("high")).isEmpty());
  const QJsonArray presets{QJsonObject{{"id","preset-1"},{"name","Preset"},{"providerInstanceId",p.id},{"modelId","agent.model"}}};
  const auto chain=selectedModelChain(preset,{p},presets,catalogs(p),override("high"));
  QCOMPARE(chain.size(),1);QVERIFY(chain.first().provider.reasoningEffort.isEmpty());QVERIFY(!chain.first().hasThinkingOverride);
 }
 void selectedModelChainAppliesOverrideReasoningEffort(){
  const auto p=harness();const ModelSelection direct{p.id,"agent.model",{},{},{}};
  QVERIFY(selectedModelChain(direct,{p},{},catalogs(p)).first().provider.reasoningEffort.isEmpty());
  const auto chain=selectedModelChain(direct,{p},{},catalogs(p),override("high"));
  QCOMPARE(chain.size(),1);QCOMPARE(chain.first().provider.reasoningEffort,QString("high"));QVERIFY(chain.first().hasThinkingOverride);
  // A disabled entry or an effort no longer discovered is not sent.
  QVERIFY(selectedModelChain(direct,{p},{},catalogs(p),override("high",false)).first().provider.reasoningEffort.isEmpty());
  QVERIFY(selectedModelChain(direct,{p},{},catalogs(p),override("xhigh")).first().provider.reasoningEffort.isEmpty());
 }
 void malformedOverridesAreIgnored(){
  QVERIFY(validThinkingOverrides(QJsonValue(QString("bad"))).isEmpty());
  QVERIFY(validThinkingOverrides(QJsonArray{}).isEmpty());
  const QJsonObject mixed{{"a",QJsonObject{{"m",QJsonObject{{"enabled","yes"},{"provider","codex"},{"value","low"}}},
                                          {"n",QJsonObject{{"enabled",true},{"provider","codex"},{"value",QString(33,'x')}}},
                                          {"o",QJsonObject{{"enabled",true},{"provider","codex"},{"value","low"},{"extra",1}}}}},
                          {"b",QString("not an object")}};
  const auto valid=validThinkingOverrides(mixed);
  QCOMPARE(valid.keys(),QStringList({"a"}));QCOMPARE(valid["a"].toObject().keys(),QStringList({"o"}));
  QVERIFY(!valid["a"].toObject()["o"].toObject().contains("extra"));
  QJsonObject many;for(int i=0;i<300;++i)many.insert(QString("instance%1").arg(i,3,10,QChar('0')),QJsonObject{{"m",QJsonObject{{"enabled",true},{"provider","codex"},{"value","low"}}}});
  QCOMPARE(validThinkingOverrides(many).size(),256);
  const auto p=harness();
  QVERIFY(selectedModelChain({p.id,"agent.model",{},{},{}},{p},{},catalogs(p),validThinkingOverrides(QJsonObject{{p.id,QString("malformed")}})).first().provider.reasoningEffort.isEmpty());
 }
};
QTEST_MAIN(ModelSelectorDisplayTest)
#include "test_model_selector_display.moc"
