#include "ChatSurface.h"
#include "Icons.h"
#include "MainWindow.h"
#include "WorkbenchSplitter.h"
#include "WorkspaceChangesController.h"
#include "WorkspaceChangesView.h"
#include "SideChatPane.h"
#include "git/GitPanel.h"
#include "git/GitService.h"
#include <QButtonGroup>
#include <QFileInfo>
#include "WorkspaceFileTreeModel.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QSettings>
#include <QStackedWidget>
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
    new QShortcut(QKeySequence("Ctrl+Alt+B"), m_pages->widget(0), [this] { m_splitter->toggleContent(); });
    new QShortcut(QKeySequence("Ctrl+Shift+B"), m_pages->widget(0), [this] { m_splitter->toggleFileTree(); });
    // Upstream toggle_file_tree_files (dominant) and toggle_file_tree_diff (not
    // dominant). Both open the tree in a mode; neither is a toggle. Mode buttons
    // are created by initializeWorkspaceChanges below and looked up lazily.
    auto* filesMode = new QShortcut(QKeySequence("Ctrl+Shift+E"), m_pages->widget(0), [this] {
        m_splitter->setFileTreeVisible(true);
        if (auto* mode = m_fileTreePanel->findChild<QToolButton*>("fileTreeFilesMode"))
            mode->click();
        if (m_files->isVisible())
            m_files->setFocus();
    });
    filesMode->setObjectName("shortcut_toggle_file_tree_files");
    filesMode->setProperty("adeDominantNavigation", true);
    auto* diffMode = new QShortcut(QKeySequence("Ctrl+Shift+G"), m_pages->widget(0), [this] {
        m_splitter->setFileTreeVisible(true);
        if (auto* mode = m_fileTreePanel->findChild<QToolButton*>("fileTreeChangesMode"))
            mode->click();
    });
    diffMode->setObjectName("shortcut_toggle_file_tree_diff");
    new QShortcut(QKeySequence("Ctrl+W"), m_pages->widget(0), [this] {
        if (m_splitter->contentVisible() && m_tabs->currentIndex() >= 0)
            emit m_tabs->tabCloseRequested(m_tabs->currentIndex());
    });
    initializeWorkspaceChanges();
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
    auto* session = current();
    const auto selectedOwner=m_fileTreePanel->property("changedFilesOwner").toString();
    if(!selectedOwner.isEmpty()){
        const auto* selected=find(selectedOwner);
        const bool primary=m_fileTreePanel->property("changedFilesMainOwner").toString()==m_currentId;
        const bool available=selected&&!selected->archived&&(selectedOwner==m_currentId||m_sideChats.value(selectedOwner));
        const auto expected=m_fileTreePanel->property("changedFilesWorkspace").toString();
        if(primary&&available&&GitService::canonicalPath(selected->workspace).compare(expected,Qt::CaseInsensitive)==0)session=find(selectedOwner);
        else m_fileTreePanel->setProperty("changedFilesOwner",QString());
    }
    const bool connected = session && !session->workspace.isEmpty();
    const auto owner=session?session->id:QString();const auto primaryOwner=m_currentId;
    const auto workspace=connected?session->workspace:QString();
    const auto valid=[this,owner,primaryOwner,workspace]{const auto* selected=find(owner);return m_currentId==primaryOwner&&(!owner.isEmpty()?selected&&!selected->archived&&selected->workspace==workspace:!current());};
    if(m_workspaceChanges){
        const QPointer<MainWindow> self(this);
        m_workspaceChanges->setWorkspace(workspace);if(!self||!valid())return;
        m_workspaceChanges->setActive(files);if(!self||!valid())return;
    }
    const bool changes=m_fileTreePanel->property("changedFilesMode").toBool();
    m_files->setVisible(connected&&!changes);
    if(m_workspaceChangesView){
        m_workspaceChangesView->setVisible(connected&&changes);
        const auto* controller=m_workspaceChanges.data();
        if(controller){
            const QPointer<MainWindow> self(this);
            m_workspaceChangesView->setSnapshot(controller->workspace(),controller->phase(),controller->summary(),controller->hasSnapshot(),controller->error(),controller->busy());if(!self||!valid())return;
        }
    }
    if(m_fileModel->rootPath()!=workspace){
        const QPointer<MainWindow> self(this);m_files->setModel(connected?m_fileModel:nullptr);if(!self||!valid())return;
        const auto index=m_fileModel->setRootPath(workspace);if(!self||!valid())return;if(connected)m_files->setRootIndex(index);
    }
    m_fileTreePanel->findChild<QWidget*>("fileTreeEmpty")->setVisible(!connected);
    auto* title = m_fileTreePanel->findChild<QLabel*>("fileTreeTitle");
    title->setText(connected ? QFileInfo(workspace).fileName() : "Files");
    title->setToolTip(workspace);
}
void MainWindow::initializeWorkspaceChanges(){
    auto* layout=qobject_cast<QVBoxLayout*>(m_fileTreePanel->layout());
    auto* modes=new QHBoxLayout;modes->setSpacing(2);
    auto* files=new QToolButton;files->setObjectName("fileTreeFilesMode");files->setText("Files");files->setCheckable(true);files->setAutoRaise(true);
    auto* changes=new QToolButton;changes->setObjectName("fileTreeChangesMode");changes->setText("Changed files");changes->setToolTip("Changed files");changes->setCheckable(true);changes->setAutoRaise(true);
    auto* group=new QButtonGroup(m_fileTreePanel);group->addButton(files);group->addButton(changes);group->setExclusive(true);
    modes->addWidget(files);modes->addWidget(changes);modes->addStretch();layout->insertLayout(1,modes);
    auto* totals=new QLabel;totals->setObjectName("fileTreeChangesTotals");totals->setTextFormat(Qt::PlainText);totals->setWordWrap(true);layout->insertWidget(2,totals);totals->hide();
    m_workspaceChangesView=new WorkspaceChangesView(m_fileTreePanel);layout->addWidget(m_workspaceChangesView,1);
    m_workspaceChanges=new WorkspaceChangesController(m_git->service(),this);
    QSettings preferences(m_store.directory()+"/window.ini",QSettings::IniFormat);
    const bool mode=preferences.value("fileTreeViewMode").toString()=="diff";
    m_fileTreePanel->setProperty("changedFilesMode",mode);files->setChecked(!mode);changes->setChecked(mode);
    const auto select=[this,files,changes](bool diff){
        m_fileTreePanel->setProperty("changedFilesMode",diff);files->setChecked(!diff);changes->setChecked(diff);
        QSettings preferences(m_store.directory()+"/window.ini",QSettings::IniFormat);preferences.setValue("fileTreeViewMode",diff?"diff":"files");
        refreshWorkbench();
    };
    connect(files,&QToolButton::clicked,this,[select]{select(false);});connect(changes,&QToolButton::clicked,this,[select]{select(true);});
    connect(m_workspaceChanges,&WorkspaceChangesController::changed,this,[this,select,changes,totals]{
        const auto phase=m_workspaceChanges->phase();
        if(phase==WorkspaceChangesController::Phase::NotRepository){select(false);return;}
        const auto& summary=m_workspaceChanges->summary();
        changes->setText(summary.entries.isEmpty()?"Changed files":QString("Changes %1").arg(summary.entries.size()));
        changes->setToolTip(QString("Changed files · +%1 −%2").arg(summary.added).arg(summary.deleted));
        totals->setText(QString("+%1 −%2").arg(summary.added).arg(summary.deleted));totals->setVisible(!summary.entries.isEmpty());
        changes->setAccessibleName(changes->toolTip());refreshWorkbench();
    });
    connect(m_workspaceChangesView,&WorkspaceChangesView::openRequested,this,&MainWindow::openWorkspaceDiff);
}
void MainWindow::refreshWorkspaceChanges(){if(m_workspaceChanges)m_workspaceChanges->requestRefresh();}
void MainWindow::showWorkspaceChanges(const QString& workspace,const QString& ownerChat){
    const auto owner=ownerChat.isEmpty()?m_currentId:ownerChat;const auto* session=find(owner);
    const auto canonical=GitService::canonicalPath(workspace);
    if(!session||session->archived||canonical.isEmpty()||(owner!=m_currentId&&!m_sideChats.value(owner))||GitService::canonicalPath(session->workspace).compare(canonical,Qt::CaseInsensitive)!=0)return;
    const auto primary=m_currentId;const auto expectedWorkspace=session->workspace;const QPointer<MainWindow> self(this);
    m_fileTreePanel->setProperty("changedFilesOwner",owner);m_fileTreePanel->setProperty("changedFilesMainOwner",primary);m_fileTreePanel->setProperty("changedFilesWorkspace",canonical);
    m_splitter->setFileTreeVisible(true);if(!self||m_currentId!=primary||!find(owner)||find(owner)->workspace!=expectedWorkspace)return;
    if(auto* button=m_fileTreePanel->findChild<QToolButton*>("fileTreeChangesMode"))button->click();
}
void MainWindow::revealContent() {
    m_splitter->setHasContent(m_tabs->count() > 0);
    m_splitter->showContent();
    refreshWorkbench();
}
} // namespace ade
