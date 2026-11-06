#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/WorkbenchSplitter.h"
#include "terminal/TerminalWidget.h"
#include "core/ProviderStore.h"
#include "core/AgentEngine.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QShortcut>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <algorithm>
using namespace ade;
namespace {
bool seed(const QString& directory) {
    QList<Conversation> chats;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int index = 0; index < 5; ++index) {
        Conversation chat;
        chat.id = QString("agent%1").arg(index);
        chat.title = QString("Agent %1").arg(index);
        chat.updated = now + index;
        chat.created = now - 1000 + index;
        chat.lastMessageAt = now - 1000 + index;
        chat.draft = QString("Draft %1").arg(index);
        chats.append(chat);
    }
    Conversation archived;
    archived.id = "archived"; archived.title = "Archived"; archived.archived = true;
    chats.append(archived);
    return SessionStore(directory).save(chats);
}
void activate(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1540, 940);
    window.show();
    window.activateWindow();
    QApplication::setActiveWindow(&window);
    QCoreApplication::processEvents();
}
void key(QWidget* target, Qt::Key code, Qt::KeyboardModifiers modifiers) {
    target->setFocus();
    QTest::keyClick(target, code, modifiers);
    QCoreApplication::processEvents();
}
}
class NavigationShortcutsTest : public QObject {
    Q_OBJECT
private slots:
    void focusChatDoesNotAlterDraft() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* search = window.findChild<QLineEdit*>("agentSearch");
        QVERIFY(input && search);
        const auto draft = input->toPlainText();
        key(search, Qt::Key_L, Qt::ControlModifier);
        QCOMPARE(QApplication::focusWidget(), input);
        QCOMPARE(input->toPlainText(), draft);
        QCOMPARE(search->text(), QString());
    }
    void numericAgentNavigationFollowsSidebarAndKeepsDrafts() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput"); QVERIFY(input);
        key(input, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(window.currentSessionId(), QString("agent4"));
        input->setPlainText("Unsaved draft kept while switching");
        key(input, Qt::Key_2, Qt::ControlModifier);
        QCOMPARE(window.currentSessionId(), QString("agent3"));
        key(input, Qt::Key_9, Qt::ControlModifier);
        QCOMPARE(window.currentSessionId(), QString("agent0"));
        key(input, Qt::Key_1, Qt::ControlModifier);
        QCOMPARE(window.currentSessionId(), QString("agent4"));
        QCOMPARE(input->toPlainText(), QString("Unsaved draft kept while switching"));
        QString error; const auto saved = SessionStore(data.path()).load(&error); QVERIFY2(error.isEmpty(), qPrintable(error));
        auto it = std::find_if(saved.begin(), saved.end(), [](const auto& chat) { return chat.id == "agent4"; });
        QVERIFY(it != saved.end()); QCOMPARE(it->draft, input->toPlainText());
    }
    void modelShortcutOpensPickerWithoutSending_data() {
        QTest::addColumn<int>("modifiers");
        QTest::newRow("primary") << int(Qt::ControlModifier);
        QTest::newRow("shift-alias") << int(Qt::ControlModifier | Qt::ShiftModifier);
    }
    void modelShortcutOpensPickerWithoutSending() {
        QFETCH(int, modifiers);
        QTemporaryDir data; QVERIFY(seed(data.path())); QString error;
        const ProviderConfig provider{"fixture", "Fixture", "http://127.0.0.1:1/v1", "synthetic-test-only", "fixture-model", "openai"};
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"), {provider}, &error), qPrintable(error));
        MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* models = window.findChild<QComboBox*>("modelSelector"); QVERIFY(input && models && models->count());
        const auto draft = input->toPlainText(); const auto selected = models->currentIndex();
        key(input, Qt::Key_Slash, Qt::KeyboardModifiers(modifiers));
        QTRY_VERIFY(QApplication::activePopupWidget());
        models->hidePopup();
        QCOMPARE(models->currentIndex(), selected); QCOMPARE(input->toPlainText(), draft);
        QVERIFY(window.findChildren<AgentEngine*>().isEmpty());
    }
    void filteredAgentListAndAbsentIndicesDoNotSelectHiddenChats() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* search = window.findChild<QLineEdit*>("agentSearch"); QVERIFY(input && search);
        search->setText("Agent 2");
        key(input, Qt::Key_9, Qt::ControlModifier); QCOMPARE(window.currentSessionId(), QString("agent2"));
        key(input, Qt::Key_2, Qt::ControlModifier); QCOMPARE(window.currentSessionId(), QString("agent2"));
        search->setText("does not exist");
        key(input, Qt::Key_PageDown, Qt::ControlModifier); QCOMPARE(window.currentSessionId(), QString("agent2"));
    }
    void tabCyclingWrapsAndNineSelectsNinthNotLast() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        if (!tabs) tabs = window.findChild<QTabWidget*>();
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        for (int i = 0; i < 10; ++i) tabs->addTab(new QWidget, QString("Tab %1").arg(i + 1));
        tabs->setCurrentIndex(9); split->setContentCollapsed(true);
        key(input, Qt::Key_PageDown, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(tabs->currentIndex(), 0); QVERIFY(split->contentVisible());
        key(input, Qt::Key_PageUp, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(tabs->currentIndex(), 9);
        key(input, Qt::Key_9, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(tabs->currentIndex(), 8);
        key(input, Qt::Key_1, Qt::ControlModifier | Qt::AltModifier); QCOMPARE(tabs->currentIndex(), 0);
    }
    void addressAliasesFocusCurrentBrowserOnly_data() {
        QTest::addColumn<int>("code"); QTest::addColumn<int>("modifiers");
        QTest::newRow("alt-d") << int(Qt::Key_D) << int(Qt::AltModifier);
        QTest::newRow("f6") << int(Qt::Key_F6) << int(Qt::NoModifier);
        QTest::newRow("ctrl-alt-l") << int(Qt::Key_L) << int(Qt::ControlModifier | Qt::AltModifier);
    }
    void addressAliasesFocusCurrentBrowserOnly() {
        QFETCH(int, code); QFETCH(int, modifiers);
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput"); QVERIFY(input);
        window.findChild<QToolButton*>("openBrowserButton")->click();
        auto* address = window.findChild<QLineEdit*>("browserAddress"); QVERIFY(address);
        address->setText("https://example.test/path");
        key(input, Qt::Key(code), Qt::KeyboardModifiers(modifiers));
        QCOMPARE(QApplication::focusWidget(), address); QCOMPARE(address->selectedText(), address->text());
    }
    void mainNavigationIsSuppressedInSettingsAndModalDialogs() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        const auto current = window.currentSessionId();
        window.findChild<QToolButton*>("settingsButton")->click();
        auto* settings = window.findChild<SettingsPage*>(); QVERIFY(settings);
        key(settings, Qt::Key_9, Qt::ControlModifier); QCOMPARE(window.currentSessionId(), current);
        auto* shortcut = window.findChild<QShortcut*>("shortcut_focus_agent_last"); QVERIFY(shortcut);
        emit shortcut->activated(); QCOMPARE(window.currentSessionId(), current);
        QDialog dialog(&window); dialog.setModal(true); dialog.show(); QCoreApplication::processEvents();
        emit shortcut->activated(); QCOMPARE(window.currentSessionId(), current);
        dialog.close();
    }
    void registeredNavigationWorksFromTerminalWithoutStealingCopy() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>();
        auto* input = window.findChild<Composer*>("composerInput"); QVERIFY(tabs && input);
        auto* terminal = new TerminalWidget;
        tabs->setCurrentIndex(tabs->addTab(terminal, "Terminal fixture"));
        auto* split = window.findChild<WorkbenchSplitter*>(); split->setContentCollapsed(false);
        key(terminal, Qt::Key_L, Qt::ControlModifier); QCOMPARE(QApplication::focusWidget(), input);
        key(terminal, Qt::Key_1, Qt::ControlModifier); QCOMPARE(window.currentSessionId(), QString("agent4"));
        key(terminal, Qt::Key_C, Qt::ControlModifier); QCOMPARE(QApplication::focusWidget(), terminal);
    }
    void focusChatRevealsHiddenNarrowComposer() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>();
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        auto* terminal = new TerminalWidget;
        tabs->setCurrentIndex(tabs->addTab(terminal, "Narrow terminal fixture"));
        window.resize(350, 700); split->setContentCollapsed(false); QCoreApplication::processEvents();
        QVERIFY(split->exclusiveContent()); QVERIFY(!input->isVisible());
        key(terminal, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(input->isVisible()); QCOMPARE(QApplication::focusWidget(), input); QVERIFY(split->contentCollapsed());
    }
    void modelFocusObserverCanCloseWindow() {
        QTemporaryDir data; QVERIFY(seed(data.path())); QString error;
        const ProviderConfig provider{"fixture", "Fixture", "http://127.0.0.1:1/v1", "synthetic-test-only", "fixture-model", "openai"};
        QVERIFY(ProviderStore::save(data.filePath("providers.json"), {provider}, &error));
        QPointer<MainWindow> window = new MainWindow(data.path()); activate(*window);
        window->setAttribute(Qt::WA_DeleteOnClose);
        auto* input = window->findChild<Composer*>("composerInput");
        auto* models = window->findChild<QComboBox*>("modelSelector"); QVERIFY(input && models);
        input->setFocus();
        const auto observer = connect(qApp, &QApplication::focusChanged, qApp, [&window, models](QWidget*, QWidget* next) {
            // Qt owns the active focus dispatch. Closing schedules destruction;
            // deleting a widget synchronously inside QWidget::setFocus is invalid.
            if (next == models && window) window->close();
        });
        auto* shortcut = window->findChild<QShortcut*>("shortcut_open_model_select"); QVERIFY(shortcut);
        emit shortcut->activated();
        disconnect(observer);
        QVERIFY(!QApplication::activePopupWidget());
        QTRY_VERIFY(window.isNull());
    }
};
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv); initializeAppStyle(app);
    NavigationShortcutsTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_navigation_shortcuts.moc"
