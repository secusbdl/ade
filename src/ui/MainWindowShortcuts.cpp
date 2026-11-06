#include "MainWindow.h"
#include "AgentCycleController.h"
#include "Composer.h"
#include "WorkbenchSplitter.h"
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabWidget>
#include <functional>

namespace ade {
void MainWindow::initializeNavigationShortcuts() {
    // Windows bindings from the pinned shared/hotkeys.ts. These Qt window
    // shortcuts do not assert dominant interception inside WebView2 content.
    const auto enabled = [this] {
        return m_pages->currentIndex() == 0 && !QApplication::activeModalWidget()
            && !QApplication::activePopupWidget();
    };
    m_agentCycle = new AgentCycleController(this, m_agents, this);
    m_agentCycle->setActivationGuard(enabled);
    connect(m_agentCycle, &AgentCycleController::openRequested, this, [this, enabled](const QString& id) {
        const auto* session = find(id);
        if (!enabled() || !session || session->archived || !session->sideChatParentId.isEmpty()) return;
        // selectConversation owns draft persistence and may refuse the switch.
        // refreshAgentCycle observes only the actually selected conversation.
        selectConversation(id);
    });
    if (!refreshAgentCycle()) return;
    const auto bind = [this, enabled](const QString& action, const QStringList& keys,
                                    std::function<void()> callback) {
        auto* shortcut = new QShortcut(this);
        shortcut->setObjectName("shortcut_" + action);
        QList<QKeySequence> sequences;
        for (const auto& key : keys) sequences.append(QKeySequence(key));
        shortcut->setKeys(sequences);
        shortcut->setContext(Qt::WindowShortcut);
        shortcut->setAutoRepeat(false);
        shortcut->setProperty("adeDominantNavigation", true);
        connect(shortcut, &QShortcut::activated, this, [enabled, callback] {
            if (enabled()) callback();
        });
    };
    bind("focus_chat_input", {"Ctrl+L"}, [this] {
        const QPointer<MainWindow> self(this);
        const QPointer<Composer> input(m_composer);
        if (m_splitter->exclusiveContent()) m_splitter->setContentCollapsed(true);
        if (self && input) input->setFocus(Qt::ShortcutFocusReason);
    });
    // Keep the existing Ctrl+Shift+O alias while adding the upstream binding.
    bind("open_workspace_select", {"Ctrl+O"}, [this] { chooseWorkspace(); });
    bind("open_model_select", {"Ctrl+/", "Ctrl+Shift+/"}, [this] {
        if (!m_models->isEnabled() || m_models->count() == 0) return;
        const QPointer<MainWindow> self(this);
        const QPointer<QComboBox> models(m_models);
        if (m_splitter->exclusiveContent()) m_splitter->setContentCollapsed(true);
        if (!self || !models) return;
        models->setFocus(Qt::ShortcutFocusReason);
        if (self && models && self->isVisible() && models->hasFocus()) models->showPopup();
    });
    bind("focus_url_bar", {"Alt+D", "F6", "Ctrl+Alt+L"}, [this] {
        const auto* page = m_tabs->currentWidget();
        if (!page || page->property("browserTabId").toString().isEmpty()) return;
        auto* address = page->findChild<QLineEdit*>("browserAddress");
        if (!address) return;
        const QPointer<MainWindow> self(this);
        const QPointer<QLineEdit> input(address);
        revealContent();
        if (!self || !input) return;
        input->setFocus(Qt::ShortcutFocusReason);
        if (input) input->selectAll();
    });
    const auto selectTab = [this](int index) {
        if (index < 0 || index >= m_tabs->count()) return;
        const QPointer<MainWindow> self(this);
        m_tabs->setCurrentIndex(index);
        if (self) revealContent();
    };
    bind("next_tab", {"Ctrl+Alt+PgDown"}, [this, selectTab] {
        if (m_tabs->count() > 1 && m_tabs->currentIndex() >= 0)
            selectTab((m_tabs->currentIndex() + 1) % m_tabs->count());
    });
    bind("prev_tab", {"Ctrl+Alt+PgUp"}, [this, selectTab] {
        if (m_tabs->count() > 1 && m_tabs->currentIndex() >= 0)
            selectTab((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
    });
    for (int index = 0; index < 9; ++index)
        bind(QString("focus_tab_%1").arg(index + 1), {QString("Ctrl+Alt+%1").arg(index + 1)},
             [selectTab, index] { selectTab(index); });

    // Match the rendered sidebar order, ignoring headers, load-more rows and
    // collapsed/filtered agents. Ctrl+9 selects the last visible agent; Alt+9
    // above selects the ninth tab, which is intentionally a different rule.
    const auto visibleAgents = [this] {
        QStringList ids;
        for (int row = 0; row < m_agents->count(); ++row) {
            const auto* item = m_agents->item(row);
            const auto id = item->data(Qt::UserRole).toString();
            if (!id.isEmpty() && !item->isHidden() && (item->flags() & Qt::ItemIsSelectable)
                && !ids.contains(id)) ids.append(id);
        }
        return ids;
    };
    const auto cycleAgent = [this](int direction) {
        if (m_agentCycle) m_agentCycle->stepImmediately(direction);
    };
    // The source uses the same visible MRU snapshot for both Page aliases and
    // Ctrl+Tab; Page aliases commit immediately without waiting for key release.
    bind("next_agent", {"Ctrl+PgDown"}, [cycleAgent] { cycleAgent(1); });
    bind("prev_agent", {"Ctrl+PgUp"}, [cycleAgent] { cycleAgent(-1); });
    for (int index = 0; index < 9; ++index)
        bind(index == 8 ? "focus_agent_last" : QString("focus_agent_%1").arg(index + 1),
             {QString("Ctrl+%1").arg(index + 1)}, [this, visibleAgents, index] {
                 const auto ids = visibleAgents();
                 const int target = index == 8 ? ids.size() - 1 : index;
                 if (target >= 0 && target < ids.size()) {
                     const QPointer<MainWindow> self(this);
                     if (m_agentCycle) m_agentCycle->cancel();
                     if (self) selectConversation(ids[target]);
                 }
             });
}
bool MainWindow::refreshAgentCycle() {
    if (!m_agentCycle) return true;
    QStringList ids;
    for (const auto& session : m_sessions)
        if (!session.archived && session.sideChatParentId.isEmpty()) ids.append(session.id);
    const QPointer<MainWindow> self(this);
    m_agentCycle->setAvailableIds(ids);
    if (!self) return false;
    m_agentCycle->observeCurrent(m_currentId);
    return !self.isNull();
}
} // namespace ade
