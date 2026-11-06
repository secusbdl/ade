#include "MainWindow.h"
#include "BrowserSessionStore.h"
#include "SettingsPage.h"
#include "WorkbenchSplitter.h"
#include <QLabel>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

namespace ade {
void MainWindow::initializeBrowserSession() {
    m_browserSessionStore = std::make_unique<BrowserSessionStore>(m_store.directory());
    QString error;
    const auto saved = m_browserSessionStore->load(&error);
    m_browserSessionTimer = new QTimer(this);
    m_browserSessionTimer->setObjectName("browserSessionSaveTimer");
    m_browserSessionTimer->setSingleShot(true);
    m_browserSessionTimer->setInterval(200);
    connect(m_browserSessionTimer, &QTimer::timeout, this, [this] { saveBrowserSession(); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (const auto* page = m_tabs->widget(index)) {
            const auto id = page->property("browserSessionId").toString();
            if (!id.isEmpty()) m_lastActiveBrowserId = id;
        }
        scheduleBrowserSessionSave();
    });
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, [this] { scheduleBrowserSessionSave(); });
    const QPointer<MainWindow> alive(this);
    const bool collapsed = m_splitter->contentCollapsed();
    m_restoringBrowserSession = true;
    for (const auto& tab : saved.tabs) {
        addBrowserTab(tab.url, tab.id, tab.title);
        if (!alive) return;
    }
    if (!saved.tabs.isEmpty()) {
        m_lastActiveBrowserId = saved.activeTabId;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (m_tabs->widget(i)->property("browserSessionId").toString() == saved.activeTabId) {
                m_tabs->setCurrentIndex(i);
                if (!alive) return;
                break;
            }
        }
        // Creating a tab reveals content interactively. Restoration retains the
        // separately persisted panel collapse preference instead.
        m_splitter->setContentCollapsed(collapsed);
        if (!alive) return;
    } else if (error.isEmpty() && !m_browserSessionStore->existedAtLoad()) {
        // Existing native profiles had no browser inventory. Respect their
        // configured startup page once; an explicitly empty saved list stays empty.
        const auto startup = m_settings->preference("browsing.startupUrl").toString();
        const auto url = BrowserSessionStore::restorableUrl(QUrl(startup, QUrl::StrictMode));
        if (!url.isEmpty()) addBrowserTab(url);
        if (!alive) return;
    }
    m_restoringBrowserSession = false;
    m_browserSessionInitialized = true;
    if (!error.isEmpty()) m_status->setText(error);
    else if (!m_browserSessionStore->existedAtLoad()) scheduleBrowserSessionSave();
}
void MainWindow::scheduleBrowserSessionSave() {
    if (m_browserSessionInitialized && !m_restoringBrowserSession && m_browserSessionTimer)
        m_browserSessionTimer->start();
}
bool MainWindow::saveBrowserSession() {
    if (!m_browserSessionInitialized || m_restoringBrowserSession || !m_browserSessionStore) return true;
    if (m_browserSessionTimer) m_browserSessionTimer->stop();
    BrowserSessionState state;
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* page = m_tabs->widget(i);
        const auto id = page->property("browserSessionId").toString();
        const auto url = BrowserSessionStore::restorableUrl(page->property("browserSessionUrl").toUrl());
        if (id.isEmpty() || url.isEmpty()) continue;
        state.tabs.append({id, url, page->property("browserSessionTitle").toString().left(1024)});
        if (id == m_lastActiveBrowserId) state.activeTabId = id;
    }
    if (state.activeTabId.isEmpty() && !state.tabs.isEmpty()) state.activeTabId = state.tabs.first().id;
    QString error;
    if (!m_browserSessionStore->save(state, &error)) {
        m_status->setText(error);
        return false;
    }
    return true;
}
}
