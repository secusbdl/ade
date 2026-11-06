#include "MainWindow.h"
#include <QStandardItemModel>
#include "AgentCycleController.h"
#include "ChevronToolButton.h"
#include "WorkbenchSplitter.h"
#include "ChatSurface.h"
#include "Composer.h"
#include "ConversationView.h"
#include "AttachmentStrip.h"
#include "attachments/AttachmentStore.h"
#include "Icons.h"
#include "ProviderDialog.h"
#include "ModelSelection.h"
#include "ContextPreferences.h"
#include "SettingsPage.h"
#include "DownloadsPage.h"
#include "BrowserSessionStore.h"
#include "browser/browserwidget.h"
#include "browser/BrowserToolBridge.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "core/UtilityService.h"
#include "runtime/AgentToolHost.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalWidget.h"
#include "git/GitPanel.h"
#include "git/WorktreeSetupRunner.h"
#include "git/WorktreeCleanupController.h"
#include "git/WorktreeSettingsController.h"
#include "git/WorktreeSettingsPane.h"
#include <QApplication>
#include <QBuffer>
#include <QImage>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStackedWidget>
#include <QRegularExpression>
#include <QStyleHints>
#include <QShortcut>
#include <QSplitter>
#include "SidebarSplitter.h"
#include "SidebarNavigation.h"
#include "FlowLayout.h"
#include "links/TranscriptLinkProvenanceCodec.h"
#include "apps/MiniAppPage.h"
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <algorithm>
namespace ade {
namespace {
QToolButton* button(const QString& image,const QString& hint,QWidget* parent=nullptr) {
    auto* b=new QToolButton(parent);b->setIcon(icon(image));b->setToolTip(hint);b->setAccessibleName(hint);b->setFixedSize(30,30);b->setAutoRaise(true);return b;
}
}
MainWindow::MainWindow(QString dataDirectory,QWidget* parent):QMainWindow(parent),m_store(std::move(dataDirectory)),m_providerRepository(m_store.directory()+"/providers.json") {
    setWindowTitle("ADE");setObjectName("adeMainWindow");resize(1540,940);setMinimumSize(350,250);
    setWindowIcon(icon("agent",QColor("#4169ff")));
    QString error,providerError;m_sessions=m_store.load(&error);m_providers=m_providerRepository.load(&providerError);if(error.isEmpty())error=providerError;
    buildUi();applyPreferences();refreshModels();refreshAgents();
    QSettings settings(m_store.directory()+"/window.ini",QSettings::IniFormat);
    const auto lastId=settings.value("lastConversationId").toString();
    if(const auto* saved=find(lastId);saved&&!saved->archived&&saved->sideChatParentId.isEmpty())selectConversation(lastId);
    if(m_currentId.isEmpty())for(const auto& s:m_sessions)if(!s.archived&&s.sideChatParentId.isEmpty()){selectConversation(s.id);break;}
    if(m_currentId.isEmpty())newConversation();
    if(!error.isEmpty())m_status->setText(error);
    else QTimer::singleShot(0,this,&MainWindow::retryConversationCleanup);
    if(settings.contains("geometry"))restoreGeometry(settings.value("geometry").toByteArray());
    if(settings.contains("splitter"))m_splitter->restoreLegacyState(settings.value("splitter").toByteArray());
    initializeSideChats();
    initializeBrowserSession();
}
void MainWindow::buildUi() {
    m_browserBridge=new BrowserToolBridge(this);
    m_pages=new QStackedWidget;setCentralWidget(m_pages);
    auto* central=new QWidget;central->setObjectName("appSurface");m_pages->addWidget(central);
    auto* root=new QHBoxLayout(central);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
    m_sidebarSplitter=new SidebarSplitter(m_store.directory()+"/window.ini");root->addWidget(m_sidebarSplitter,1);
    m_sidebar=new QWidget;m_sidebar->setObjectName("sidebar");m_sidebarSplitter->addWidget(m_sidebar);
    auto* side=new QVBoxLayout(m_sidebar);side->setContentsMargins(10,12,10,12);side->setSpacing(10);
    auto* collapse=button("sidebar","Toggle sidebar");collapse->setObjectName("sidebarToggleButton");side->addWidget(collapse);side->addSpacing(18);
    auto* newAgent=new QPushButton(icon("new"),"New Agent");newAgent->setObjectName("newAgentButton");newAgent->setCursor(Qt::PointingHandCursor);newAgent->setMinimumHeight(36);side->addWidget(newAgent);
    m_search=new QLineEdit;m_search->setObjectName("agentSearch");m_search->addAction(icon("search"),QLineEdit::LeadingPosition);m_search->setPlaceholderText("Search agents…");side->addWidget(m_search);side->addSpacing(8);
    auto* section=new QHBoxLayout;auto* agentsLabel=new QLabel("Agents");agentsLabel->setProperty("muted",true);section->addWidget(agentsLabel);section->addStretch();
    auto* grouping=new ChevronToolButton;grouping->setText("Group by Age");grouping->setAutoRaise(true);section->addWidget(grouping);side->addLayout(section);
    m_agents=new QListWidget;m_agents->setObjectName("agentList");m_agents->setContextMenuPolicy(Qt::CustomContextMenu);m_agents->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);m_agents->setTextElideMode(Qt::ElideRight);side->addWidget(m_agents,1);
    auto* bottom=new QHBoxLayout;
    auto* settings=new QToolButton;
    settings->setObjectName("settingsButton");settings->setText("Settings");settings->setIcon(icon("settings"));
    settings->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);settings->setAutoRaise(true);
    settings->setAccessibleName("Settings");settings->setToolTip("Settings (Ctrl+,)");settings->setMinimumHeight(30);
    settings->setCursor(Qt::PointingHandCursor);bottom->addWidget(settings);bottom->addStretch();side->addLayout(bottom);
    m_splitter=new WorkbenchSplitter(m_store.directory()+"/window.ini");m_sidebarSplitter->addWidget(m_splitter);
    m_chat=new ChatSurface;auto* chatLayout=m_chat->footerLayout();
    m_sidebarReopen=button("sidebar","Show sidebar");m_sidebarReopen->setObjectName("sidebarReopenButton");m_sidebarReopen->setVisible(m_sidebarSplitter->isCollapsed());m_chat->headerLayout()->addWidget(m_sidebarReopen);m_chat->headerLayout()->addStretch();
    connect(m_sidebarReopen,&QToolButton::clicked,m_sidebarSplitter,&SidebarSplitter::toggle);
    connect(m_sidebarSplitter,&SidebarSplitter::collapsedChanged,m_sidebarReopen,&QWidget::setVisible);
    m_transcript=new ConversationView;m_transcript->setObjectName("conversationView");m_chat->setHistoryWidget(m_transcript);connectTurnFileControls(m_transcript);
    connect(m_transcript,&ConversationView::imageRequested,this,[this](quint64 request,const QString& id,const QString& row,const QString& part,const QUrl& url){provideTranscriptImage(request,id,row,part,url);});
    m_transcriptRefresh=new QTimer(this);m_transcriptRefresh->setSingleShot(true);m_transcriptRefresh->setInterval(16);
    connect(m_transcriptRefresh,&QTimer::timeout,this,&MainWindow::renderConversation);
    m_status=new QLabel;m_status->setObjectName("sessionStatus");m_status->setWordWrap(true);m_status->setProperty("muted",true);chatLayout->addWidget(m_status);
    auto* composerBox=new QWidget;m_composerBox=composerBox;composerBox->setObjectName("composerBox");
    auto* composerColumns=new QHBoxLayout(composerBox);composerColumns->setContentsMargins(8,8,8,8);composerColumns->setSpacing(4);
    auto* inputColumn=new QWidget;inputColumn->setObjectName("composerEditorColumn");inputColumn->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);
    auto* compose=new QVBoxLayout(inputColumn);compose->setContentsMargins(0,0,0,0);compose->setSpacing(4);composerColumns->addWidget(inputColumn,1);
    m_attachments=new AttachmentStrip;compose->addWidget(m_attachments);
    connect(m_attachments,&AttachmentStrip::removeRequested,this,[this](int index){
        auto* session=current();if(!session||index<0||index>=session->draftAttachments.size())return;
        const auto previous=session->draftAttachments;session->draftAttachments.removeAt(index);
        if(!saveSessions())session->draftAttachments=previous;
        refreshAttachments();
    });
    m_composer=new Composer;compose->addWidget(m_composer);
    connect(m_composer,&Composer::filesAttached,this,&MainWindow::attachFiles);
    connect(m_composer,&Composer::imageAttached,this,&MainWindow::attachImage);
    auto* draftTimer=new QTimer(this);draftTimer->setSingleShot(true);draftTimer->setInterval(500);
    connect(m_composer,&QPlainTextEdit::textChanged,this,[this,draftTimer]{if(auto* s=current()){s->draft=m_composer->toPlainText();draftTimer->start();}updateComposerState();});
    connect(draftTimer,&QTimer::timeout,this,&MainWindow::saveSessions);
    auto* controls=new QHBoxLayout;controls->setContentsMargins(0,0,0,0);controls->setSpacing(4);
    auto* selectors=new QWidget;selectors->setObjectName("composerSelectors");selectors->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Minimum);auto* flow=new FlowLayout(selectors);
    m_models=new QComboBox;m_models->setObjectName("modelSelector");m_models->setSizeAdjustPolicy(QComboBox::AdjustToContents);m_models->setMinimumWidth(0);m_models->setMaximumWidth(270);flow->addWidget(m_models);
    m_permissions=new QComboBox;m_permissions->setObjectName("permissionSelector");m_permissions->addItem("Ask before changes",false);m_permissions->addItem("Always allow",true);flow->addWidget(m_permissions);controls->addWidget(selectors,1);
    connect(m_permissions,&QComboBox::currentIndexChanged,this,[this]{
        const auto* session=current();if(!session)return;
        const bool previous=session->alwaysAllow;
        if(session->archived||(m_engines.value(session->id)&&m_engines[session->id]->isBusy())){
            const QSignalBlocker blocker(m_permissions);m_permissions->setCurrentIndex(previous?1:0);return;
        }
        auto next=m_sessions;for(auto& candidate:next)if(candidate.id==session->id)candidate.alwaysAllow=m_permissions->currentData().toBool();
        if(!commitConversationState(next,m_store.pendingCleanup())){const QSignalBlocker blocker(m_permissions);m_permissions->setCurrentIndex(previous?1:0);}
    });
    m_models->setEditable(true);m_models->setInsertPolicy(QComboBox::NoInsert);m_models->lineEdit()->setPlaceholderText("Search models…");
    m_models->completer()->setCompletionMode(QCompleter::PopupCompletion);m_models->completer()->setFilterMode(Qt::MatchContains);m_models->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    connect(m_models,&QComboBox::currentIndexChanged,this,[this]{m_models->setToolTip(m_models->currentText());if(m_models->lineEdit())m_models->lineEdit()->setCursorPosition(0);});
    connect(m_models,&QComboBox::activated,this,[this](int index){if(auto* s=current()){const auto selection=ModelSelection::fromJson(m_models->itemData(index).toJsonObject());s->providerId=selection.providerId;s->model=selection.modelId;s->presetId=selection.presetId;saveSessions();}});
    m_stop=button("stop","Stop agent");m_stop->setObjectName("stopButton");m_stop->setEnabled(false);m_stop->hide();m_stop->setFixedSize(32,32);controls->addWidget(m_stop,0,Qt::AlignBottom);compose->addLayout(controls);
    auto* actions=new QWidget;actions->setObjectName("composerActions");actions->setFixedWidth(32);auto* rail=new QVBoxLayout(actions);rail->setContentsMargins(0,0,0,0);rail->setSpacing(4);rail->addStretch();
    auto* attach=button("attach","Attach files");attach->setObjectName("attachFilesButton");attach->setFixedSize(32,32);rail->addWidget(attach);
    m_send=button("send","Send message");m_send->setObjectName("sendButton");m_send->setIcon(icon("send",Qt::white));m_send->setFixedSize(32,32);rail->addWidget(m_send);composerColumns->addWidget(actions);
    chatLayout->addWidget(composerBox);
    auto* workspaceButton=new ChevronToolButton;workspaceButton->setChevronVisible(false);m_workspace=workspaceButton;m_workspace->setObjectName("connectWorkspaceButton");m_workspace->setIcon(icon("folder"));m_workspace->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);m_workspace->setText("Connect workspace  +");m_workspace->setAutoRaise(true);chatLayout->addWidget(m_workspace,0,Qt::AlignLeft);
    m_tabs=new QTabWidget;m_tabs->setObjectName("contentTabs");m_tabs->setDocumentMode(true);m_tabs->setTabsClosable(true);m_tabs->setMovable(true);m_tabs->setMinimumWidth(0);
    auto* tabControls=new QWidget;auto* tc=new QHBoxLayout(tabControls);tc->setContentsMargins(0,0,4,0);tc->setSpacing(1);auto* plus=button("plus","New browser tab");plus->setObjectName("newBrowserTabButton");auto* files=button("folder","Show file tree (Ctrl+Shift+B)");files->setObjectName("contentFileTreeButton");auto* terminal=button("terminal","Open terminal");terminal->setObjectName("newTerminalTabButton");tc->addWidget(plus);tc->addWidget(terminal);tc->addWidget(files);m_tabs->setCornerWidget(tabControls);
    m_fileModel=new QFileSystemModel(this);m_files=new QTreeView(this);m_files->hide();m_files->setObjectName("workspaceFiles");m_files->setModel(m_fileModel);m_files->setHeaderHidden(true);for(int col=1;col<4;++col)m_files->hideColumn(col);
    m_downloads=new DownloadsPage(m_store.directory(),this);m_downloads->hide();
    connect(m_downloads,&DownloadsPage::storageErrorOccurred,this,[this](const QString& error){m_status->setText(error);});
    auto* downloads=button("download","Downloads (Ctrl+J)");tc->addWidget(downloads);connect(downloads,&QToolButton::clicked,this,&MainWindow::showDownloads);
    initializeGit();auto* git=button("branch","Source control (Ctrl+Shift+G)");git->setObjectName("sourceControlButton");tc->addWidget(git);connect(git,&QToolButton::clicked,this,&MainWindow::showGit);
    initializeWorkbench();
    connect(newAgent,&QPushButton::clicked,this,&MainWindow::newConversation);
    connect(collapse,&QToolButton::clicked,m_sidebarSplitter,&SidebarSplitter::toggle);
    connect(settings,&QToolButton::clicked,this,&MainWindow::showSettings);
    connect(m_search,&QLineEdit::textChanged,this,&MainWindow::refreshAgents);
    connect(m_agents,&QListWidget::currentItemChanged,this,[this](QListWidgetItem* item){if(item)selectConversation(item->data(Qt::UserRole).toString());});
    connect(m_agents,&QWidget::customContextMenuRequested,this,&MainWindow::showConversationMenu);
    connect(m_send,&QToolButton::clicked,this,&MainWindow::send);connect(m_composer,&Composer::submitted,this,&MainWindow::send);
    connect(m_stop,&QToolButton::clicked,this,[this]{if(m_engines.contains(m_currentId))m_engines[m_currentId]->cancel();});
    connect(m_composer,&Composer::stopRequested,m_stop,&QToolButton::click);
    connect(m_workspace,&QToolButton::clicked,this,&MainWindow::chooseWorkspace);
    connect(m_transcript,&ConversationView::scopedLinkActivated,this,[this](const QString& id,const QString& row,const QString& part,const QUrl& url){routeTranscriptLink(id,row,part,url);});
    connect(m_transcript,&ConversationView::approvalDecided,this,[this](const QString& conversationId,const QString& rowId,const QString& partId,const QString& toolCallId,bool allowed){
        // Render updates are coalesced. A visible button can therefore refer to
        // an older round even when a provider reuses its tool-call identifier.
        if(conversationId!=m_currentId||conversationId!=m_transcript->conversationId())return;
        auto* engine=m_engines.value(conversationId);if(!engine||!engine->isBusy())return;
        for(const auto& value:engine->displayMessages()){
            const auto row=value.toObject();if(row["id"]!=rowId)continue;
            for(const auto& entry:row["parts"].toArray()){
                const auto part=entry.toObject();
                if(part["id"]==partId&&part["type"]=="tool"&&part["toolCallId"]==toolCallId&&part["state"]=="approval-requested"){
                    engine->approveTool(toolCallId,allowed);return;
                }
            }
        }
    });
    connect(plus,&QToolButton::clicked,this,[this]{addBrowserTab();});
    connect(terminal,&QToolButton::clicked,this,&MainWindow::showTerminal);
    connect(files,&QToolButton::clicked,m_splitter,&WorkbenchSplitter::toggleFileTree);
    connect(m_files,&QTreeView::doubleClicked,this,[this](const QModelIndex& index){if(!m_fileModel->isDir(index))openFile(m_fileModel->filePath(index));});
    connect(m_tabs,&QTabWidget::tabCloseRequested,this,[this](int index){
        auto* page=m_tabs->widget(index);if(!page)return;
        const auto sideId=page->property("sideChatAgentId").toString();if(!sideId.isEmpty()){discardSideChat(sideId);return;}
        if(auto* editor=qobject_cast<EditorWidget*>(page);editor&&!editor->requestClose())return;
        const auto instance=page->property("appInstanceId").toString();
        if(!instance.isEmpty())if(auto* host=m_toolHosts.value(page->property("appAgentId").toString())){
            host->apps()->closeTab(instance);return;
        }
        m_tabs->removeTab(index);if(page!=m_files&&page!=m_downloads&&page!=m_git)page->deleteLater();refreshWorkbench();scheduleBrowserSessionSave();
    });
    connect(m_tabs,&QTabWidget::currentChanged,this,[this](int index){refreshWorkbench();if(auto* page=m_tabs->widget(index)){if(m_fullscreenBrowser&&!page->isAncestorOf(m_fullscreenBrowser))setBrowserFullscreen(m_fullscreenBrowser,false);const auto id=page->property("browserTabId").toString();if(!id.isEmpty())m_browserBridge->setActiveTab(id);}});
    connect(attach,&QToolButton::clicked,this,[this]{attachFiles(QFileDialog::getOpenFileNames(this,"Attach files",current()?current()->workspace:QString()));});
    new QShortcut(QKeySequence("Ctrl+N"),this,[this]{newConversation();});
    new QShortcut(QKeySequence("Ctrl+B"),this,[this]{m_sidebarSplitter->toggle();if(m_sidebarSplitter->isCollapsed())m_sidebarReopen->setFocus();else m_search->setFocus();});
    new QShortcut(QKeySequence("Ctrl+,"),this,[this]{showSettings();});
    new QShortcut(QKeySequence("Ctrl+Shift+O"),this,[this]{chooseWorkspace();});
    new QShortcut(QKeySequence("Ctrl+T"),this,[this]{addBrowserTab();});
    new QShortcut(QKeySequence("Ctrl+Alt+T"),this,[this]{showTerminal();});
    new QShortcut(QKeySequence("Ctrl+J"),this,[this]{showDownloads();});
    new QShortcut(QKeySequence("Ctrl+Shift+G"),this,[this]{showGit();});
    m_fullscreenExit=new QShortcut(QKeySequence(Qt::Key_Escape),this,[this]{if(m_fullscreenBrowser)setBrowserFullscreen(m_fullscreenBrowser,false);});m_fullscreenExit->setEnabled(false);
    m_settings=new SettingsPage(m_store.directory());m_pages->addWidget(m_settings);
    m_sidebarNavigation=new SidebarNavigation(m_agents,grouping,agentsLabel,m_settings,this);m_sidebarNavigation->setGitService(m_git->service());
    connect(m_sidebarNavigation,&SidebarNavigation::errorOccurred,this,&MainWindow::conversationError);
    configureWorktreeSettingsPage();
    connect(m_settings,&SettingsPage::backRequested,this,[this]{m_pages->setCurrentIndex(0);});
    connect(m_settings,&SettingsPage::providersRequested,this,[this]{showProviders();});
    connect(m_settings,&SettingsPage::addProviderRequested,this,[this]{showProviders(true);});
    connect(m_settings,&SettingsPage::providerEditRequested,this,[this](const QString& id){showProviders(false,id);});
    connect(m_settings,&SettingsPage::providerVisibilityChanged,this,[this]{
        QString error;const auto providers=m_providerRepository.load(&error);
        if(!error.isEmpty()){m_settings->reportProviderImportResult(error,true);return;}
        m_providers=providers;const QPointer<MainWindow> self(this);refreshModels();if(self)refreshSideChats();
    });
    connect(m_settings,&SettingsPage::preferencesChanged,this,&MainWindow::applyPreferences);
    connect(qApp->styleHints(),&QStyleHints::colorSchemeChanged,this,[this]{
        if(m_settings->preference("personalization.appearance").toString()=="system")applyPreferences();
    });
    connect(m_settings,&SettingsPage::openUrlRequested,this,[this](const QUrl& url){m_pages->setCurrentIndex(0);addBrowserTab(url);});
    connect(m_settings,&SettingsPage::clearBrowsingDataRequested,this,&MainWindow::clearBrowsingData);
    connect(m_settings,&SettingsPage::pluginCredentialsChanged,this,&MainWindow::refreshPluginConfiguration);
    initializePluginConnections();
    connect(m_settings,&SettingsPage::restoreConversationRequested,this,[this](const QString& id){restoreConversation(id);});
    connect(m_settings,&SettingsPage::deleteConversationRequested,this,&MainWindow::requestDeleteConversation);
    initializeAgentControls();initializeCompletions();initializeCommandCenter();initializeNavigationShortcuts();
}
Conversation* MainWindow::find(const QString& id){for(auto& s:m_sessions)if(s.id==id)return &s;return nullptr;}
Conversation* MainWindow::current(){return find(m_currentId);}
void MainWindow::refreshAgents(){
    if(!refreshAgentCycle()||!m_sidebarNavigation)return;
    QSet<QString> busy;
    for(auto it=m_engines.cbegin();it!=m_engines.cend();++it)
        if(it.value()&&it.value()->isBusy())busy.insert(it.key());
    m_sidebarNavigation->refresh(m_sessions,busy,m_currentId,m_search->text());
}
void MainWindow::refreshModels(){
    const QSignalBlocker blocker(m_models);const auto previous=ModelSelection::fromJson(m_models->currentData().toJsonObject()).key();
    m_models->clear();
    const auto selections=availableModelSelections(m_providers,m_settings?m_settings->preference("modelCatalogs").toObject():QJsonObject{},m_settings?m_settings->preference("modelPresets").toArray():QJsonArray{});
    int restore=-1;
    for(const auto& selection:selections){m_models->addItem(selection.label,selection.toJson());m_models->setItemData(m_models->count()-1,selection.modelId,Qt::ToolTipRole);if(selection.key()==previous)restore=m_models->count()-1;}
    if(restore>=0)m_models->setCurrentIndex(restore);
    if(m_models->count()==0)m_models->addItem("Configure a provider…",QJsonObject{});
    restoreModelSelection();
}
void MainWindow::restoreModelSelection(){
    const auto* s=current();if(!s||s->providerId.isEmpty())return;
    ModelSelection saved{s->providerId,s->model,s->presetId,{},{}};
    for(int i=0;i<m_models->count();++i){const auto candidate=ModelSelection::fromJson(m_models->itemData(i).toJsonObject());if((!saved.presetId.isEmpty()&&candidate.presetId==saved.presetId)||candidate.key()==saved.key()){m_models->setCurrentIndex(i);return;}}
    // Old conversations keep their exact model even if the default/catalog changed.
    // A removed preset keeps that model but no longer supplies its overrides.
    ProviderConfig provider;saved.presetId.clear();
    if(saved.resolve(m_providers,&provider)){
        saved.label=provider.name+" / "+saved.modelId;m_models->addItem(saved.label,saved.toJson());const int index=m_models->count()-1;
        if(!isModelEnabled(provider,saved.modelId)){
            if(auto* model=qobject_cast<QStandardItemModel*>(m_models->model()))model->item(index)->setEnabled(false);
            m_models->setItemData(index,"Hidden from new model choices. This conversation keeps its saved model.",Qt::ToolTipRole);
        }
        m_models->setCurrentIndex(index);
    }
    else {m_models->addItem("Saved provider unavailable",saved.toJson());m_models->setCurrentIndex(m_models->count()-1);}
}
void MainWindow::selectConversation(const QString& id){
    auto* s=find(id);if(!s||!s->sideChatParentId.isEmpty())return;
    if(auto* previous=current();previous&&previous->id!=id){
        previous->draft=m_composer->toPlainText();
        if(!saveSessions()){refreshAgents();return;}
    }
    m_currentId=id;m_fileChangesAgent.clear();if(!s->archived)m_liveConversationOwners.insert(id);restoreModelSelection();
    {const QSignalBlocker blocker(m_permissions);m_permissions->setCurrentIndex(s->alwaysAllow?1:0);}
    {const QSignalBlocker block(m_composer);m_composer->setPlainText(s->draft);}
    refreshAttachments();
    m_workspace->setText(s->workspace.isEmpty()?"Connect workspace  +":QFileInfo(s->workspace).fileName());static_cast<ChevronToolButton*>(m_workspace)->setChevronVisible(!s->workspace.isEmpty());m_workspace->setToolTip(s->workspace);
    m_files->setModel(s->workspace.isEmpty()?nullptr:m_fileModel);
    if(!s->workspace.isEmpty())m_files->setRootIndex(m_fileModel->setRootPath(s->workspace));
    const bool busy=m_engines.contains(id)&&m_engines[id]->isBusy();m_models->setEnabled(!busy&&!s->archived);m_permissions->setEnabled(!busy&&!s->archived);m_workspace->setEnabled(!busy&&!s->archived);m_composer->setReadOnly(s->archived);updateComposerState();
    m_sidebarSplitter->showWorkbench();m_status->clear();renderConversation();refreshAgents();
    if(!s->archived){QSettings settings(m_store.directory()+"/window.ini",QSettings::IniFormat);settings.setValue("lastConversationId",id);}
    updateGitContext();refreshCompletionContext();refreshAgentControls();refreshFileChanges();refreshSideChats();
}
void MainWindow::renderConversation(){
    m_transcriptRefresh->stop();
    const auto* session=current();if(!session){m_transcript->clear();m_chat->setHistoryPresent(false);refreshWorkbench();refreshAgentControls();return;}
    const auto* engine=m_engines.value(session->id);
    if(!engine&&!session->archived&&!m_pendingMutationHydration.contains(session->id)){
        bool pending=false;
        for(int i=0;i<session->messages.size();++i)for(const auto& value:session->messages[i].toObject()["metadata"].toObject()["mutationInvocations"].toArray()){
            const auto invocation=value.toObject();
            if(invocation["agentId"]!=session->id||QUuid(invocation["invocationId"].toString()).isNull())continue;
            bool resultPresent=false;
            for(int next=i+1;next<session->messages.size()&&session->messages[next].toObject()["role"]=="tool";++next)
                resultPresent|=session->messages[next].toObject()["tool_call_id"]==invocation["toolCallId"];
            pending|=!invocation["settled"].toBool()||!resultPresent;
        }
        if(pending){
            const auto id=session->id;m_pendingMutationHydration.insert(id);
            // A committed write may outlive the previous window. Hydrate after
            // selection finishes so recovery can save through the normal chat
            // committer without invalidating the selection's session pointers.
            QTimer::singleShot(0,this,[this,id]{
                m_pendingMutationHydration.remove(id);
                const auto* selected=find(id);if(!selected||selected->archived||m_currentId!=id)return;
                const QPointer<MainWindow> self(this);engineFor(id);
                if(self&&m_currentId==id)renderConversation();
            });
        }
    }
    const auto rows=engine?engine->displayMessages():AgentEngine::displayHistory(session->messages);
    if(engine)updateTranscriptLinkRows(session->id,rows);
    const QPointer<MainWindow> self(this);m_transcript->setConversation(session->id,rows,engine&&engine->isBusy());if(!self)return;
    m_chat->setHistoryPresent(!rows.isEmpty());refreshWorkbench();refreshAgentControls();
}
bool MainWindow::saveSessions(){QString error;if(!m_store.save(m_sessions,&error)){m_status->setText("Could not save conversations: "+error);return false;}updateGitUsageTracking();return true;}
AgentEngine* MainWindow::engineFor(const QString& id){if(m_engines.contains(id))return m_engines[id];
    if(const auto* session=find(id);session&&!session->archived)m_liveConversationOwners.insert(id);
    auto* engine=new AgentEngine(this);engine->setQuestionToolEnabled(true);m_engines.insert(id,engine);if(auto* s=find(id)){engine->setMessages(s->messages);engine->setWorkspace(s->workspace);}
    auto* host=new AgentToolHost(engine,m_browserBridge,id,m_store.directory(),engine);m_toolHosts.insert(id,host);host->setWorkspace(engine->workspace());
    connectMutationControls(id,host);
    connect(engine,&AgentEngine::turnAboutToStart,this,[this,id,engine](const QJsonObject& message){
        const QPointer<MainWindow> self(this);const QPointer<AgentEngine> safe(engine);QString error;
        if(const auto* session=find(id)){
            QJsonObject activePreset;for(const auto& value:m_settings->preference("modelPresets").toArray())if(!session->presetId.isEmpty()&&value.toObject()["id"]==session->presetId){activePreset=value.toObject();break;}
            const auto entries=activePreset.isEmpty()?m_settings->preference("utilityModels.contextCompression").toArray():activePreset["contextCompression"].toArray();
            engine->setCompressionModels(resolveModelEntries(m_providers,entries,m_settings->preference("modelCatalogs").toObject()));
        }
        if(!configureTurnContext(id,message["content"].toString(),&error)){if(self)m_status->setText(error);if(safe)safe->cancel();return;}
        if(self&&safe){beginTranscriptLinkTurn(id,true);if(!saveSessions()&&safe)safe->cancel();}
    });
    m_linkKnownRows[id].clear();m_linkProvenance[id]=find(id)?decodeLinkProvenance(find(id)->transcriptLinkProvenance):QHash<QString,TranscriptLinkProvenance>{};m_linkTurnContext.remove(id);
    updateTranscriptLinkRows(id);
    connect(host,&QObject::destroyed,this,[this,id,host]{if(m_toolHosts.value(id)==host)m_toolHosts.remove(id);});
    connectAppTabs(host);
    connect(host,&AgentToolHost::output,engine,[engine](const QString& requestId,const QString& output){engine->appendExternalToolOutput(requestId,output);});
    connect(host,&AgentToolHost::attachmentCreated,engine,[engine](const QString& requestId,const QString& path,const QString&){engine->appendExternalToolOutput(requestId,"\nCreated attachment: "+QFileInfo(path).fileName()+"\n");});
    connect(engine,&AgentEngine::displayChanged,this,[this,id]{if(id==m_currentId&&!m_transcriptRefresh->isActive())m_transcriptRefresh->start();});
    connect(engine,&AgentEngine::finished,this,[this,id]{updateTranscriptLinkRows(id);m_linkTurnContext.remove(id);});
    connect(engine,&AgentEngine::toolStarted,this,[this,id](const QString&,const QString& name,const QJsonObject&){if(id==m_currentId)m_status->setText(name);});
    connect(engine,&AgentEngine::toolFinished,this,[this,id](const QString&,const QString&,bool error){if(id==m_currentId)m_status->setText(error?"Tool returned an error":"Working…");});
    connect(engine,&AgentEngine::approvalRequired,this,[this,id](const QString&,const QString& name,const QJsonObject&){if(id==m_currentId){m_status->setText("Approval required: "+name);renderConversation();}});
    connect(engine,&AgentEngine::busyChanged,this,[this,id](bool busy){if(id==m_currentId){m_models->setEnabled(!busy);m_permissions->setEnabled(!busy);m_workspace->setEnabled(!busy);updateComposerState();}refreshAgents();});
    connect(engine,&AgentEngine::failed,this,[this,id](const QString& error){if(id==m_currentId)m_status->setText(error);});
    connect(engine,&AgentEngine::compressionStarted,this,[this,id]{if(id==m_currentId)m_status->setText("Compressing context…");});
    connect(engine,&AgentEngine::compressionFinished,this,[this,id](bool applied,const QString& error){if(id==m_currentId)m_status->setText(applied?"Working…":"Automatic compression could not finish; full history retained. "+error);});
    connect(engine,&AgentEngine::finished,this,[this,id,engine]{if(auto* s=find(id)){s->messages=engine->messages();s->updated=QDateTime::currentMSecsSinceEpoch();if(!s->messages.isEmpty())s->lastMessageAt=s->updated;}if(engine->lastTurnSucceeded())generateTitle(id);saveSessions();refreshAgents();if(id==m_currentId){if(m_status->text()=="Working…"||m_status->text()=="Working...")m_status->clear();renderConversation();}});
    connectAgentControls(id,engine);
    updateGitContext();
    return engine;
}
void MainWindow::send(){
    auto* session=current();
    if(!session||session->archived||(m_composer->toPlainText().trimmed().isEmpty()&&session->draftAttachments.isEmpty()))return;
    if(gitPathBusy(session->workspace)){m_status->setText("Wait for the workspace operation to finish before running this agent.");return;}
    if(m_composer->toPlainText().trimmed()=="/compact"&&session->draftAttachments.isEmpty()){compressContext();return;}
    const auto id=session->id;const QPointer<MainWindow> self(this);const QPointer<AgentEngine> engine(engineFor(id));
    if(!self||!engine)return;
    session=find(id);if(!session||session->archived)return;
    const bool wasBusy=engine->isBusy();ProviderConfig provider=engine->provider();ModelSelection selection{session->providerId,session->model,session->presetId,{},{}};
    if(!wasBusy){
        selection=ModelSelection::fromJson(m_models->currentData().toJsonObject());
        if(!selection.resolve(m_providers,&provider)){showProviders();return;}
        if(m_models->currentText()!=m_models->itemText(m_models->currentIndex())){m_status->setText("Select a model from the search results before sending.");return;}
        engine->setModelChain(selectedModelChain(selection,m_providers,m_settings->preference("modelPresets").toArray(),m_settings->preference("modelCatalogs").toObject()),selection.presetId);
        engine->setWorkspace(session->workspace);engine->setAlwaysAllow(session->alwaysAllow);
        const QPointer<AgentToolHost> host=m_toolHosts.value(id);
        if(host){
            QString error;if(!host->setWorkspace(session->workspace,&error)){m_status->setText(error);return;}
        }
    }
    session=find(id);if(!session||session->archived||id!=m_currentId)return;
    QString error;QJsonObject metadata;
    if(auto* host=m_toolHosts.value(id))metadata=host->captureAttachments(session->draftAttachments,&error);
    if(!error.isEmpty()){m_status->setText("Attachment: "+error);return;}
    auto previous=*session;previous.draft=m_composer->toPlainText();const auto submittedDraft=previous.draft;const auto text=submittedDraft.trimmed();
    const auto history=engine->messages(),queue=engine->queuedMessages();
    if(!wasBusy){session->providerId=provider.id;session->model=provider.model;session->presetId=selection.presetId;}
    session->draft.clear();session->draftAttachments={};
    if(session->messages.isEmpty()&&!session->titleLocked)session->title=text.isEmpty()?"Attached files":text.simplified().left(55);
    // No optimistic user row: the committer publishes history + queue together.
    const bool accepted=engine->sendUserMessage(text,metadata["attachments"].toArray(),metadata["pathReferences"].toObject(),metadata["pathMetadata"].toObject());
    if(!self||!engine)return;
    if(!accepted&&engine->messages()==history&&engine->queuedMessages()==queue){
        if(auto* rejected=find(id)){
            *rejected=previous;
            // Preacceptance failed/finished observers may have saved the staged
            // draft clear. Persist the exact restoration, surfacing CAS errors.
            QString restoreError;if(!m_store.save(m_sessions,&restoreError))conversationError(m_status->text()+"\nThe draft is retained here, but could not be saved: "+restoreError);
        }
        if(id==m_currentId){refreshAttachments();renderConversation();refreshAgents();}
        return;
    }
    if(accepted&&id==m_currentId&&m_composer->toPlainText()==submittedDraft){
        const QSignalBlocker blocker(m_composer);m_composer->clear();
    }
    if(id==m_currentId){refreshAttachments();renderConversation();refreshAgentControls();}
    refreshAgents();
}

