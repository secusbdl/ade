#include "ChatSurface.h"
#include "Icons.h"
#include "MainWindow.h"
#include "WorkbenchSplitter.h"
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>

namespace ade {
namespace {
QToolButton* control(const QString& name, const QString& image, const QString& label, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setIcon(icon(image));
    button->setAutoRaise(true);
    button->setFixedSize(26, 26);
    button->setToolTip(label);
    button->setAccessibleName(label);
    return button;
}
} // namespace
void MainWindow::initializeWorkbench() {
    auto* browser = control("openBrowserButton", "globe", "Open new browser tab (Ctrl+T)", m_chat);
    auto* terminal = control("openTerminalButton", "terminal", "Open new terminal tab (Ctrl+Alt+T)", m_chat);
    auto* show = control("showContentPanelButton", "sidebar", "Show content panel (Ctrl+Alt+B)", m_chat);
    auto* files = control("toggleFileTreeButton", "folder", "Show file tree (Ctrl+Shift+B)", m_chat);
    auto* more = control("workbenchMenuButton", "settings", "More workbench actions", m_chat);
    for (auto* button : {browser, terminal, show, files, more})
        m_chat->headerLayout()->addWidget(button);
    connect(browser, &QToolButton::clicked, this, [this] { addBrowserTab(); });
    connect(terminal, &QToolButton::clicked, this, &MainWindow::showTerminal);
    connect(show, &QToolButton::clicked, m_splitter, &WorkbenchSplitter::toggleContent);
    connect(files, &QToolButton::clicked, m_splitter, &WorkbenchSplitter::toggleFileTree);
    auto* menu = new QMenu(more);
    menu->addAction(icon("download"), "Downloads", this, &MainWindow::showDownloads);
    menu->addAction(icon("branch"), "Source control", this, &MainWindow::showGit);
    menu->addAction(icon("document"), "File changes and undo", this, &MainWindow::showFileChanges);
    more->setMenu(menu);
    more->setPopupMode(QToolButton::InstantPopup);
    auto* hide = control("hideContentPanelButton", "sidebar", "Hide content panel (Ctrl+Alt+B)",
                         m_tabs->cornerWidget());
    m_tabs->cornerWidget()->layout()->addWidget(hide);
    connect(hide, &QToolButton::clicked, m_splitter, &WorkbenchSplitter::toggleContent);

    m_fileTreePanel = new QWidget;
    m_fileTreePanel->setObjectName("fileTreePanel");
    auto* layout = new QVBoxLayout(m_fileTreePanel);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* header = new QHBoxLayout;
    auto* title = new QLabel("Files");
    title->setObjectName("fileTreeTitle");
    header->addWidget(title, 1);
    auto* close = control("closeFileTreeButton", "close", "Hide file tree", m_fileTreePanel);
    header->addWidget(close);
    layout->addLayout(header);
    connect(close, &QToolButton::clicked, m_splitter, [this] { m_splitter->setFileTreeVisible(false); });
    m_files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_files->setTextElideMode(Qt::ElideRight);
    m_files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_files, 1);
    m_files->show();
    auto* empty = new QWidget;
    empty->setObjectName("fileTreeEmpty");
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(0, 0, 0, 0);
    emptyLayout->addStretch();
    auto* label = new QLabel("No workspace connected");
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    emptyLayout->addWidget(label);
    auto* connectButton = new QPushButton("Connect workspace");
    connectButton->setObjectName("fileTreeConnectWorkspaceButton");
    connectButton->setMinimumWidth(0);
    emptyLayout->addWidget(connectButton);
    emptyLayout->addStretch();
    layout->addWidget(empty, 1);
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::chooseWorkspace);
    m_splitter->initialize(m_chat, m_tabs, m_fileTreePanel);
    connect(m_splitter, &WorkbenchSplitter::visibilityChanged, this, &MainWindow::refreshWorkbench);
    new QShortcut(QKeySequence("Ctrl+Alt+B"), this, [this] { m_splitter->toggleContent(); });
    new QShortcut(QKeySequence("Ctrl+Shift+B"), this, [this] { m_splitter->toggleFileTree(); });
    new QShortcut(QKeySequence("Ctrl+Shift+E"), this, [this] {
        m_splitter->setFileTreeVisible(true);
        if (m_files->isVisible())
            m_files->setFocus();
    });
    new QShortcut(QKeySequence("Ctrl+W"), this, [this] {
        if (m_splitter->contentVisible() && m_tabs->currentIndex() >= 0)
            emit m_tabs->tabCloseRequested(m_tabs->currentIndex());
    });
    refreshWorkbench();
}
void MainWindow::refreshWorkbench() {
    if (!m_fileTreePanel)
        return;
    m_splitter->setHasContent(m_tabs->count() > 0);
    const bool content = m_splitter->contentVisible(), hasTabs = m_tabs->count() > 0,
               files = m_splitter->fileTreeExposed();
    findChild<QToolButton*>("openBrowserButton")->setVisible(!content && !hasTabs);
    findChild<QToolButton*>("openTerminalButton")->setVisible(!content && !hasTabs);
    findChild<QToolButton*>("showContentPanelButton")->setVisible(!content && hasTabs);
    findChild<QToolButton*>("toggleFileTreeButton")->setVisible(!content && !files);
    findChild<QToolButton*>("workbenchMenuButton")->setVisible(!content);
    findChild<QToolButton*>("contentFileTreeButton")->setVisible(!files);
    const auto* session = current();
    const bool connected = session && !session->workspace.isEmpty();
    m_files->setVisible(connected);
    m_fileTreePanel->findChild<QWidget*>("fileTreeEmpty")->setVisible(!connected);
    auto* title = m_fileTreePanel->findChild<QLabel*>("fileTreeTitle");
    title->setText(connected ? QFileInfo(session->workspace).fileName() : "Files");
    title->setToolTip(connected ? session->workspace : QString());
}
void MainWindow::revealContent() {
    m_splitter->setHasContent(m_tabs->count() > 0);
    m_splitter->showContent();
    refreshWorkbench();
}
} // namespace ade
