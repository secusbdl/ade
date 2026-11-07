#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/MainWindow.h"
#include "ui/ModelSelection.h"
#include "ui/ModelThinking.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/WorkbenchSplitter.h"
#include "terminal/TerminalWidget.h"
#include "core/ProviderStore.h"
#include "core/AgentEngine.h"
#include "browser/browserwidget.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QLineEdit>
#include <QListWidget>
#include <QShortcut>
#include <QTabWidget>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
#include <algorithm>
using namespace ade;
namespace {
bool seed(const QString& directory, const QString& workspace = {}) {
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
        chat.workspace = workspace;
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
// Synthetic external-harness provider. The executable path does not exist and
// is never started: cycling only writes a preference.
ProviderConfig harnessProvider(const QString& directory) {
    ProviderConfig provider;
    provider.id = "harness";
    provider.name = "Harness";
    provider.model = "fixture-agent";
    provider.kind = "codex";
    provider.protocol = "codex-app-server";
    provider.harnessExecutable = QDir(directory).filePath("missing/fixture-agent.exe");
    return provider;
}
QJsonObject harnessCatalogs(const ProviderConfig& provider) {
    const QJsonObject model{{"id", "fixture-agent"}, {"name", "Fixture agent"},
                            {"reasoningEfforts", QJsonArray{"low", "medium", "high"}},
                            {"defaultReasoningEffort", "low"}, {"isDefault", true}};
    return {{provider.id, cleanModelCatalog(provider, QJsonArray{model})}};
}
int modelIndex(QComboBox* models, const QString& providerId, const QString& presetId) {
    for (int index = 0; index < models->count(); ++index) {
        const auto selection = ModelSelection::fromJson(models->itemData(index).toJsonObject());
        if (selection.providerId == providerId && selection.presetId == presetId) return index;
    }
    return -1;
}
QString storedEffort(MainWindow& window) {
    const auto overrides = window.findChild<SettingsPage*>()->preference("agent.modelThinkingOverrides").toObject();
    return overrides.value("harness").toObject().value("fixture-agent").toObject().value("value").toString();
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
    // S1. Before focus_or_toggle_terminal existed, Ctrl+` in the composer did
    // not open a terminal, so the tab count and focus assertions failed.
    void focusOrToggleTerminalCreatesTerminalWhenNone() {
        QTemporaryDir data, workspace;
        QVERIFY(seed(data.path(), QDir(workspace.path()).canonicalPath()));
        MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        const int count = tabs->count();
        key(input, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(tabs->count(), count + 1);
        auto* terminal = qobject_cast<TerminalWidget*>(tabs->currentWidget()); QVERIFY(terminal);
        QCOMPARE(terminal->objectName(), QString("workspaceTerminal"));
        QVERIFY(split->contentVisible());
        QCOMPARE(QApplication::focusWidget(), terminal);
        // The only live-process test of this slice: the shell runs in the
        // temporary workspace and is stopped explicitly here.
        terminal->stop();
        QTRY_VERIFY(!terminal->isRunning());
    }
    void focusOrToggleTerminalRevealsCollapsedContentAndFocusesActive() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        auto* first = new TerminalWidget; auto* second = new TerminalWidget;
        tabs->addTab(first, "Terminal one"); tabs->addTab(new QWidget, "Plain");
        const int target = tabs->addTab(second, "Terminal two");
        tabs->setCurrentIndex(target); split->setContentCollapsed(true); QVERIFY(!split->contentVisible());
        const int count = tabs->count();
        key(input, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QVERIFY(split->contentVisible());
        QCOMPARE(tabs->currentIndex(), target); QCOMPARE(tabs->count(), count);
        QCOMPARE(QApplication::focusWidget(), second);
    }
    void focusOrToggleTerminalCollapsesWhenFocusedAndCyclesFromOtherTab() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        auto* first = new TerminalWidget; auto* second = new TerminalWidget;
        const int a = tabs->addTab(first, "Terminal one");
        const int plain = tabs->addTab(new QWidget, "Plain");
        const int b = tabs->addTab(second, "Terminal two");
        const int last = tabs->addTab(new QWidget, "Last");
        tabs->setCurrentIndex(a); split->setContentCollapsed(false); QCoreApplication::processEvents();
        key(first, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QVERIFY(split->contentCollapsed());
        split->setContentCollapsed(false); tabs->setCurrentIndex(plain);
        key(input, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(tabs->currentIndex(), b); QCOMPARE(QApplication::focusWidget(), second);
        tabs->setCurrentIndex(last);
        key(input, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(tabs->currentIndex(), a); QCOMPARE(QApplication::focusWidget(), first);
    }
    void terminalYieldsCtrlBackquoteButKeepsCtrlI() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && split);
        auto* terminal = new TerminalWidget;
        tabs->setCurrentIndex(tabs->addTab(terminal, "Terminal fixture")); split->setContentCollapsed(false);
        auto* selector = window.findChild<QShortcut*>("shortcut_toggle_context_selector");
        auto* toggle = window.findChild<QShortcut*>("shortcut_focus_or_toggle_terminal"); QVERIFY(selector && toggle);
        QVERIFY(!selector->property("adeDominantNavigation").toBool());
        QVERIFY(toggle->property("adeDominantNavigation").toBool());
        QSignalSpy selected(selector, &QShortcut::activated), toggled(toggle, &QShortcut::activated);
        key(terminal, Qt::Key_I, Qt::ControlModifier);
        QCOMPARE(selected.size(), 0); QCOMPARE(QApplication::focusWidget(), terminal);
        key(terminal, Qt::Key_QuoteLeft, Qt::ControlModifier);
        QCOMPARE(toggled.size(), 1);
    }
    // Negative cases run first: the positive click starts element selection,
    // which disables the button while the inspector is busy.
    void contextSelectorClicksSelectButtonOnlyForVisibleOwnBrowserTab() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        // A web page, not the internal about:blank new tab (see the next test).
        QVERIFY(window.findChild<SettingsPage*>()->setPreference("browsing.newTabUrl", "https://example.test/page"));
        window.findChild<QToolButton*>("openBrowserButton")->click();
        auto* page = tabs->currentWidget(); QVERIFY(page);
        auto* select = page->findChild<QToolButton*>("selectBrowserElement");
        auto* browser = page->findChild<BrowserWidget*>(); QVERIFY(select && browser);
        QSignalSpy clicked(select, &QToolButton::clicked);
        const auto press = [&] { select->setEnabled(true); key(input, Qt::Key_I, Qt::ControlModifier); };
        split->setContentCollapsed(true); press(); QCOMPARE(clicked.size(), 0);
        split->setContentCollapsed(false);
        browser->setProperty("adeOwnerAgentId", QString("other")); press(); QCOMPARE(clicked.size(), 0);
        browser->setProperty("adeOwnerAgentId", QString());
        browser->setProperty("miniAppBrowser", true); press(); QCOMPARE(clicked.size(), 0);
        browser->setProperty("miniAppBrowser", false);
        select->setEnabled(false); key(input, Qt::Key_I, Qt::ControlModifier); QCOMPARE(clicked.size(), 0);
        press();
        QCOMPARE(clicked.size(), 1);
        QCOMPARE(QApplication::focusWidget(), input);
    }
    // Upstream skips stagewise://internal/ pages; the native equivalent is the
    // about:blank new tab. Before, Ctrl+I started element selection there.
    void contextSelectorIgnoresInternalNewTabPage() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>(); QVERIFY(tabs && input && split);
        window.findChild<QToolButton*>("openBrowserButton")->click();
        auto* page = tabs->currentWidget(); QVERIFY(page);
        QCOMPARE(page->property("browserSessionUrl").toUrl(), QUrl("about:blank"));
        auto* select = page->findChild<QToolButton*>("selectBrowserElement"); QVERIFY(select);
        QSignalSpy clicked(select, &QToolButton::clicked);
        split->setContentCollapsed(false);
        select->setEnabled(true); key(input, Qt::Key_I, Qt::ControlModifier);
        QCOMPARE(clicked.size(), 0);
    }
    void contextSelectorIgnoresNonBrowserTabs() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput"); QVERIFY(tabs && input);
        window.findChild<QToolButton*>("openBrowserButton")->click();
        auto* select = tabs->currentWidget()->findChild<QToolButton*>("selectBrowserElement"); QVERIFY(select);
        QSignalSpy clicked(select, &QToolButton::clicked);
        tabs->setCurrentIndex(tabs->addTab(new QWidget, "Plain"));
        select->setEnabled(true); key(input, Qt::Key_I, Qt::ControlModifier);
        QCOMPARE(clicked.size(), 0);
    }
    // Each action has a positive control after the guards, so a removed
    // enabled() check would change the overrides, tabs, selection or engine.
    void newShortcutsAreSuppressedInSettingsAndModalDialogs() {
        QTemporaryDir data, workspace; QString error;
        QVERIFY(seed(data.path(), QDir(workspace.path()).canonicalPath()));
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        const auto harness = harnessProvider(data.path());
        const ProviderConfig api{"fixture", "Fixture", QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
                                 "synthetic-test-only", "fixture-model", "openai"};
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"), {api, harness}, &error), qPrintable(error));
        {
            SettingsPage page(data.path());
            QVERIFY(page.setPreference("modelCatalogs", harnessCatalogs(harness)));
            QVERIFY(page.setPreference("browsing.newTabUrl", "https://example.test/page"));
        }
        MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* models = window.findChild<QComboBox*>("modelSelector"); QVERIFY(tabs && models);
        auto* settings = window.findChild<SettingsPage*>(); QVERIFY(settings);
        window.findChild<QToolButton*>("openBrowserButton")->click();
        auto* select = tabs->currentWidget()->findChild<QToolButton*>("selectBrowserElement"); QVERIFY(select);
        QSignalSpy clicked(select, &QToolButton::clicked);
        // The fixture server accepts the turn and never answers, so it stays busy.
        const int http = modelIndex(models, "fixture", {}), direct = modelIndex(models, "harness", {});
        QVERIFY(http >= 0 && direct >= 0);
        models->setCurrentIndex(http);
        window.submitPrompt("Keep running");
        QTRY_VERIFY(window.findChild<AgentEngine*>() && window.findChild<AgentEngine*>()->isBusy());
        auto* engine = window.findChild<AgentEngine*>();
        // Cycling reads the selector, so a discovered harness model makes it write.
        models->setCurrentIndex(direct);
        const int count = tabs->count(), currentTab = tabs->currentIndex();
        QMap<QString, QShortcut*> shortcuts;
        for (const auto* name : {"shortcut_focus_or_toggle_terminal", "shortcut_toggle_context_selector",
                                 "shortcut_cycle_model_thinking_effort", "shortcut_stop_agent"}) {
            auto* shortcut = window.findChild<QShortcut*>(name); QVERIFY2(shortcut, name); shortcuts.insert(name, shortcut);
        }
        QTRY_VERIFY(shortcuts["shortcut_stop_agent"]->isEnabled());
        const auto fire = [&] {
            for (auto* shortcut : shortcuts) { select->setEnabled(true); emit shortcut->activated(); }
            QCoreApplication::processEvents();
        };
        const auto unchanged = [&] {
            QCOMPARE(tabs->count(), count); QCOMPARE(tabs->currentIndex(), currentTab); QCOMPARE(clicked.size(), 0);
            QVERIFY(settings->preference("agent.modelThinkingOverrides").toObject().isEmpty());
            QVERIFY(engine->isBusy());
        };
        window.findChild<QToolButton*>("settingsButton")->click();
        QVERIFY(settings->isVisible());
        QVERIFY(!shortcuts["shortcut_stop_agent"]->isEnabled());
        fire(); unchanged();
        settings->findChild<QPushButton*>("settingsBack")->click();
        QVERIFY(!settings->isVisible());
        QTRY_VERIFY(shortcuts["shortcut_stop_agent"]->isEnabled());
        {
            QDialog dialog(&window); dialog.setModal(true); dialog.show(); QCoreApplication::processEvents();
            fire(); unchanged();
            dialog.close(); QCoreApplication::processEvents();
        }
        // Positive controls: the same activations act once nothing suppresses them.
        QCOMPARE(models->currentIndex(), direct);
        emit shortcuts["shortcut_cycle_model_thinking_effort"]->activated();
        QCOMPARE(storedEffort(window), QString("medium"));
        select->setEnabled(true); emit shortcuts["shortcut_toggle_context_selector"]->activated();
        QCOMPARE(clicked.size(), 1);
        emit shortcuts["shortcut_stop_agent"]->activated();
        QTRY_VERIFY(!engine->isBusy());
    }
    // S2. Before the rebind, Ctrl+Shift+G opened the Source control tab.
    void fileTreeDiffShortcutSelectsChangedFilesMode() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>();
        auto* changes = window.findChild<QToolButton*>("fileTreeChangesMode"); QVERIFY(tabs && input && split && changes);
        QVERIFY(!changes->isChecked());
        const int count = tabs->count(); const auto draft = input->toPlainText();
        key(input, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(split->fileTreeVisible()); QVERIFY(changes->isChecked());
        QCOMPARE(tabs->count(), count); QCOMPARE(input->toPlainText(), draft);
        auto* git = window.findChild<QWidget*>("gitPanel");
        QVERIFY(!git || tabs->indexOf(git) < 0);
        QVERIFY(!window.findChild<QToolButton*>("sourceControlButton")->toolTip().contains("Ctrl+Shift+G"));
    }
    // Before, Ctrl+Shift+E only showed the tree and left changed-files mode on.
    void fileTreeFilesShortcutSelectsFilesMode() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* split = window.findChild<WorkbenchSplitter*>();
        auto* files = window.findChild<QToolButton*>("fileTreeFilesMode");
        auto* changes = window.findChild<QToolButton*>("fileTreeChangesMode"); QVERIFY(input && split && files && changes);
        changes->click(); QVERIFY(changes->isChecked());
        split->setFileTreeVisible(false);
        key(input, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(split->fileTreeVisible()); QVERIFY(files->isChecked()); QVERIFY(!changes->isChecked());
    }
    // Before, Ctrl+Shift+E was not dominant, so the terminal swallowed it.
    void fileTreeFilesShortcutWorksFromTerminal() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* split = window.findChild<WorkbenchSplitter*>();
        auto* files = window.findChild<QToolButton*>("fileTreeFilesMode");
        auto* changes = window.findChild<QToolButton*>("fileTreeChangesMode"); QVERIFY(tabs && split && files && changes);
        changes->click(); QVERIFY(changes->isChecked());
        auto* terminal = new TerminalWidget;
        tabs->setCurrentIndex(tabs->addTab(terminal, "Terminal fixture")); split->setContentCollapsed(false);
        key(terminal, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(files->isChecked()); QVERIFY(split->fileTreeVisible());
    }
    void fileTreeDiffShortcutDoesNotStealFromTerminal() {
        QTemporaryDir data; QVERIFY(seed(data.path())); MainWindow window(data.path()); activate(window);
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* split = window.findChild<WorkbenchSplitter*>();
        auto* files = window.findChild<QToolButton*>("fileTreeFilesMode"); QVERIFY(tabs && split && files);
        files->click();
        auto* terminal = new TerminalWidget;
        tabs->setCurrentIndex(tabs->addTab(terminal, "Terminal fixture")); split->setContentCollapsed(false);
        key(terminal, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(files->isChecked()); QCOMPARE(QApplication::focusWidget(), terminal);
    }
    // S7. Before cycle_model_thinking_effort existed, Ctrl+Alt+/ wrote nothing.
    void cycleThinkingEffortPersistsOverrideWithoutSending() {
        QTemporaryDir data; QVERIFY(seed(data.path())); QString error;
        const auto harness = harnessProvider(data.path());
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"), {harness}, &error), qPrintable(error));
        { SettingsPage page(data.path()); QVERIFY(page.setPreference("modelCatalogs", harnessCatalogs(harness))); }
        MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* models = window.findChild<QComboBox*>("modelSelector"); QVERIFY(input && models);
        const int index = modelIndex(models, "harness", {}); QVERIFY(index >= 0);
        models->setCurrentIndex(index);
        const auto draft = input->toPlainText();
        key(input, Qt::Key_Slash, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(storedEffort(window), QString("medium"));
        QVERIFY2(models->currentText().endsWith(QString(" · medium")), qPrintable(models->currentText()));
        QCOMPARE(window.findChild<QLabel*>("sessionStatus")->text(), QString("Reasoning effort: medium"));
        QVERIFY(window.findChildren<AgentEngine*>().isEmpty());
        QCOMPARE(input->toPlainText(), draft);
        key(input, Qt::Key_Slash, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(storedEffort(window), QString("high"));
        key(input, Qt::Key_Slash, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(storedEffort(window), QString("low"));
        QVERIFY(models->currentText().endsWith(QString(" · low")));
        SettingsPage reopened(data.path());
        const auto stored = validThinkingOverrides(reopened.preference("agent.modelThinkingOverrides"));
        QCOMPARE(stored.value("harness").toObject().value("fixture-agent").toObject().value("value").toString(), QString("low"));
        const auto chain = selectedModelChain(ModelSelection::fromJson(models->currentData().toJsonObject()), {harness}, {},
                                              harnessCatalogs(harness), stored);
        QCOMPARE(chain.size(), 1); QCOMPARE(chain.first().provider.reasoningEffort, QString("low"));
        QVERIFY(window.findChildren<AgentEngine*>().isEmpty());
    }
    void cycleThinkingEffortNoOpForApiProviderAndPreset() {
        QTemporaryDir data; QVERIFY(seed(data.path())); QString error;
        const auto harness = harnessProvider(data.path());
        const ProviderConfig api{"fixture", "Fixture", "http://127.0.0.1:1/v1", "synthetic-test-only", "fixture-model", "openai"};
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"), {api, harness}, &error), qPrintable(error));
        {
            SettingsPage page(data.path());
            QVERIFY(page.setPreference("modelCatalogs", harnessCatalogs(harness)));
            QVERIFY(page.setPreference("modelPresets", QJsonArray{QJsonObject{
                {"id", "preset-fixture"}, {"name", "Fixture preset"},
                {"providerInstanceId", "harness"}, {"modelId", "fixture-agent"}}}));
        }
        MainWindow window(data.path()); activate(window);
        auto* input = window.findChild<Composer*>("composerInput");
        auto* models = window.findChild<QComboBox*>("modelSelector");
        auto* status = window.findChild<QLabel*>("sessionStatus"); QVERIFY(input && models && status);
        const int preset = modelIndex(models, "harness", "preset-fixture");
        const int http = modelIndex(models, "fixture", {});
        const int direct = modelIndex(models, "harness", {}); QVERIFY(preset >= 0 && http >= 0 && direct >= 0);
        status->clear();
        for (const int index : {preset, http}) {
            models->setCurrentIndex(index);
            key(input, Qt::Key_Slash, Qt::ControlModifier | Qt::AltModifier);
            QVERIFY(window.findChild<SettingsPage*>()->preference("agent.modelThinkingOverrides").toObject().isEmpty());
            QCOMPARE(status->text(), QString());
        }
        // Positive control: the same key on the discovered harness model writes.
        models->setCurrentIndex(direct);
        key(input, Qt::Key_Slash, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(storedEffort(window), QString("medium"));
        QVERIFY(window.findChildren<AgentEngine*>().isEmpty());
    }
};
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication app(argc, argv); initializeAppStyle(app);
    NavigationShortcutsTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_navigation_shortcuts.moc"
