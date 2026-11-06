#include "ui/AgentCycleController.h"
#include "ui/AppFonts.h"
#include <QApplication>
#include <QDialog>
#include <QDynamicPropertyChangeEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
using namespace ade;
namespace {
class PropertyObserver : public QObject {
public:
    std::function<void()> action;
    bool fired = false;
    bool eventFilter(QObject* object,QEvent* event) override {
        if (!fired && event->type()==QEvent::DynamicPropertyChange
            && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()=="previewAgentId"
            && object->property("previewAgentId")=="b") {
            fired=true;const QPointer<QObject> target(object);action();return target.isNull();
        }
        return false;
    }
};
void key(QWidget* target, QEvent::Type type, int code, Qt::KeyboardModifiers modifiers = {}, bool repeat = false) {
    QKeyEvent event(type, code, modifiers, {}, repeat);
    QApplication::sendEvent(target, &event);
}
struct Fixture {
    QWidget window;
    QListWidget* list;
    QPlainTextEdit* input;
    AgentCycleController* cycle;
    QString selected = "a";
    int commits = 0;
    bool enabled = true;
    Fixture() {
        auto* row = new QHBoxLayout(&window);
        list = new QListWidget; list->setObjectName("agentList"); list->setFixedWidth(230);
        row->addWidget(list);
        auto* chat = new QVBoxLayout;
        auto* label = new QLabel("Current conversation stays open while previewing");
        label->setWordWrap(true); chat->addWidget(label); chat->addStretch();
        input = new QPlainTextEdit; input->setObjectName("draft"); input->setMaximumHeight(110);
        input->setPlainText("Unsent draft stays exactly here."); chat->addWidget(input);
        row->addLayout(chat, 1);
        cycle = new AgentCycleController(&window, list);
        cycle->setActivationGuard([this] { return enabled; });
        rows({"a", "b", "c"}); cycle->observeCurrent("a");
        QObject::connect(cycle, &AgentCycleController::openRequested, &window, [this](const QString& id) {
            selected = id; ++commits; cycle->observeCurrent(id);
        });
        window.resize(850, 520); window.show(); QApplication::setActiveWindow(&window);
        input->setFocus(); QApplication::processEvents();
    }
    void rows(const QStringList& ids) {
        list->clear();
        for (const auto& id : ids) {
            auto* item = new QListWidgetItem("Agent " + id.toUpper(), list);
            item->setData(Qt::UserRole, id); item->setSizeHint(QSize(0, 32));
        }
        cycle->setAvailableIds(ids);
        if (list->count()) list->setCurrentRow(0);
    }
    void tab(bool reverse = false) { key(input, QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier | (reverse ? Qt::ShiftModifier : Qt::NoModifier)); }
    void release() { key(input, QEvent::KeyRelease, Qt::Key_Control); }
};
}
class AgentCycleTest : public QObject {
    Q_OBJECT
private slots:
    void previewDoesNotSelectOrAlterDraftFocus() {
        Fixture f; auto* original = f.list->currentItem(); const auto text = f.input->toPlainText();
        f.tab(); QVERIFY(f.cycle->active()); QCOMPARE(f.cycle->previewId(), QString("b"));
        QCOMPARE(f.selected, QString("a")); QCOMPARE(f.commits, 0); QCOMPARE(f.list->currentItem(), original);
        QCOMPARE(QApplication::focusWidget(), f.input); QCOMPARE(f.input->toPlainText(), text);
        key(f.input, QEvent::KeyRelease, Qt::Key_Tab, Qt::ControlModifier);
        QCOMPARE(f.commits, 0); f.release(); QCOMPARE(f.selected, QString("b")); QCOMPARE(f.commits, 1);
        QVERIFY(!f.cycle->active()); QCOMPARE(f.input->toPlainText(), text); QCOMPARE(QApplication::focusWidget(), f.input);
    }
    void mruSnapshotDeduplicatesVisibleOrderAndStaysFixed() {
        Fixture f; f.rows({"a", "b", "c", "b", "d"});
        f.cycle->observeCurrent("b"); f.cycle->observeCurrent("c"); f.cycle->observeCurrent("a");
        f.tab(); QCOMPARE(f.cycle->snapshot(), QStringList({"a", "c", "b", "d"})); QCOMPARE(f.cycle->previewId(), QString("c"));
        f.rows({"e", "d", "c", "b", "a"}); f.tab();
        QCOMPARE(f.cycle->snapshot(), QStringList({"a", "c", "b", "d"})); QCOMPARE(f.cycle->previewId(), QString("b"));
        f.release(); QCOMPARE(f.selected, QString("b"));
        f.tab(); QCOMPARE(f.cycle->snapshot(), QStringList({"b", "a", "c", "e", "d"}));
    }
    void reverseWrapAndNoCurrentFollowSource() {
        Fixture f; f.tab(true); QCOMPARE(f.cycle->previewId(), QString("c")); f.tab(true); QCOMPARE(f.cycle->previewId(), QString("b"));
        f.cycle->cancel(); f.cycle->observeCurrent({}); f.tab(); QCOMPARE(f.cycle->previewId(), QString("a"));
        f.cycle->cancel(); f.tab(true); QCOMPARE(f.cycle->previewId(), QString("c"));
    }
    void repeatDoesNotAdvanceAndEscapeCancels() {
        Fixture f; f.tab();
        key(f.input, QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier, true); QCOMPARE(f.cycle->previewId(), QString("b"));
        key(f.input, QEvent::KeyRelease, Qt::Key_Control, {}, true); QVERIFY(f.cycle->active());
        key(f.input, QEvent::KeyPress, Qt::Key_Escape); QVERIFY(!f.cycle->active()); f.release(); QCOMPARE(f.commits, 0);
    }
    void blurCommitsButSettingsAndModalCancel() {
        Fixture f; f.tab(); QEvent blur(QEvent::WindowDeactivate); QApplication::sendEvent(&f.window, &blur);
        QCOMPARE(f.selected, QString("b")); QCOMPARE(f.commits, 1);
        f.tab(); f.enabled = false; QApplication::sendEvent(&f.window, &blur); QVERIFY(!f.cycle->active()); QCOMPARE(f.commits, 1);
        f.enabled = true; f.tab(); QDialog modal(&f.window); modal.setModal(true); modal.show(); QApplication::processEvents();
        QVERIFY(!f.cycle->active()); QCOMPARE(f.commits, 1); modal.close();
    }
    void deletedPreviewDoesNotOpenAndDeletedSnapshotSlotsAreSkipped() {
        Fixture f; f.tab(); f.cycle->setAvailableIds({"a", "c"}); QCOMPARE(f.cycle->previewId(), QString());
        f.release(); QCOMPARE(f.commits, 0);
        f.cycle->setAvailableIds({"a", "b", "c"}); f.tab(); f.cycle->setAvailableIds({"a", "c"}); f.tab();
        QCOMPARE(f.cycle->previewId(), QString("c")); f.release(); QCOMPARE(f.selected, QString("c"));
    }
    void hiddenHeadersAndUnavailableIdsAreNotCandidates() {
        Fixture f; f.list->item(1)->setHidden(true);
        auto* header = new QListWidgetItem("Today", f.list); header->setFlags(Qt::ItemIsEnabled);
        auto* unknown = new QListWidgetItem("Unavailable", f.list); unknown->setData(Qt::UserRole, "removed");
        f.tab(); QCOMPARE(f.cycle->snapshot(), QStringList({"a", "c"})); f.release(); QCOMPARE(f.selected, QString("c"));
    }
    void emptyAndSingleCandidateNeverCauseSpuriousSelection() {
        Fixture f; f.rows({}); f.tab(); QVERIFY(!f.cycle->active()); f.release(); QCOMPARE(f.commits, 0);
        f.rows({"a"}); f.tab(); QVERIFY(f.cycle->active()); f.release(); QCOMPARE(f.commits, 0);
    }
    void otherWindowAndNonMatchingModifiersPassThrough() {
        Fixture f; QPlainTextEdit other; other.show(); QApplication::setActiveWindow(&other);
        key(&other, QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier); QVERIFY(!f.cycle->active());
        key(f.input, QEvent::KeyPress, Qt::Key_Tab, Qt::ControlModifier | Qt::AltModifier); QVERIFY(!f.cycle->active());
    }
    void changingCurrentCancelsPreviewWithoutRestoringStaleOwner() {
        Fixture f; f.tab(); f.cycle->observeCurrent("c"); f.release(); QCOMPARE(f.commits, 0); QCOMPARE(f.cycle->currentId(), QString("c"));
    }
    void commitObserverMayDeleteController() {
        Fixture f; QObject::disconnect(f.cycle, nullptr, &f.window, nullptr);
        QPointer<AgentCycleController> cycle(f.cycle);
        QObject::connect(f.cycle, &AgentCycleController::openRequested, &f.window, [&] { delete cycle.data(); });
        f.tab(); f.release(); QVERIFY(cycle.isNull());
    }
    void previewObserverMayDeleteControllerOrReplaceCycle_data() {
        QTest::addColumn<bool>("destroy"); QTest::addColumn<bool>("immediate");
        QTest::newRow("destroy") << true << false;
        QTest::newRow("replace") << false << false;
        QTest::newRow("immediate-destroy") << true << true;
        QTest::newRow("immediate-replace") << false << true;
    }
    void previewObserverMayDeleteControllerOrReplaceCycle() {
        QFETCH(bool, destroy); QFETCH(bool, immediate); Fixture f; QPointer<AgentCycleController> cycle(f.cycle); bool once = false;
        QObject::connect(f.cycle, &AgentCycleController::previewChanged, &f.window, [&](const QString& id) {
            if (once || id.isEmpty()) return; once = true;
            if (destroy) delete cycle.data(); else { cycle->cancel(); cycle->cycle(-1); }
        });
        if (immediate) f.cycle->stepImmediately(1); else f.tab();
        QCOMPARE(f.commits, 0);
        if (destroy) QVERIFY(cycle.isNull()); else { QCOMPARE(cycle->previewId(), QString("c")); f.release(); QCOMPARE(f.selected, QString("c")); }
    }
    void closingOwnerCancelsWithoutCommit() {
        Fixture f; f.tab(); f.window.close(); QVERIFY(!f.cycle->active()); QCOMPARE(f.commits, 0);
    }
    void rejectedHostSelectionDoesNotAdoptRequestedMru() {
        Fixture f; QObject::disconnect(f.cycle, nullptr, &f.window, nullptr);
        QSignalSpy requests(f.cycle,&AgentCycleController::openRequested);
        f.tab(); f.release(); QCOMPARE(requests.size(),1);
        QCOMPARE(f.cycle->currentId(),QString("a")); QCOMPARE(f.cycle->history(),QStringList({"a"}));
        f.tab(); QCOMPARE(f.cycle->previewId(),QString("b"));
    }
    void clearedPreviewObserverCanReplaceCycleWithoutStaleCommit() {
        Fixture f; bool replace=true;
        QObject::connect(f.cycle,&AgentCycleController::previewChanged,&f.window,[&](const QString& id){
            if(id.isEmpty()&&replace){replace=false;f.cycle->cycle(-1);}
        });
        f.tab(); f.release(); QCOMPARE(f.commits,0); QCOMPARE(f.cycle->previewId(),QString("c"));
        f.release(); QCOMPARE(f.selected,QString("c")); QCOMPARE(f.commits,1);
    }
    void scrollObserverCanDeleteController() {
        Fixture f; QStringList ids;for(int i=0;i<50;++i)ids.append(QString::number(i));f.rows(ids);
        f.cycle->observeCurrent("49");f.cycle->observeCurrent("0"); QPointer<AgentCycleController> cycle(f.cycle);
        QObject::connect(f.list->verticalScrollBar(),&QScrollBar::valueChanged,&f.window,[&]{delete cycle.data();});
        f.tab();QVERIFY(cycle.isNull());
    }
    void blurCommitObserverCanDestroyOwner() {
        QPointer<QWidget> window=new QWidget;auto* list=new QListWidget(window);
        for(const auto& id:{QString("a"),QString("b")}){auto* item=new QListWidgetItem(id,list);item->setData(Qt::UserRole,id);}
        auto* cycle=new AgentCycleController(window,list);cycle->setAvailableIds({"a","b"});cycle->observeCurrent("a");
        window->show();QApplication::setActiveWindow(window);cycle->cycle(1);
        QObject::connect(cycle,&AgentCycleController::openRequested,qApp,[&]{delete window.data();});
        QEvent blur(QEvent::WindowDeactivate);QApplication::sendEvent(window,&blur);QVERIFY(window.isNull());
    }
    void previewPropertyObserverCanCancelWithoutStaleOverlay() {
        Fixture f;auto* overlay=f.window.findChild<QWidget*>("agentCyclePreview");QVERIFY(overlay);
        PropertyObserver observer;observer.action=[&]{f.cycle->cancel();};overlay->installEventFilter(&observer);
        f.tab();QVERIFY(observer.fired);QVERIFY(!f.cycle->active());QVERIFY(!overlay->isVisible());
        QCOMPARE(overlay->accessibleName(),QString());
    }
    void previewPropertyObserverCanDestroyController() {
        Fixture f;QPointer<QWidget> overlay=f.window.findChild<QWidget*>("agentCyclePreview");QVERIFY(overlay);
        QPointer<AgentCycleController> cycle(f.cycle);PropertyObserver observer;
        observer.action=[&]{delete cycle.data();};overlay->installEventFilter(&observer);
        f.tab();QVERIFY(observer.fired);QVERIFY(!cycle);QVERIFY(!overlay);
    }
    void previewScrollsRowWithoutMovingSelection() {
        Fixture f; QStringList ids; for(int i=0;i<50;++i)ids.append(QString::number(i)); f.rows(ids);
        f.cycle->observeCurrent("49"); f.cycle->observeCurrent("0"); const auto* selected=f.list->currentItem();
        f.tab(); QCOMPARE(f.cycle->previewId(), QString("49")); QVERIFY(f.list->verticalScrollBar()->value()>0); QCOMPARE(f.list->currentItem(), selected);
    }
    void renderLightDarkNarrow_data() {
        QTest::addColumn<bool>("dark"); QTest::newRow("light") << false; QTest::newRow("dark") << true;
    }
    void renderLightDarkNarrow() {
        QFETCH(bool, dark); Fixture f; QPalette palette;
        palette.setColor(QPalette::Window,dark?QColor("#161820"):QColor("#f6f8fc"));
        palette.setColor(QPalette::Base,dark?QColor("#20232c"):QColor("#ffffff"));
        palette.setColor(QPalette::Text,dark?QColor("#e3e7ef"):QColor("#222833"));
        palette.setColor(QPalette::WindowText,palette.color(QPalette::Text)); f.window.setPalette(palette);
        f.window.resize(550,460); f.tab(); QApplication::processEvents();
        auto* overlay=f.window.findChild<QWidget*>("agentCyclePreview"); QVERIFY(overlay&&overlay->isVisible());
        QCOMPARE(overlay->geometry(),f.list->viewport()->rect()); QCOMPARE(overlay->focusPolicy(),Qt::NoFocus);
        QVERIFY(overlay->testAttribute(Qt::WA_TransparentForMouseEvents));
        const auto folder=qEnvironmentVariable("ADE_CYCLE_CAPTURE_DIR"); if(!folder.isEmpty())QVERIFY(f.window.grab().save(folder+QString("/cycle-%1.png").arg(dark?"dark":"light")));
    }
};
int main(int argc,char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen"); QApplication app(argc,argv); AppFonts::load(); QFont font("Geist"); font.setPixelSize(13); app.setFont(font);
    AgentCycleTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_agent_cycle.moc"
