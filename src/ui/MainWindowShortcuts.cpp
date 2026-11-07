#include "MainWindow.h"
#include "AgentCycleController.h"
#include "Composer.h"
#include "ModelThinking.h"
#include "SettingsPage.h"
#include "SideChatPane.h"
#include "WorkbenchSplitter.h"
#include "browser/browserwidget.h"
#include "terminal/TerminalWidget.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <functional>

namespace ade {
namespace {
// Re-evaluates the stop shortcut when a stop target changes enabled state.
// Neither MainWindow::updateComposerState nor SideChatPane::updateActions
// needs to know about the shortcut.
class EnabledChangeWatcher final : public QObject {
  public:
    EnabledChangeWatcher(std::function<void()> changed, QObject* parent)
        : QObject(parent), m_changed(std::move(changed)) {}

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::EnabledChange && m_changed)
            m_changed();
        return QObject::eventFilter(object, event);
    }

  private:
    std::function<void()> m_changed;
};
// Upstream shouldPreserveNativeCopy: a text selection keeps Ctrl+C as copy.
// Read-only text does not accept the Copy override, so copy it explicitly.
bool copyFocusedSelection(QWidget* focus) {
    for (int depth = 0; focus && depth < 2; ++depth, focus = focus->parentWidget()) {
        if (auto* text = qobject_cast<QTextEdit*>(focus)) {
            if (!text->textCursor().hasSelection())
                return false;
            text->copy();
            return true;
        }
        if (auto* plain = qobject_cast<QPlainTextEdit*>(focus)) {
            if (!plain->textCursor().hasSelection())
                return false;
            plain->copy();
            return true;
        }
        if (auto* line = qobject_cast<QLineEdit*>(focus)) {
            if (!line->hasSelectedText())
                return false;
            line->copy();
            return true;
        }
        if (auto* label = qobject_cast<QLabel*>(focus)) {
            if (!label->hasSelectedText())
                return false;
            QGuiApplication::clipboard()->setText(label->selectedText());
            return true;
        }
    }
    return false;
}
} // namespace
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
                                    std::function<void()> callback, bool dominant = true) {
        auto* shortcut = new QShortcut(this);
        shortcut->setObjectName("shortcut_" + action);
        QList<QKeySequence> sequences;
        for (const auto& key : keys) sequences.append(QKeySequence(key));
        shortcut->setKeys(sequences);
        shortcut->setContext(Qt::WindowShortcut);
        shortcut->setAutoRepeat(false);
        // TerminalWidget yields only keys marked dominant (upstream captureDominantly).
        shortcut->setProperty("adeDominantNavigation", dominant);
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

    // focus_or_toggle_terminal: upstream agent-hotkey-bindings steps, over
    // content tabs that are workspace terminals. Agent shell pages are not.
    bind("focus_or_toggle_terminal", {"Ctrl+`"}, [this] {
        const QPointer<MainWindow> self(this);
        QList<int> terminals;
        for (int index = 0; index < m_tabs->count(); ++index)
            if (qobject_cast<TerminalWidget*>(m_tabs->widget(index))) terminals.append(index);
        if (terminals.isEmpty()) {
            showTerminal();
            return;
        }
        const int current = m_tabs->currentIndex();
        const QPointer<TerminalWidget> active(qobject_cast<TerminalWidget*>(m_tabs->currentWidget()));
        int next = terminals.first();
        for (const int index : terminals)
            if (index > current) {
                next = index;
                break;
            }
        const auto focusTab = [this, self](int index) {
            const QPointer<TerminalWidget> terminal(qobject_cast<TerminalWidget*>(m_tabs->widget(index)));
            m_tabs->setCurrentIndex(index);
            if (!self) return;
            revealContent();
            if (self && terminal) terminal->setFocus(Qt::ShortcutFocusReason);
        };
        if (m_splitter->contentCollapsed() || !m_splitter->contentVisible()) {
            focusTab(active ? current : next);
            return;
        }
        if (active && active->hasFocus()) {
            const QPointer<Composer> input(m_composer);
            m_splitter->setContentCollapsed(true);
            if (self && input && input->isVisible()) input->setFocus(Qt::ShortcutFocusReason);
            return;
        }
        if (active) {
            active->setFocus(Qt::ShortcutFocusReason);
            return;
        }
        focusTab(next);
    });
    // toggle_context_selector. Deliberately not dominant: Ctrl+I is Tab inside a
    // terminal, and the action never applies while a terminal tab is current.
    // Not reachable from WebView2 content (no accelerator forwarding yet).
    bind("toggle_context_selector", {"Ctrl+I"}, [this] {
        const auto* session = current();
        if (!session || session->archived || !m_splitter->contentVisible()) return;
        auto* page = m_tabs->currentWidget();
        if (!page || page->property("browserTabId").toString().isEmpty()) return;
        auto* browser = page->findChild<BrowserWidget*>();
        if (!browser || browser->property("miniAppBrowser").toBool()) return;
        const auto owner = browser->property("adeOwnerAgentId").toString();
        if (!owner.isEmpty() && owner != m_currentId) return;
        // Upstream skips stagewise://internal/ pages. The native internal page is
        // the about:blank new tab, where selectBrowserElement stays enabled once
        // the view is ready, so exclude about: pages here. Before the view
        // reports a URL, use the tab's requested URL.
        auto url = browser->currentUrl();
        if (url.isEmpty()) url = page->property("browserSessionUrl").toUrl();
        if (url.scheme() == "about") return;
        auto* select = page->findChild<QToolButton*>("selectBrowserElement");
        if (!select || !select->isEnabled()) return;
        const QPointer<MainWindow> self(this);
        const QPointer<Composer> input(m_composer);
        select->click();
        if (self && input && input->isVisible()) input->setFocus(Qt::ShortcutFocusReason);
    }, false);
    // cycle_model_thinking_effort: upstream registers it without a busy gate;
    // the new value applies at the next setModelChain. Discovered harness
    // efforts only; presets keep their own thinking settings.
    bind("cycle_model_thinking_effort", {"Ctrl+Alt+/"}, [this] {
        const auto* session = current();
        if (!session || session->archived || !m_settings) return;
        const auto selection = ModelSelection::fromJson(m_models->currentData().toJsonObject());
        if (!selection.presetId.isEmpty() || selection.modelId.isEmpty()) return;
        ProviderConfig provider;
        if (!selection.resolve(m_providers, &provider)) return;
        const auto catalogs = m_settings->preference("modelCatalogs").toObject();
        const auto entry = thinkingCatalogEntry(provider, selection.modelId, catalogs);
        const auto options = thinkingOptions(provider, entry);
        if (options.isEmpty()) return;
        auto overrides = validThinkingOverrides(m_settings->preference("agent.modelThinkingOverrides"));
        auto effort = thinkingOverrideValue(selection, m_providers, catalogs, overrides);
        if (effort.isEmpty()) effort = provider.reasoningEffort;
        const auto next = nextThinkingOption(options, effort, entry.value("defaultReasoningEffort").toString());
        auto models = overrides.value(selection.providerId).toObject();
        models.insert(selection.modelId, QJsonObject{{"enabled", true}, {"provider", provider.kind}, {"value", next}});
        overrides.insert(selection.providerId, models);
        if (validThinkingOverrides(overrides) != overrides) {
            m_status->setText("Reasoning effort could not be saved for this model.");
            return;
        }
        // Write the whole object once; instance and model ids may contain dots.
        const QPointer<MainWindow> self(this);
        if (!m_settings->setPreference("agent.modelThinkingOverrides", overrides)) {
            if (!self) return;
            const auto error = m_settings->storageError();
            m_status->setText(error.isEmpty() ? QString("Reasoning effort could not be saved.") : error);
            return;
        }
        if (!self) return;
        // preferencesChanged normally rebuilds the labels; keep the visible one exact.
        const auto suffix = QString(" · ") + next;
        const int index = m_models->currentIndex();
        if (index >= 0 && !m_models->itemText(index).endsWith(suffix)
            && ModelSelection::fromJson(m_models->itemData(index).toJsonObject()).key() == selection.key())
            m_models->setItemText(index, selection.label + suffix);
        m_status->setText("Reasoning effort: " + next);
    });

    // stop_agent fallback. Composers handle Ctrl+C themselves. This window
    // shortcut is enabled only while the active chat surface can stop, so
    // copy in item views and read-only text is untouched while idle.
    // ~MainWindow deletes it (and its watcher and hook connections) before
    // the widgets its callbacks read are destroyed.
    auto* stop = new QShortcut(QKeySequence("Ctrl+C"), m_pages->widget(0));
    stop->setObjectName("shortcut_stop_agent");
    stop->setContext(Qt::WindowShortcut);
    stop->setAutoRepeat(false);
    // Upstream isActiveChatSurface: a visible side-chat tab is the only active
    // surface, so the main STOP_AGENT listener is off while it is current.
    const auto stopTarget = [this]() -> QToolButton* {
        if (m_splitter->contentVisible())
            if (auto* pane = qobject_cast<SideChatPane*>(m_tabs->currentWidget()))
                return pane->findChild<QToolButton*>("sideChatStopButton");
        return m_stop;
    };
    const QPointer<QShortcut> stopGuard(stop);
    const auto refreshStop = [stopGuard, stopTarget, enabled] {
        if (!stopGuard) return;
        const auto* target = stopTarget();
        stopGuard->setEnabled(enabled() && target && target->isEnabled());
    };
    auto* watcher = new EnabledChangeWatcher(refreshStop, stop);
    m_stop->installEventFilter(watcher);
    const auto watchStopTarget = [this, watcher, refreshStop] {
        if (auto* pane = qobject_cast<SideChatPane*>(m_tabs->currentWidget()))
            if (auto* button = pane->findChild<QToolButton*>("sideChatStopButton"))
                button->installEventFilter(watcher); // Reinstalling does not duplicate.
        refreshStop();
    };
    // Context `stop`: deleting the shortcut disconnects every hook. Modal
    // dialogs open and close through focus changes; popups take the keys
    // while open, and activation re-checks enabled() regardless.
    connect(m_tabs, &QTabWidget::currentChanged, stop, watchStopTarget);
    connect(m_splitter, &WorkbenchSplitter::visibilityChanged, stop, refreshStop);
    connect(m_pages, &QStackedWidget::currentChanged, stop, refreshStop);
    connect(qApp, &QApplication::focusChanged, stop, refreshStop);
    connect(stop, &QShortcut::activated, stop, [enabled, stopTarget] {
        if (!enabled()) return;
        if (copyFocusedSelection(QApplication::focusWidget())) return;
        if (auto* target = stopTarget(); target && target->isEnabled()) target->click();
    });
    // S3a: the main composer's Ctrl+C stops the active chat surface too, so a
    // current side-chat tab is stopped instead of the main agent.
    connect(m_composer, &Composer::stopShortcutRequested, stop, [stopTarget] {
        if (auto* target = stopTarget()) target->click(); // Inert while disabled.
    });
    watchStopTarget();
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