void MainWindow::refreshAttachments(){if(m_attachments)m_attachments->setAttachments(current()?current()->draftAttachments:QJsonArray{});updateComposerState();}
void MainWindow::updateComposerState(){
    if(!m_send||!m_stop||!m_composer)return;
    const auto* session=current();const bool busy=session&&m_engines.value(session->id)&&m_engines.value(session->id)->isBusy();
    m_stop->setEnabled(busy);m_stop->setVisible(busy);
    const auto* host=session?m_toolHosts.value(session->id):nullptr;
    const bool undoing=(host&&host->mutationBusy())||(session&&m_mutationUndoAgent==session->id&&!m_mutationUndoId.isEmpty());
    const bool active=session&&!session->archived;
    m_models->setEnabled(active&&!busy&&!undoing);m_permissions->setEnabled(active&&!busy&&!undoing);m_workspace->setEnabled(active&&!busy&&!undoing);
    m_send->setToolTip(busy?"Add message to queue":"Send message");
    m_send->setEnabled(session&&!session->archived&&!undoing&&(!m_composer->toPlainText().trimmed().isEmpty()||!session->draftAttachments.isEmpty()));
}
void MainWindow::attachFiles(const QStringList& paths){
    auto* session=current();if(!session||paths.isEmpty())return;
    AttachmentStore store(m_store.directory());AttachmentMounts mounts;
    if(!session->workspace.isEmpty())mounts.insert("ws1",session->workspace);
    const auto previous=session->draftAttachments;QStringList errors;
    for(const auto& path:paths){
        if(session->draftAttachments.size()>=32){errors.append("A message can contain at most 32 attachments.");break;}
        QString error;const auto ref=store.importFile(session->id,path,mounts,&error);
        if(!error.isEmpty()){errors.append(QFileInfo(path).fileName()+": "+error);continue;}
        bool duplicate=false;for(const auto& value:session->draftAttachments)duplicate|=value.toObject()["path"]==ref.path;
        if(!duplicate)session->draftAttachments.append(ref.toJson());
    }
    if(!saveSessions())session->draftAttachments=previous;
    else if(!errors.isEmpty())m_status->setText(errors.join('\n'));
    refreshAttachments();
}
void MainWindow::attachImage(const QImage& image){
    auto* session=current();if(!session||image.isNull())return;
    if(session->draftAttachments.size()>=32){m_status->setText("A message can contain at most 32 attachments.");return;}
    if(qint64(image.width())*image.height()>64*1024*1024){m_status->setText("The pasted image exceeds the 64 megapixel limit.");return;}
    QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
    if(!image.save(&buffer,"PNG")){m_status->setText("The pasted image could not be encoded.");return;}
    QString error;const auto ref=AttachmentStore(m_store.directory()).write(session->id,"Pasted image.png",bytes,&error);
    if(!error.isEmpty()){m_status->setText(error);return;}
    const auto previous=session->draftAttachments;session->draftAttachments.append(ref.toJson());
    if(!saveSessions())session->draftAttachments=previous;
    refreshAttachments();
}
void MainWindow::submitPrompt(const QString& prompt){m_composer->setPlainText(prompt);send();}
void MainWindow::configureUtilities(UtilityService* service,const Conversation& session){
    ModelSelection selection{session.providerId,session.model,session.presetId,{},{}};
    const auto presets=m_settings->preference("modelPresets").toArray();const auto catalogs=m_settings->preference("modelCatalogs").toObject();service->setMainModels(selectedModelChain(selection,m_providers,presets,catalogs));
    QJsonObject activePreset;for(const auto& value:presets)if(value.toObject()["id"].toString()==session.presetId&&!session.presetId.isEmpty()){activePreset=value.toObject();break;}
    const auto titles=activePreset.isEmpty()?m_settings->preference("utilityModels.titleGeneration").toArray():activePreset["titleGeneration"].toArray();
    const auto compression=activePreset.isEmpty()?m_settings->preference("utilityModels.contextCompression").toArray():activePreset["contextCompression"].toArray();
    service->setTitleModels(resolveModelEntries(m_providers,titles,catalogs));service->setCompressionModels(resolveModelEntries(m_providers,compression,catalogs));
}
void MainWindow::generateTitle(const QString& id){
    auto* session=find(id);if(!session||session->titleLocked||session->autoTitleAttempted||session->messages.isEmpty())return;
    const auto last=session->messages.last().toObject();if(last["role"].toString()!="assistant"||last["content"].toString().trimmed().isEmpty())return;
    session->autoTitleAttempted=true;const auto originalTitle=session->title;
    auto* utility=new UtilityService(this);configureUtilities(utility,*session);m_utilities.insert(id,utility);
    connect(utility,&UtilityService::titleReady,this,[this,id,originalTitle,utility](const QString& title){
        if(m_utilities.value(id)!=utility)return;
        m_utilities.remove(id);
        if(auto* s=find(id);s&&!s->titleLocked&&s->title==originalTitle){s->title=title;saveSessions();refreshAgents();}
        utility->deleteLater();
    });
    connect(utility,&UtilityService::failed,this,[this,id,utility](const QString&){if(m_utilities.value(id)==utility)m_utilities.remove(id);utility->deleteLater();});
    utility->generateTitle(session->messages);
}
void MainWindow::compressContext(){
    auto* session=current();if(!session)return;const auto id=session->id;auto* engine=engineFor(id);
    if(engine->isBusy()){m_status->setText("Wait for the agent before compressing context.");return;}
    const auto snapshot=engine->messages();const int boundary=UtilityService::compressionBoundary(snapshot);
    if(boundary<0){m_status->setText("This conversation is too short to compress while preserving recent turns.");return;}
    if(auto existing=m_utilities.take(id)){existing->cancel();existing->deleteLater();}
    auto* utility=new UtilityService(this);configureUtilities(utility,*session);m_utilities.insert(id,utility);
    connect(utility,&UtilityService::compressionReady,this,[this,id,engine,snapshot,boundary,utility](const QString& briefing){
        if(m_utilities.value(id)!=utility)return;
        m_utilities.remove(id);
        if(!engine->isBusy()&&engine->messages()==snapshot&&engine->applyCompression(boundary,briefing)){
            if(auto* s=find(id))s->messages=engine->messages();
            saveSessions();
            if(id==m_currentId){m_status->setText("Context compressed. The complete conversation remains saved.");renderConversation();}
        }else if(id==m_currentId)m_status->setText("The conversation changed during compression. Its context was preserved.");
        utility->deleteLater();
    });
    connect(utility,&UtilityService::failed,this,[this,id,utility](const QString& error){if(m_utilities.value(id)==utility)m_utilities.remove(id);if(id==m_currentId)m_status->setText("Context compression failed: "+error);utility->deleteLater();});
    QJsonArray prefix;for(int i=0;i<boundary;++i)prefix.append(snapshot[i]);
    if(m_composer->toPlainText().trimmed()=="/compact")m_composer->clear();
    m_status->setText("Compressing context…");utility->compressHistory(prefix);
}
void MainWindow::showProviders(bool add,const QString& providerId){
    const QPointer<MainWindow> self(this);
    // Reload before reporting an outcome: reload clears the settings status.
    const auto finish=[self](const QString& error={}){
        if(!self)return;
        self->m_settings->reload();
        if(!self||error.isEmpty())return;
        self->m_status->setText(error);self->m_settings->reportProviderImportResult(error,true);
    };
    QString error;const auto latest=m_providerRepository.load(&error);
    if(!error.isEmpty()){finish(error);return;}
    m_providers=latest;
    if(!providerId.isEmpty()&&std::none_of(m_providers.cbegin(),m_providers.cend(),[&](const ProviderConfig& provider){return provider.id==providerId;})){
        finish(QStringLiteral("This provider is no longer configured. The provider list has been refreshed."));return;
    }
    QPointer<ProviderDialog> dialog=new ProviderDialog(m_providers,m_settings->preference("modelCatalogs").toObject(),this);
    if(!providerId.isEmpty()&&!dialog->selectProviderId(providerId)){
        if(dialog)dialog->deleteLater();
        finish(QStringLiteral("The selected provider could not be opened."));return;
    }
    if(!self||!dialog)return;
    if(add||m_providers.isEmpty())QTimer::singleShot(0,dialog.data(),&ProviderDialog::startAddProvider);
    const auto result=dialog->exec();
    if(!self||!dialog)return;
    if(result!=QDialog::Accepted){dialog->deleteLater();finish();return;}
    const auto providers=dialog->providers();const auto catalogs=dialog->modelCatalogs();dialog->deleteLater();
    if(!m_providerRepository.save(providers,&error)){finish(error);return;}
    m_providers=providers;
    const bool savedCatalog=m_settings->setPreference("modelCatalogs",catalogs);
    if(!self)return;
    finish(savedCatalog?QString():QStringLiteral("Provider saved, but the discovered catalog could not be saved."));
    if(self)refreshModels();
}
void MainWindow::showSettings(){m_settings->setContextWorkspace(current()?current()->workspace:QString());m_settings->reload();m_pages->setCurrentWidget(m_settings);}
void MainWindow::applyPreferences(){
    if(!m_settings)return;
    refreshModels();
    if(!qApp->property("adeBaseStyleSheet").isValid())qApp->setProperty("adeBaseStyleSheet",qApp->styleSheet());
    QString style=qApp->property("adeBaseStyleSheet").toString();
    const auto appearance=m_settings->preference("personalization.appearance").toString("system");
    const bool dark=appearance=="dark"||(appearance=="system"&&qApp->styleHints()->colorScheme()==Qt::ColorScheme::Dark);
    if(!qApp->property("adeBasePalette").isValid())qApp->setProperty("adeBasePalette",QVariant::fromValue(qApp->palette()));
    auto palette=qvariant_cast<QPalette>(qApp->property("adeBasePalette"));
    if(dark){palette.setColor(QPalette::Window,QColor("#21242b"));palette.setColor(QPalette::Base,QColor("#21242b"));palette.setColor(QPalette::AlternateBase,QColor("#252a33"));palette.setColor(QPalette::Text,QColor("#e8e8ed"));palette.setColor(QPalette::WindowText,QColor("#e8e8ed"));palette.setColor(QPalette::Button,QColor("#2b303a"));palette.setColor(QPalette::ButtonText,QColor("#e8e8ed"));palette.setColor(QPalette::PlaceholderText,QColor("#a6abb4"));}
    else {palette.setColor(QPalette::Window,QColor("#eff4fa"));palette.setColor(QPalette::Base,QColor("#fdfdfd"));palette.setColor(QPalette::AlternateBase,QColor("#f2f4f6"));palette.setColor(QPalette::Text,QColor("#292a2d"));palette.setColor(QPalette::WindowText,QColor("#292a2d"));palette.setColor(QPalette::Button,QColor("#f2f2f2"));palette.setColor(QPalette::ButtonText,QColor("#292a2d"));palette.setColor(QPalette::PlaceholderText,QColor("#64676c"));}
    qApp->setPalette(palette);
    if(dark){const QList<QPair<QString,QString>> colors={{"#292a2d","#e8e8ed"},{"#eff4fa","#1d222c"},{"#fdfdfd","#21242b"},{"#fcfcfc","#21242b"},{"#fafafa","#252a33"},{"#e2e8ee","#343d4b"},{"#e8edf2","#303846"},{"#e5ebf1","#2b3340"},{"#f2f2f2","#2b303a"},{"#e6e6e6","#3d424b"},{"#e4e6e8","#3d424b"},{"#e4e5e7","#3d424b"},{"#64676c","#c4c8d0"},{"#6a6d72","#bec3cb"},{"#e1e1e1","#393d45"},{"#eeeff1","#313743"},{"#fff","#252a33"},{"#22262b","#eceef4"}};for(const auto& pair:colors)style.replace(pair.first,pair.second,Qt::CaseInsensitive);}
    const double scale=m_settings->preference("personalization.uiZoom").toInt(100)/100.0;
    auto bodyFont=qApp->font();bodyFont.setPixelSize(qMax(1,qRound(13*scale)));qApp->setFont(bodyFont);
    if(scale!=1.0){QRegularExpression expression("([0-9]+)px");auto matches=expression.globalMatch(style);QList<QPair<qsizetype,qsizetype>> positions;QStringList replacements;while(matches.hasNext()){auto match=matches.next();positions.append({match.capturedStart(),match.capturedLength()});replacements.append(QString::number(qMax(1,qRound(match.captured(1).toInt()*scale)))+"px");}for(int i=positions.size()-1;i>=0;--i)style.replace(positions[i].first,positions[i].second,replacements[i]);}
    const QMap<QString,QString> accents={{"blue","#4778d7"},{"violet","#855ad2"},{"green","#348a67"},{"amber","#ac7a24"},{"neutral","#777f8c"}};
    const auto accent=accents.value(m_settings->preference("personalization.theme").toString(),"#4778d7");
    style.replace("#7193ff",accent).replace("#4b75f8",QColor(accent).darker(110).name());
    qApp->setStyleSheet(style);m_sidebarSplitter->setUiScale(scale);
    auto transcriptFont=bodyFont;transcriptFont.setPixelSize(qMax(1,qRound(14*scale)));m_transcript->setFont(transcriptFont);
    m_chat->setUiScale(scale);m_splitter->setUiScale(scale);
    m_composer->setUiScale(scale);m_models->setMaximumWidth(qRound(270*scale));
    for(const auto& name:{"sendButton","stopButton","attachFilesButton"})if(auto* action=findChild<QToolButton*>(name)){action->setFixedSize(qRound(32*scale),qRound(32*scale));action->setIconSize(QSize(qRound(16*scale),qRound(16*scale)));}
    if(auto* rail=findChild<QWidget*>("composerActions")){rail->setFixedWidth(qRound(32*scale));rail->layout()->setSpacing(qRound(4*scale));}
    m_composerBox->layout()->setContentsMargins(qRound(8*scale),qRound(8*scale),qRound(8*scale),qRound(8*scale));m_composerBox->layout()->setSpacing(qRound(4*scale));
    if(auto* selectors=findChild<QWidget*>("composerSelectors"))selectors->layout()->setSpacing(qRound(8*scale));
    for(auto* browser:findChildren<BrowserWidget*>())if(!browser->property("miniAppBrowser").toBool()&&browser->property("adeOwnerAgentId").toString().isEmpty()){browser->setPermissionDefaults(m_settings->preference("browsing.permissions").toObject());browser->setPermissionExceptions(m_settings->preference("browsing.permissionExceptions").toObject());}
    for(auto* terminal:findChildren<TerminalWidget*>()){terminal->setDarkTheme(dark);terminal->setZoomPercent(m_settings->preference("personalization.uiZoom").toInt(100));}
    for(auto* editor:findChildren<EditorWidget*>())editor->setDarkTheme(dark);
    if(m_git)m_git->setDarkTheme(dark);
    for(auto* pane:findChildren<WorktreeSettingsPane*>())pane->setDarkTheme(dark);
    const QPointer<MainWindow> self(this);refreshPluginConfiguration();if(!self)return;
    if(current())renderConversation();
    updateAppThemes();if(!self)return;refreshAgents();refreshCompletionContext();refreshSideChats();
}
void MainWindow::importKeys(const QString& path){
    const auto report=[this](const QString& message,bool failed){
        m_status->setText(message);
        if(m_settings)m_settings->reportProviderImportResult(message,failed);
    };
    QString error;auto providers=m_providerRepository.load(&error);
    if(!error.isEmpty()){report(error,true);return;}
    const auto imported=ProviderStore::importKeyFile(path,&error);
    if(imported.isEmpty()){report(error.isEmpty()?"No provider keys found.":error,true);return;}
    const auto notice=error;
    providers=ProviderStore::mergeImported(providers,imported);
    if(!m_providerRepository.save(providers,&error)){report(error,true);return;}
    m_providers=providers;refreshModels();
    const QPointer<MainWindow> self(this);
    if(m_settings)m_settings->reload();
    if(!self)return;
    report(QString("Imported %1 provider key(s). ").arg(imported.size())+notice,false);
}
void MainWindow::chooseWorkspace(){const auto path=QFileDialog::getExistingDirectory(this,"Connect workspace",current()?current()->workspace:QString());if(!path.isEmpty())connectWorkspace(path);}
void MainWindow::connectWorkspace(const QString& path){
    auto* s=current();if(!s||!QFileInfo(path).isDir())return;
    if(s->archived){conversationError("Restore this chat before changing its workspace.");return;}
    if(gitPathBusy(path)||gitPathBusy(s->workspace)){m_status->setText("Wait for the workspace operation to finish before changing workspaces.");return;}
    if(m_engines.contains(s->id)&&m_engines[s->id]->isBusy()){m_status->setText("Stop the agent before changing workspace.");return;}
    const auto canonical=GitService::canonicalPath(path);if(canonical.isEmpty()){m_status->setText("The workspace path could not be resolved.");return;}
    const auto previous=s->workspace;const auto previousAttachments=s->draftAttachments;
    if(GitService::pathKey(previous)!=GitService::pathKey(canonical)){
        // A draft's selected file must not silently follow ws1 to a new root.
        AttachmentStore store(m_store.directory());const AttachmentMounts mounts{{"ws1",previous},{"w1",previous}};QJsonArray retained;
        for(const auto& entry:s->draftAttachments){
            QString error;const auto ref=AttachmentRef::fromJson(entry.toObject(),&error);
            if(ref.path.startsWith("att/")){retained.append(entry);continue;}
            const auto bytes=store.read(s->id,ref,mounts,&error);
            if(!error.isEmpty()){m_status->setText("Keep or remove the unreadable draft attachment before changing workspace: "+error);return;}
            const auto copy=store.write(s->id,ref.originalFileName.isEmpty()?QFileInfo(ref.path).fileName():ref.originalFileName,bytes,&error);
            if(!error.isEmpty()){m_status->setText("Could not preserve the draft attachment: "+error);return;}
            retained.append(copy.toJson());
        }
        s->draftAttachments=retained;
    }
    s->workspace=canonical;
    if(!saveSessions()){s->workspace=previous;s->draftAttachments=previousAttachments;return;}
    m_liveConversationOwners.insert(s->id);
    if(auto* host=m_toolHosts.value(s->id)){QString error;if(!host->setWorkspace(s->workspace,&error)){s->workspace=previous;s->draftAttachments=previousAttachments;saveSessions();m_status->setText(error);return;}}
    if(m_engines.contains(s->id))m_engines[s->id]->setWorkspace(s->workspace);
    m_files->setModel(m_fileModel);m_files->setRootIndex(m_fileModel->setRootPath(s->workspace));m_workspace->setText(QFileInfo(s->workspace).fileName());static_cast<ChevronToolButton*>(m_workspace)->setChevronVisible(!s->workspace.isEmpty());m_workspace->setToolTip(s->workspace);refreshAttachments();updateGitContext();refreshWorkbench();refreshCompletionContext();refreshFileChanges();
}
void MainWindow::addBrowserTab(const QUrl& url,const QString& restoredId,const QString& restoredTitle){
    const QPointer<MainWindow> self(this);const QPointer<BrowserToolBridge> bridge(m_browserBridge);
    // Registration and tab selection emit public signals. Own the provisional
    // widgets before those callbacks, and stop if they dispose this window.
    auto* page=new QWidget(m_tabs);const QPointer<QWidget> preview(page);
    QUrl target=url;if(restoredId.isEmpty()&&url==QUrl("about:blank")&&m_settings){const auto custom=m_settings->preference("browsing.newTabUrl").toString();if(!custom.isEmpty())target=QUrl(custom);}
    page->setProperty("browserSessionId",restoredId.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):restoredId);
    page->setProperty("browserSessionUrl",BrowserSessionStore::restorableUrl(target));
    page->setProperty("browserSessionTitle",restoredTitle);
    auto* layout=new QVBoxLayout(page);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* navWidget=new QWidget;navWidget->setObjectName("browserNavigation");auto* nav=new QHBoxLayout(navWidget);nav->setContentsMargins(5,3,5,3);
    auto* back=button("back","Back");auto* forward=button("forward","Forward");auto* reload=button("reload","Reload");auto* address=new QLineEdit;address->setObjectName("browserAddress");address->setPlaceholderText("Search or enter address");
    nav->addWidget(back);nav->addWidget(forward);nav->addWidget(reload);nav->addWidget(address,1);layout->addWidget(navWidget);
    auto* browser=new BrowserWidget(page);const QPointer<BrowserWidget> browserGuard(browser);
    const auto owned=[self,bridge,preview,browserGuard]{return self&&bridge&&preview&&browserGuard;};
    browser->setProfileDirectory(m_store.directory()+"/browser");const auto tabId=bridge->registerTab(browser);
    if(!owned()){if(preview)preview->deleteLater();return;}
    page->setProperty("browserTabId",tabId);if(!owned())return;
    auto* viewport=configureBrowserTools(page,browser,nav);if(!owned())return;
    layout->addWidget(viewport,1);if(!owned())return;
    int index=-1;
    {
        // Qt's insertTab continues touching its tab bar after currentChanged.
        // Publish the final selection only after that internal frame returns.
        const QSignalBlocker currentNotifications(m_tabs);
        index=m_tabs->addTab(page,icon("globe"),restoredTitle.isEmpty()?QString("New tab"):restoredTitle.left(28));
        m_tabs->setCurrentIndex(index);
    }
    if(!owned())return;
    emit m_tabs->currentChanged(index);if(!owned())return;
    revealContent();if(!owned())return;
    address->setFocus();if(!owned())return;
    connect(address,&QLineEdit::returnPressed,page,[this,browser,address,page]{QString text=address->text().trimmed();if(text.isEmpty())return;QUrl target;if(text.contains(' ')||(!text.contains('.')&&!text.contains(':')&&text!="localhost")){QString searchTemplate="https://www.google.com/search?q=%s";if(m_settings){const auto selected=m_settings->preference("browsing.defaultEngineId").toString();for(const auto& value:m_settings->preference("browsing.searchEngines").toArray()){const auto engine=value.toObject();if(engine["id"].toString()==selected){searchTemplate=engine["url"].toString();break;}}}target=QUrl(searchTemplate.replace("%s",QString::fromUtf8(QUrl::toPercentEncoding(text))));}else target=QUrl::fromUserInput(text);if(target.scheme()=="https"||target.scheme()=="http"||target.scheme()=="about"){page->setProperty("browserSessionUrl",BrowserSessionStore::restorableUrl(target));scheduleBrowserSessionSave();browser->navigate(target);}});
    connect(back,&QToolButton::clicked,browser,&BrowserWidget::back);connect(forward,&QToolButton::clicked,browser,&BrowserWidget::forward);connect(reload,&QToolButton::clicked,browser,&BrowserWidget::reload);
    connect(browser,&BrowserWidget::urlChanged,address,[this,page,address](const QUrl& value){page->setProperty("browserSessionUrl",BrowserSessionStore::restorableUrl(value));scheduleBrowserSessionSave();address->setText(value==QUrl("about:blank")?QString():value.toDisplayString());});
    connect(browser,&BrowserWidget::titleChanged,page,[this,page](const QString& title){page->setProperty("browserSessionTitle",title.left(1024));scheduleBrowserSessionSave();auto i=m_tabs->indexOf(page);if(i>=0)m_tabs->setTabText(i,title.isEmpty()?"New tab":title.left(28));});
    connect(browser,&BrowserWidget::errorOccurred,page,[this](const QString& message){m_status->setText(message);});
    if(m_downloads)m_downloads->attachBrowser(browser);
    connect(browser,&BrowserWidget::fullscreenChanged,this,[this,browser](bool enabled){setBrowserFullscreen(browser,enabled);});
    connect(browser,&BrowserWidget::browsingDataCleared,this,[this](const QString& id,bool success,const QString& error){if(!m_dataClearId.isEmpty()&&id==m_dataClearId)finishBrowsingDataClear(success,error);});
    connect(browser,&QObject::destroyed,this,[this,browser]{if(m_dataClearBrowser==browser||(!m_dataClearId.isEmpty()&&m_dataClearBrowser.isNull()))finishBrowsingDataClear(false,"The browser closed while clearing data.");if(!m_fullscreenHidden.isEmpty()&&m_fullscreenBrowser.isNull())setBrowserFullscreen(nullptr,false);});
    if(m_settings){browser->setPermissionDefaults(m_settings->preference("browsing.permissions").toObject());browser->setPermissionExceptions(m_settings->preference("browsing.permissionExceptions").toObject());}
    connect(browser,&BrowserWidget::permissionRequested,page,[this,browser](const QString& requestId,const QUrl& origin,const QString& permission,bool){
        const QMap<QString,QString> names={{"media","camera or microphone"},{"geolocation","location"},{"notifications","notifications"},{"clipboard-read","clipboard"},{"fullscreen","fullscreen"},{"downloads","multiple downloads"}};
        auto* box=new QMessageBox(QMessageBox::Question,"Website permission",origin.toDisplayString()+" wants access to "+names.value(permission,permission)+".",QMessageBox::Yes|QMessageBox::No,this);
        box->setAttribute(Qt::WA_DeleteOnClose);box->setDefaultButton(QMessageBox::No);box->button(QMessageBox::Yes)->setText("Allow");box->button(QMessageBox::No)->setText("Block");
        const QPointer<BrowserWidget> safeBrowser(browser);
        connect(box,&QMessageBox::finished,this,[safeBrowser,requestId](int choice){if(safeBrowser)safeBrowser->resolvePermission(requestId,choice==QMessageBox::Yes);});
        connect(browser,&BrowserWidget::permissionRequestFinished,box,[box,requestId](const QString& settled,bool){if(settled==requestId)box->reject();});
        connect(browser,&QObject::destroyed,box,&QDialog::reject);box->open();
    });
    connect(browser,&BrowserWidget::loadingChanged,page,[this,browser,page](bool loading){if(!loading){const auto index=m_tabs->indexOf(page);recordBrowserHistory(browser->currentUrl(),index>=0?m_tabs->tabText(index):QString());}});
    connect(browser,&BrowserWidget::titleChanged,page,[this,browser](const QString& title){recordBrowserHistory(browser->currentUrl(),title);});
    address->setText(target==QUrl("about:blank")?QString():target.toDisplayString());if(!owned())return;scheduleBrowserSessionSave();browser->navigate(target);
}
void MainWindow::recordBrowserHistory(const QUrl& url,const QString& title){
    if(url.scheme()!="https"&&url.scheme()!="http")return;
    const QString path=m_store.directory()+"/browsing-history.json";QFile file(path);QJsonArray entries;
    if(file.exists()){if(!file.open(QIODevice::ReadOnly)||file.size()>4*1024*1024)return;QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);if(parse.error!=QJsonParseError::NoError)return;entries=doc.isArray()?doc.array():doc.object()["entries"].toArray();}
    QUrl clean=url;clean.setUserInfo(QString());const QString address=clean.toString();const auto now=QDateTime::currentMSecsSinceEpoch();
    if(!entries.isEmpty()&&entries.first().toObject()["url"].toString()==address){auto entry=entries.first().toObject();entry["title"]=title;entries.replace(0,entry);}else entries.prepend(QJsonObject{{"url",address},{"title",title},{"visitedAt",now}});
    while(entries.size()>5000)entries.removeLast();
    QSaveFile output(path);if(output.open(QIODevice::WriteOnly)){const auto data=QJsonDocument(entries).toJson();if(output.write(data)==data.size())output.commit();}
}
void MainWindow::openFile(const QString& path){
    const auto canonical=QFileInfo(path).canonicalFilePath();
    for(int i=0;i<m_tabs->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(m_tabs->widget(i));editor&&!canonical.isEmpty()&&QFileInfo(editor->filePath()).canonicalFilePath()==canonical){m_tabs->setCurrentIndex(i);revealContent();return;}
    auto* editor=new EditorWidget;editor->setDarkTheme(qApp->palette().color(QPalette::Base).lightness()<128);QString error;
    if(!editor->openFile(path,&error)){m_status->setText(error);delete editor;return;}
    editor->setProperty("filePath",editor->filePath());
    connect(editor,&EditorWidget::modifiedChanged,this,[this,editor](bool modified){const auto index=m_tabs->indexOf(editor);if(index>=0)m_tabs->setTabText(index,QFileInfo(editor->filePath()).fileName()+(modified?" •":""));});
    connect(editor,&EditorWidget::fileSaved,this,[this](const QString& file){m_status->setText("Saved "+QFileInfo(file).fileName());refreshWorkspaceChanges();});
    connect(editor,&EditorWidget::errorOccurred,this,[this](const QString& message){m_status->setText(message);});
    m_tabs->setCurrentIndex(m_tabs->addTab(editor,QFileInfo(path).fileName()));revealContent();
}
MainWindow::~MainWindow(){
    // This application-wide filter invokes guards that read our page widgets.
    // Retire it while those widgets are intact, before QObject deletes children.
    auto* agentCycle = m_agentCycle;m_agentCycle = nullptr;delete agentCycle;
    m_browserSessionInitialized=false;if(m_browserSessionTimer)m_browserSessionTimer->stop();
    // Child destruction occurs after derived members are destroyed. Browser
    // teardown signals must not access those members during QObject cleanup.
    for(auto* child:findChildren<QObject*>())disconnect(child,nullptr,this,nullptr);
    // Some browser connections are scoped to the tab page and capture the host.
    // Stop managers first, then tear tabs down while derived members live.
    delete m_downloads;m_downloads=nullptr;
    delete m_worktreeSettings;m_worktreeSettings=nullptr;
    delete m_worktreeCleanup;m_worktreeCleanup=nullptr;
    delete m_git;m_git=nullptr;delete m_worktreeSetup;m_worktreeSetup=nullptr;
    // Keep QObject::destroyed observers intact (notably Qt's stylesheet cache).
    // Tear browsers down now, while callbacks scoped to their pages can still
    // safely reference MainWindow members; QObject base teardown is too late.
    // MiniAppPage owns and explicitly destroys its browser; destroy the page
    // first so its raw owned pointer cannot outlive a separately deleted child.
    QList<QPointer<MiniAppPage>> apps;for(auto* app:findChildren<MiniAppPage*>())apps.append(app);
    for(const auto& app:apps)if(app)delete app;
    QList<QPointer<BrowserWidget>> browsers;for(auto* browser:findChildren<BrowserWidget*>())browsers.append(browser);
    for(const auto& browser:browsers)if(browser)delete browser;
}

