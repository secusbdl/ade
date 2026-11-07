#pragma once
#include "SessionStore.h"
#include "ProviderRepository.h"
#include "core/ProviderConfig.h"
#include "plugins/PluginCatalog.h"
#include "links/TranscriptLinkResolver.h"
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QUuid>
#include <memory>
#include <functional>
class QComboBox; class QLabel; class QLineEdit; class QListWidget;
class QPlainTextEdit; class QSplitter; class QStackedWidget; class QTabWidget; class QTextBrowser;
class QToolButton; class QTreeView;
class QHBoxLayout;
class QScrollArea;
class QShortcut; class QTimer;
class QImage;
namespace ade {
class AgentEngine; class AgentToolHost; class BrowserWidget; class BrowserToolBridge; class Composer; class SettingsPage; class UtilityService;
class DownloadsPage;
class WorkspaceFileTreeModel;
class AttachmentStrip;
class SidebarSplitter;class SidebarNavigation;class WorkbenchSplitter;class ChatSurface;
class ConversationView;
class AgentCycleController;
class GitPanel;class WorktreeSetupRunner;class WorktreeSettingsController;enum class GitOperation;
class WorktreeCleanupController;
class ComposerCompletionController;
class CommandCenterController;
class FileChangesPanel;
class WorkspaceChangesController;class WorkspaceChangesView;class GitDiffTab;
class QuestionPanel;class MessageQueuePanel;
class SideChatPane;
class BrowserSessionStore;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QString dataDirectory,QWidget* parent=nullptr);
    ~MainWindow() override;
    void importKeys(const QString& path);
    void connectWorkspace(const QString& path);
    void submitPrompt(const QString& prompt);
    QString currentSessionId() const {return m_currentId;}
    QString dataDirectory() const {return m_store.directory();}
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void buildUi(); void newConversation(); void selectConversation(const QString& id);
    void showConversationMenu(const QPoint& point);
    void requestRenameConversation(const QString& id);
    void requestDeleteConversation(const QString& id);
    bool renameConversation(const QString& id,const QString& title);
    bool forkConversation(const QString& id);
    bool cloneConversation(const QString& id,bool sideChat);
    bool createSideChat(const QString& sourceId={});
    bool promoteSideChat(const QString& id);
    bool discardSideChat(const QString& id);
    void initializeSideChats();void refreshSideChats();void refreshSideChat(const QString& id);
    void openSideChat(const QString& id);void closeSideChatPane(const QString& id);void sendSideChat(const QString& id);
    QHash<QString,QPointer<SideChatPane>> m_sideChats;
    bool archiveConversation(const QString& id);
    bool restoreConversation(const QString& id);
    bool deleteConversation(const QString& id);
    bool commitConversationState(const QList<Conversation>& next,const QStringList& pendingCleanup);
    void disposeConversation(const QString& id);
    void selectAfterConversationRemoval(const QString& id);
    void retryConversationCleanup();
    void conversationError(const QString& message);
    void refreshAgents(); void refreshModels(); void restoreModelSelection(); void renderConversation(); bool saveSessions();
    void send();void showProviders(bool add=false,const QString& providerId={});void showSettings();void applyPreferences();void chooseWorkspace();void openFile(const QString& path);
    void addBrowserTab(const QUrl& url=QUrl("about:blank"),const QString& restoredId={},const QString& restoredTitle={}); void showTerminal();
    void initializeBrowserSession();void scheduleBrowserSessionSave();bool saveBrowserSession();
    std::unique_ptr<BrowserSessionStore> m_browserSessionStore;
    QTimer* m_browserSessionTimer=nullptr;
    QString m_lastActiveBrowserId;
    bool m_browserSessionInitialized=false,m_restoringBrowserSession=false;
    void showAgentShell(const QString& agentId,const QString& sessionId);
    QWidget* configureBrowserTools(QWidget* page,BrowserWidget* browser,QHBoxLayout* navigation);
    void attachBrowserContext(const QString& agentId,const QString& name,const QByteArray& bytes);
    void showDownloads();
    void initializeWorkbench();void refreshWorkbench();void revealContent();
    void refreshPluginConfiguration();
    void initializePluginConnections();
    void connectAppTabs(AgentToolHost* host);
    void beginTranscriptLinkTurn(const QString& id,bool acceptedUserMessage=false);
    QJsonObject forkTranscriptLinkProvenance(const QString& sourceId,const QString& targetId,const QString& workspace);
    void updateTranscriptLinkRows(const QString& id);
    void updateTranscriptLinkRows(const QString& id,const QJsonArray& rows);
    bool visibleTranscript(const QString& id,ConversationView* view,bool requireCurrentTab=true) const;
    void routeTranscriptLink(const QString& id,const QString& rowId,const QString& partId,const QUrl& url,ConversationView* target=nullptr);
    void provideTranscriptImage(quint64 requestId,const QString& id,const QString& rowId,const QString& partId,const QUrl& url,ConversationView* target=nullptr);
    QHash<QString,QSet<QString>> m_linkKnownRows;
    QHash<QString,QHash<QString,TranscriptLinkProvenance>> m_linkProvenance;
    QHash<QString,TranscriptLinkProvenance> m_linkTurnContext;
    QString m_linkBrowserSessionId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    void configureAppRoots(AgentToolHost* host,const PluginContext& context);
    QJsonObject appThemeVariables() const;
    void updateAppThemes();
    void initializeGit();void showGit();void updateGitContext();
    void updateGitUsageTracking();void closeGitUsageTracking();void reviewUnusedWorktrees();
    void configureWorktreeSettingsPage();
    bool prepareGitMutation(const QString& path,GitOperation operation);
    bool gitPathBusy(const QString& path) const;
    void attachFiles(const QStringList& paths);
    void attachImage(const QImage& image);
    void refreshAttachments();
    void updateComposerState();
    void initializeCompletions();
    void initializeComposerCompletions(Composer* composer,const QString& fixedChat);
    QHash<QString,QPointer<ComposerCompletionController>> m_sideCompletionControllers;
    void initializeCommandCenter();
    void initializeNavigationShortcuts();
    bool refreshAgentCycle();
    void openCommandCenter(const QString& mode="global",bool searchContents=false);
    QPointer<CommandCenterController> m_commandCenterController;
    void refreshCompletionContext();
    bool configureTurnContext(const QString& id,const QString& userMessage,QString* error=nullptr);
    QPointer<ComposerCompletionController> m_completionController;
    void showFileChanges();void refreshFileChanges();
    void initializeWorkspaceChanges();void refreshWorkspaceChanges();
    void showWorkspaceChanges(const QString& workspace,const QString& ownerChat={});
    void openWorkspaceDiff(const QString& workspace,const QString& path,bool staged,const QString& oldPath={});
    void openWorkspaceChangedFile(const QString& workspace,const QString& relativePath,std::function<bool()> stillValid={},const QString& ownerChat={});
    quint64 m_changedFileOpenEpoch=0;
    QSet<QString> m_pendingMutationHydration;
    QPointer<WorkspaceChangesController> m_workspaceChanges;
    QPointer<WorkspaceChangesView> m_workspaceChangesView;
    void connectMutationControls(const QString& id,AgentToolHost* host);
    void connectTurnFileControls(ConversationView* view);
    void undoFileChanges(const QString& id,const QStringList& mutationIds);
    void recoverFileChanges(const QString& id);
    QPointer<FileChangesPanel> m_fileChanges;
    QString m_mutationUndoAgent,m_mutationUndoId,m_fileChangesAgent;
    void initializeAgentControls();
    void connectAgentControls(const QString& id,AgentEngine* engine);
    void refreshAgentControls();
    QuestionPanel* m_questionPanel=nullptr;
    MessageQueuePanel* m_queuePanel=nullptr;
    QScrollArea* m_agentControlsScroll=nullptr;
    QString m_questionMessageToken;
    void recordBrowserHistory(const QUrl& url,const QString& title);
    void clearBrowsingData(const QJsonObject& options);
    void finishBrowsingDataClear(bool success,const QString& error);
    void setBrowserFullscreen(BrowserWidget* browser,bool enabled);
    void generateTitle(const QString& id);void compressContext();void configureUtilities(UtilityService* service,const Conversation& session);
    Conversation* current(); Conversation* find(const QString& id);
    AgentEngine* engineFor(const QString& id);
    SessionStore m_store; ProviderRepository m_providerRepository; QList<Conversation> m_sessions; QList<ProviderConfig> m_providers;
    QHash<QString,AgentEngine*> m_engines;
    QString m_currentId;
    QWidget* m_sidebar;ChatSurface* m_chat;
    QWidget* m_fileTreePanel=nullptr;
    SidebarSplitter* m_sidebarSplitter=nullptr;
    SidebarNavigation* m_sidebarNavigation=nullptr;
    AgentCycleController* m_agentCycle=nullptr;
    QToolButton* m_sidebarReopen=nullptr;
    QWidget* m_composerBox=nullptr;
    QListWidget* m_agents;QLineEdit* m_search;
    ConversationView* m_transcript;QTimer* m_transcriptRefresh=nullptr;Composer* m_composer;QComboBox *m_models,*m_permissions;
    QToolButton *m_send,*m_stop,*m_workspace;
    QLabel* m_status;WorkbenchSplitter* m_splitter;QTabWidget* m_tabs;QTreeView* m_files;
    WorkspaceFileTreeModel* m_fileModel;
    QStackedWidget* m_pages;
    SettingsPage* m_settings=nullptr;
    BrowserToolBridge* m_browserBridge=nullptr;
    QHash<QString,AgentToolHost*> m_toolHosts;
    QHash<QString,QPointer<UtilityService>> m_utilities;
    QString m_dataClearId;
    QJsonObject m_dataClearOptions;
    QPointer<BrowserWidget> m_dataClearBrowser;
    QPointer<BrowserWidget> m_fullscreenBrowser;
    QList<QPointer<QWidget>> m_fullscreenHidden;
    Qt::WindowStates m_beforeFullscreenState;
    QShortcut* m_fullscreenExit=nullptr;
    DownloadsPage* m_downloads=nullptr;
    AttachmentStrip* m_attachments=nullptr;
    GitPanel* m_git=nullptr;
    WorktreeSetupRunner* m_worktreeSetup=nullptr;
    WorktreeSettingsController* m_worktreeSettings=nullptr;
    WorktreeCleanupController* m_worktreeCleanup=nullptr;
    QSet<QString> m_gitMutatingPaths,m_worktreesRemoving,m_worktreeSetups;
    QSet<QString> m_repositoryWatchOwners;
    QSet<QString> m_liveConversationOwners;
    QHash<QString,QStringList> m_removalAffectedSessions;
    PluginCatalog m_pluginCatalog;
};
}
