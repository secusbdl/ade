#include "ui/SideChatPane.h"
#include "ui/SessionStore.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/AppStyle.h"
#include "ui/QuestionPanel.h"
#include "ui/MessageQueuePanel.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {Conversation chat(const QString& id,const QString& parent={}){Conversation c;c.id=id;c.title=id;c.sideChatParentId=parent;c.providerId="provider";c.model="model";return c;}QJsonArray models(){return {QJsonObject{{"providerInstanceId","provider"},{"modelId","model"},{"label","Fixture model"}}};}}
class SideChatTest:public QObject{
Q_OBJECT
private slots:
void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
void independentDraftsAndScopedSend(){SideChatPane first("one"),second("two");auto a=chat("one","parent"),b=chat("two","parent");a.draft="first  ";b.draft="second";first.setState(a,{},false,models(),{},{});second.setState(b,{},true,models(),{},{});QSignalSpy changes(&first,&SideChatPane::draftChanged),sent(&first,&SideChatPane::submitted);first.composer()->insertPlainText("!");QCOMPARE(changes.size(),1);QCOMPARE(changes[0][0].toString(),"one");first.findChild<QToolButton*>("sideChatSendButton")->click();QCOMPARE(sent.size(),1);QCOMPARE(sent[0][0].toString(),"one");QCOMPARE(second.composer()->toPlainText(),QString("second"));QVERIFY(second.findChild<QToolButton*>("sideChatSendButton")->isEnabled());QVERIFY(!second.findChild<QToolButton*>("promoteSideChatButton")->isEnabled());}
void stateDoesNotEmitPreferenceMutations(){SideChatPane pane("child");QSignalSpy modelsChanged(&pane,&SideChatPane::modelChanged),permissions(&pane,&SideChatPane::permissionChanged),draft(&pane,&SideChatPane::draftChanged);auto c=chat("child","parent");c.draft="kept";c.alwaysAllow=true;pane.setState(c,{},false,models(),{},{});QCOMPARE(modelsChanged.size(),0);QCOMPARE(permissions.size(),0);QCOMPARE(draft.size(),0);QCOMPARE(pane.findChild<QComboBox*>("sideChatModel")->currentData().toJsonObject()["providerInstanceId"].toString(),QString("provider"));QCOMPARE(pane.findChild<QComboBox*>("sideChatPermission")->currentData().toBool(),true);}
void wrongChatStateIgnored(){SideChatPane pane("child");auto c=chat("child","parent");c.draft="original";pane.setState(c,{},false,models(),{},{});auto wrong=chat("other","parent");wrong.draft="overwrite";pane.setState(wrong,{},true,models(),{},{});QCOMPARE(pane.composer()->toPlainText(),QString("original"));}
void imageCallbackCanDeletePane(){auto* pane=new SideChatPane("child");QPointer<SideChatPane> safe=pane;pane->show();connect(pane->transcript(),&ConversationView::imageRequested,pane,[pane]{delete pane;});const QJsonArray rows{QJsonObject{{"id","r"},{"role","assistant"},{"parts",QJsonArray{QJsonObject{{"id","p"},{"type","text"},{"text","![image](path:att/image.png)"},{"state","done"}}}}}};pane->setState(chat("child","parent"),rows,false,models(),{},{});QTRY_VERIFY(!safe);}
void compactComposerControls(){SideChatPane pane("child");auto c=chat("child","parent");c.draft="A compact side chat";pane.setState(c,{},false,models(),{},{});pane.resize(350,350);pane.show();QTest::qWait(20);QVERIFY(pane.width()<=350);for(const auto name:{"sideChatSendButton","sideChatPermission","sideChatModel","promoteSideChatButton"}){auto* control=pane.findChild<QWidget*>(name);QVERIFY(control);QVERIFY(control->isVisible());QVERIFY2(pane.rect().contains(control->mapTo(&pane,QPoint(control->width()-1,control->height()-1))),name);}}
void questionControlsUseAvailableSpaceAndRemainScrollable(){
    SideChatPane pane("child");pane.resize(350,750);pane.show();
    const QJsonObject question{{"id","phase"},{"title","Side question"},{"currentStep",0},{"answers",QJsonObject{}},{"steps",QJsonArray{QJsonObject{{"fields",QJsonArray{QJsonObject{{"questionId","name"},{"label","Name"},{"type","input"},{"required",true}}}}}}}};
    const QJsonArray queued{QJsonObject{{"id","queued"},{"content","Then check the tests"}}};
    pane.setState(chat("child","parent"),{},true,models(),question,queued);
    auto* controls=pane.findChild<QScrollArea*>("sideChatControls");QVERIFY(controls);
    auto* answer=pane.findChild<QLineEdit*>("answer_name");auto* submit=pane.findChild<QPushButton*>("questionSubmit");auto* queueRemove=pane.findChild<QPushButton*>("queueRemove_queued");QVERIFY(answer&&submit&&queueRemove);
    auto visible=[&](QWidget* control){return controls->viewport()->rect().contains(control->mapTo(controls->viewport(),control->rect().center()));};
    QTRY_VERIFY(controls->height()>=260);QTRY_VERIFY(visible(answer));QTRY_VERIFY(visible(submit));QTRY_VERIFY(visible(queueRemove));
    pane.resize(350,250);QTest::qWait(20);QCOMPARE(pane.height(),250);QVERIFY(controls->height()<=120);QVERIFY(controls->verticalScrollBar()->maximum()>0);controls->ensureWidgetVisible(submit);QTRY_VERIFY(visible(submit));
}
void parentRoundTrip(){QTemporaryDir data;SessionStore store(data.path());QVERIFY(store.save({chat("parent"),chat("child","parent")}));const auto rows=SessionStore(data.path()).load();QCOMPARE(rows.size(),2);QCOMPARE(rows[1].sideChatParentId,QString("parent"));}
void invalidParentsPreserveBytes_data(){QTest::addColumn<QString>("parent");QTest::newRow("missing")<<QString("missing");QTest::newRow("self")<<QString("child");}
void invalidParentsPreserveBytes(){QFETCH(QString,parent);QTemporaryDir data;SessionStore store(data.path());QVERIFY(store.save({chat("parent"),chat("child","parent")}));QFile f(data.filePath("sessions.json"));QVERIFY(f.open(QIODevice::ReadOnly));const auto before=f.readAll();f.close();QString error;QVERIFY(!store.save({chat("parent"),chat("child",parent)},&error));QVERIFY(!error.isEmpty());QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),before);}
void nestedAndCyclicParentsRejected(){QTemporaryDir data;SessionStore store(data.path());QString error;QVERIFY(!store.save({chat("parent"),chat("child","parent"),chat("nested","child")},&error));QVERIFY(!store.save({chat("a","b"),chat("b","a")},&error));}
void corruptPersistedParentDoesNotOverwrite(){QTemporaryDir data;SessionStore store(data.path());QVERIFY(store.save({chat("parent"),chat("child","parent")}));QFile f(data.filePath("sessions.json"));QVERIFY(f.open(QIODevice::ReadOnly));auto doc=QJsonDocument::fromJson(f.readAll()).object();f.close();auto entries=doc["sessions"].toArray();auto child=entries[1].toObject();child["sideChatParentId"]="missing";entries[1]=child;doc["sessions"]=entries;QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write(QJsonDocument(doc).toJson());f.close();SessionStore reader(data.path());QString error;QVERIFY(reader.load(&error).isEmpty());QVERIFY(!error.isEmpty());QVERIFY(!reader.save({chat("new")},&error));}
};
QTEST_MAIN(SideChatTest)
#include "test_side_chat.moc"