void MainWindow::showTerminal(){
    auto* terminal=new TerminalWidget;
    terminal->setObjectName("workspaceTerminal");
    const auto appearance=m_settings->preference("personalization.appearance").toString("system");
    terminal->setDarkTheme(appearance=="dark"||(appearance=="system"&&qApp->styleHints()->colorScheme()==Qt::ColorScheme::Dark));
    terminal->setZoomPercent(m_settings->preference("personalization.uiZoom").toInt(100));
    connect(terminal,&TerminalWidget::titleChanged,this,[this,terminal](const QString& title){const int index=m_tabs->indexOf(terminal);if(index>=0)m_tabs->setTabText(index,title.isEmpty()?"Terminal":title.left(32));});
    connect(terminal,&TerminalWidget::workingDirectoryChanged,this,[this,terminal](const QString& path){const int index=m_tabs->indexOf(terminal);if(index>=0)m_tabs->setTabToolTip(index,path);});
    connect(terminal,&TerminalWidget::errorOccurred,this,[this](const QString& error){m_status->setText(error);});
    connect(terminal,&TerminalWidget::exited,this,[this,terminal](quint32 code){const int index=m_tabs->indexOf(terminal);if(index>=0)m_tabs->setTabText(index,QString("Terminal · exited %1").arg(code));});
    m_tabs->setCurrentIndex(m_tabs->addTab(terminal,icon("terminal"),"Terminal"));revealContent();
    const auto workspace=current()?current()->workspace:QString();QString error;
    if(!terminal->start(workspace.isEmpty()?QDir::homePath():workspace,&error))m_status->setText(error);
    terminal->setFocus();
}

