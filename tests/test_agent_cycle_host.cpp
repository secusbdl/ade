#include "ui/AgentCycleController.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include <QApplication>
#include <QKeyEvent>
#include <QListWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
bool seed(const QString& dir) {
    QList<Conversation> chats;
    for(int i=0;i<3;++i){Conversation c;c.id=QString("agent%1").arg(i);c.title=c.id;c.draft="Saved draft "+c.id;c.created=100+i;c.updated=200+i;c.lastMessageAt=200+i;chats.append(c);}
    return SessionStore(dir).save(chats);
}
void activate(MainWindow& w) {w.setAttribute(Qt::WA_DontShowOnScreen);w.resize(1200,800);w.show();QApplication::setActiveWindow(&w);QApplication::processEvents();}
void key(QWidget* target,QEvent::Type type,int key,Qt::KeyboardModifiers mods={}) {QKeyEvent e(type,key,mods);QApplication::sendEvent(target,&e);}
}
class AgentCycleHostTest:public QObject {
    Q_OBJECT
private slots:
    void destructionRetiresGlobalFilterBeforeWidgets_data() {
        QTest::addColumn<bool>("preview");
        QTest::newRow("idle") << false;
        QTest::newRow("preview-active") << true;
    }
    void destructionRetiresGlobalFilterBeforeWidgets() {
        QFETCH(bool, preview);
        for (int iteration = 0; iteration < 3; ++iteration) {
            QTemporaryDir data; QVERIFY(seed(data.path()));
            auto* w = new MainWindow(data.path()); activate(*w);
            QPointer<AgentCycleController> cycle = w->findChild<AgentCycleController*>(); QVERIFY(cycle);
            if (preview) { cycle->cycle(1); QVERIFY(cycle->active()); }
            bool centralDestroyed = false, filterRetired = false;
            connect(w->centralWidget(), &QObject::destroyed, this, [&] {
                centralDestroyed = true;
                filterRetired = cycle.isNull();
                // Global filters see unrelated events too. Teardown must retire
                // the host callback before it can touch a destroyed page layout.
                QEvent event(QEvent::User); QApplication::sendEvent(this, &event);
            });
            delete w;
            QVERIFY(centralDestroyed); QVERIFY(filterRetired); QVERIFY(cycle.isNull());
            QApplication::processEvents();
        }
    }
    void pageAliasesFollowMruAndCommitImmediately() {
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow w(data.path());activate(w);
        auto* c=w.findChild<AgentCycleController*>();auto* input=w.findChild<Composer*>("composerInput");QVERIFY(c&&input);
        input->setFocus();
        QTest::keyClick(input,Qt::Key_3,Qt::ControlModifier);QCOMPARE(w.currentSessionId(),QString("agent0"));
        input->setPlainText("Draft on previous agent");
        QTest::keyClick(input,Qt::Key_1,Qt::ControlModifier);QCOMPARE(w.currentSessionId(),QString("agent2"));
        input->setPlainText("Draft on current agent");
        // Source switchAgentImmediate uses the same MRU snapshot as Ctrl+Tab.
        // Sidebar order here is agent2,agent1,agent0; MRU must choose agent0.
        QTest::keyClick(input,Qt::Key_PageDown,Qt::ControlModifier);
        QCOMPARE(w.currentSessionId(),QString("agent0"));QVERIFY(!c->active());
        QCOMPARE(input->toPlainText(),QString("Draft on previous agent"));
        QTest::keyClick(input,Qt::Key_PageDown,Qt::ControlModifier);
        QCOMPARE(w.currentSessionId(),QString("agent2"));QVERIFY(!c->active());
        QCOMPARE(input->toPlainText(),QString("Draft on current agent"));
        // Reverse stepping wraps to the least recent visible entry.
        QTest::keyClick(input,Qt::Key_PageUp,Qt::ControlModifier);
        QCOMPARE(w.currentSessionId(),QString("agent1"));QVERIFY(!c->active());
    }
    void previewKeepsActualDraftThenCommitPersistsAndRestartRestores() {
        QTemporaryDir data;QVERIFY(seed(data.path()));QString original,selected;
        {
            MainWindow w(data.path());activate(w);auto* c=w.findChild<AgentCycleController*>();QVERIFY(c);
            auto* input=w.findChild<Composer*>("composerInput");QVERIFY(input);input->setFocus();input->setPlainText("Unsent cycle draft");original=w.currentSessionId();
            key(input,QEvent::KeyPress,Qt::Key_Tab,Qt::ControlModifier);QVERIFY(c->active());selected=c->previewId();QVERIFY(!selected.isEmpty());QVERIFY(selected!=original);
            QCOMPARE(w.currentSessionId(),original);QCOMPARE(input->toPlainText(),QString("Unsent cycle draft"));QCOMPARE(QApplication::focusWidget(),input);
            key(input,QEvent::KeyRelease,Qt::Key_Control);QCOMPARE(w.currentSessionId(),selected);QVERIFY(!c->active());
            QString error;auto saved=SessionStore(data.path()).load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));bool found=false;
            for(const auto& row:saved)if(row.id==original){found=true;QCOMPARE(row.draft,QString("Unsent cycle draft"));}QVERIFY(found);
            w.close();
        }
        MainWindow reopened(data.path());activate(reopened);QCOMPARE(reopened.currentSessionId(),selected);
    }
    void persistenceFailureDoesNotSwitchOrReorderMru() {
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow w(data.path());activate(w);auto* c=w.findChild<AgentCycleController*>();QVERIFY(c);
        auto* input=w.findChild<Composer*>("composerInput");input->setPlainText("Keep despite failed save");const auto original=w.currentSessionId();
        SessionStore external(data.path());QString error;auto chats=external.load(&error);QVERIFY(error.isEmpty());chats[0].title="Concurrent writer";QVERIFY(external.save(chats,&error));
        key(input,QEvent::KeyPress,Qt::Key_Tab,Qt::ControlModifier);const auto preview=c->previewId();QVERIFY(preview!=original);
        key(input,QEvent::KeyRelease,Qt::Key_Control);QCOMPARE(w.currentSessionId(),original);QCOMPARE(c->currentId(),original);QCOMPARE(input->toPlainText(),QString("Keep despite failed save"));
        key(input,QEvent::KeyPress,Qt::Key_Tab,Qt::ControlModifier);QCOMPARE(c->previewId(),preview);c->cancel();
    }
    void openingSettingsCancelsWithoutSwitching() {
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow w(data.path());activate(w);auto* c=w.findChild<AgentCycleController*>();QVERIFY(c);
        auto* input=w.findChild<Composer*>("composerInput");const auto current=w.currentSessionId();key(input,QEvent::KeyPress,Qt::Key_Tab,Qt::ControlModifier);QVERIFY(c->active());
        w.findChild<QToolButton*>("settingsButton")->click();QApplication::processEvents();QVERIFY(!c->active());QCOMPARE(w.currentSessionId(),current);
    }
};
int main(int argc,char** argv){qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);initializeAppStyle(app);AgentCycleHostTest test;return QTest::qExec(&test,argc,argv);}
#include "test_agent_cycle_host.moc"