void MainWindow::showDownloads(){
    m_pages->setCurrentIndex(0);int index=m_tabs->indexOf(m_downloads);
    if(index<0)index=m_tabs->addTab(m_downloads,icon("download"),"Downloads");
    m_tabs->setCurrentIndex(index);m_downloads->show();revealContent();
}

void MainWindow::refreshPluginConfiguration(){
    if(!m_settings)return;
    if(m_toolHosts.isEmpty())return;
    QString contextError;const auto context=PluginCatalog::context(m_store.directory(),m_settings->preferences(),&contextError);
    QList<QPointer<AgentToolHost>> hosts;for(auto* host:m_toolHosts)hosts.append(host);
    const QPointer<MainWindow> self(this);
    const auto directory=m_store.directory();const auto preferences=m_settings->preferences();
    for(const auto& host:hosts){
        if(!host)continue;
        const auto agentId=host->agentId();QString error;
        const bool configured=m_pluginCatalog.configureAgent(*host->network(),agentId,directory,preferences,&error);
        if(!self)return;
        if(!configured)m_status->setText("Plugin configuration: "+error);
        if(host)configureAppRoots(host,contextError.isEmpty()?context:PluginContext{});
        if(!self)return;
    }
    if(!contextError.isEmpty())m_status->setText("Plugin context: "+contextError);
}

void MainWindow::setBrowserFullscreen(BrowserWidget* browser,bool enabled){
    if(!enabled){
        if(browser&&m_fullscreenBrowser&&browser!=m_fullscreenBrowser)return;
        if(m_fullscreenHidden.isEmpty())return;
        if(m_fullscreenBrowser)m_fullscreenBrowser->executeScript("document.exitFullscreen().catch(()=>{})");
        m_fullscreenExit->setEnabled(false);
        for(const auto& widget:m_fullscreenHidden)if(widget)widget->show();
        m_fullscreenHidden.clear();m_fullscreenBrowser.clear();setWindowState(m_beforeFullscreenState);m_splitter->setWorkbenchFullscreen(false);m_sidebarSplitter->setWorkbenchFullscreen(false);return;
    }
    if(!browser||m_fullscreenBrowser==browser)return;
    QWidget* page=browser;
    while(page&&m_tabs->indexOf(page)<0)page=page->parentWidget();
    if(!page||m_tabs->currentWidget()!=page||m_pages->currentIndex()!=0){browser->executeScript("document.exitFullscreen().catch(()=>{})");return;}
    if(m_fullscreenBrowser)setBrowserFullscreen(m_fullscreenBrowser,false);
    m_beforeFullscreenState=windowState();m_fullscreenBrowser=browser;
    m_fullscreenExit->setEnabled(true);
    const QList<QWidget*> chrome={m_sidebar,m_chat,m_fileTreePanel,m_tabs->tabBar(),m_tabs->cornerWidget(),page->findChild<QWidget*>("browserNavigation")};
    for(auto* widget:chrome)if(widget&&!widget->isHidden()){m_fullscreenHidden.append(widget);widget->hide();}
    m_splitter->setWorkbenchFullscreen(true);m_sidebarSplitter->setWorkbenchFullscreen(true);showFullScreen();browser->setFocus();
}

void MainWindow::clearBrowsingData(const QJsonObject& options){
    if(!m_dataClearId.isEmpty()){m_settings->completeBrowsingDataClear(false,"A data-clearing operation is already running.");return;}
    QJsonObject browserOptions;
    for(const auto& name:{"history","cookies","cache","storage","downloads"})if(options[name].toBool())browserOptions[name]=true;
    BrowserWidget* selected=nullptr;
    if(!browserOptions.isEmpty()){
        for(auto* browser:findChildren<BrowserWidget*>())if(browser->isReady()&&!browser->property("miniAppBrowser").toBool()){selected=browser;break;}
        if(!selected){m_settings->completeBrowsingDataClear(false,"Open a browser tab and wait for it to load before clearing browser data.");return;}
    }
    m_dataClearId=QUuid::createUuid().toString(QUuid::WithoutBraces);m_dataClearOptions=options;m_dataClearBrowser=selected;
    if(selected)selected->clearBrowsingData(m_dataClearId,browserOptions);
    else finishBrowsingDataClear(true,{});
}

void MainWindow::finishBrowsingDataClear(bool success,const QString& error){
    if(m_dataClearId.isEmpty())return;
    const auto options=m_dataClearOptions;m_dataClearId.clear();m_dataClearOptions={};m_dataClearBrowser.clear();
    if(!success){m_settings->completeBrowsingDataClear(false,error);return;}
    QStringList failures;
    for(const auto& entry:QList<QPair<QString,QString>>{{"history","browsing-history.json"},{"pluginCredentials","plugin-credentials.json"}}){
        if(!options[entry.first].toBool())continue;
        const auto path=QDir(m_store.directory()).filePath(entry.second);
        if(QFile::exists(path)&&!QFile::remove(path))failures.append(entry.first);
    }
    if(options["permissionExceptions"].toBool()&&!m_settings->setPreference("browsing.permissionExceptions",QJsonObject{}))failures.append("site permissions");
    if(options["downloads"].toBool()&&!m_downloads->clearHistory())failures.append("download history");
    const QPointer<MainWindow> self(this);
    if(options["pluginCredentials"].toBool())refreshPluginConfiguration();
    if(!self)return;
    m_settings->completeBrowsingDataClear(failures.isEmpty(),failures.isEmpty()?QString():"Some selected data was cleared, but these items failed: "+failures.join(", "));
}
void MainWindow::closeEvent(QCloseEvent* event){
    bool discardSetupDrafts=false;
    if(m_worktreeSettings&&m_worktreeSettings->isMutating()){
        m_status->setText("Wait for the worktree operation to finish before closing.");event->ignore();return;
    }
    if(m_worktreeSettings&&m_worktreeSettings->hasUnsavedDrafts()){
        const auto choice=QMessageBox::question(this,"Unsaved setup scripts","Discard the unsaved worktree setup scripts and close?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel);
        if(choice!=QMessageBox::Discard){event->ignore();return;}
        // Retain drafts until all remaining close checks succeed.
        discardSetupDrafts=true;
    }
    for(int i=0;i<m_tabs->count();++i)if(auto* editor=qobject_cast<EditorWidget*>(m_tabs->widget(i));editor&&!editor->requestClose()){event->ignore();return;}
    for(auto* engine:std::as_const(m_engines))if(engine->isBusy())engine->cancel();
    if(auto* s=current())s->draft=m_composer->toPlainText();
    if(!saveSessions()){event->ignore();return;}
    saveBrowserSession();
    if(discardSetupDrafts)m_worktreeSettings->discardDrafts();
    for(auto utility:std::as_const(m_utilities))if(utility)utility->cancel();
    if(m_worktreeSetup)m_worktreeSetup->cancelAll();
    closeGitUsageTracking();
    QSettings settings(m_store.directory()+"/window.ini",QSettings::IniFormat);settings.setValue("geometry",saveGeometry());m_splitter->savePreferences();QMainWindow::closeEvent(event);
}
}
