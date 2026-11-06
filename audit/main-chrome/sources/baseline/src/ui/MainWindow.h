#pragma once
#include "SessionStore.h"
#include "ProviderRepository.h"
#include "core/ProviderConfig.h"
#include "plugins/PluginCatalog.h"
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QSet>
class QComboBox; class QFileSystemModel; class QLabel; class QLineEdit; class QListWidget;
class QPlainTextEdit; class QSplitter; class QStackedWidget; class QTabWidget; class QTextBrowser;
class QToolButton; class QTreeView;
class QShortcut;
class QImage;
namespace ade {
class AgentEngine; class AgentToolHost; class BrowserWidget; class BrowserToolBridge; class Composer; class SettingsPage; class UtilityService;
class DownloadsPage;
class AttachmentStrip;
class GitPanel;class WorktreeSetupRunner;class WorktreeSettingsController;enum class GitOperation;
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
    bool archiveConversation(const QString& id);
    bool restoreConversation(const QString& id);
    bool deleteConversation(const QString& id);
    bool commitConversationState(const QList<Conversation>& next,const QStringList& pendingCleanup);
    void disposeConversation(const QString& id);
    void selectAfterConversationRemoval(const QString& id);
    void retryConversationCleanup();
    void conversationError(const QString& message);
    void refreshAgents(); void refreshModels(); void restoreModelSelection(); void renderConversation(); bool saveSessions();
    void send();void showProviders();void showSettings();void applyPreferences();void chooseWorkspace();void openFile(const QString& path);
    void addBrowserTab(const QUrl& url=QUrl("about:blank")); void showTerminal();
    void showDownloads();
    void refreshPluginConfiguration();
    void connectAppTabs(AgentToolHost* host);
    void configureAppRoots(AgentToolHost* host,const PluginContext& context);
    QJsonObject appThemeVariables() const;
    void updateAppThemes();
    void initializeGit();void showGit();void updateGitContext();
    void configureWorktreeSettingsPage();
    bool prepareGitMutation(const QString& path,GitOperation operation);
    bool gitPathBusy(const QString& path) const;
    void attachFiles(const QStringList& paths);
    void attachImage(const QImage& image);
    void refreshAttachments();
    void recordBrowserHistory(const QUrl& url,const QString& title);
    void clearBrowsingData(const QJsonObject& options);
    void finishBrowsingDataClear(bool success,const QString& error);
    void setBrowserFullscreen(BrowserWidget* browser,bool enabled);
    void generateTitle(const QString& id);void compressContext();void configureUtilities(UtilityService* service,const Conversation& session);
    Conversation* current(); Conversation* find(const QString& id);
    AgentEngine* engineFor(const QString& id);
    QString transcriptHtml(const Conversation& session) const;
    SessionStore m_store; ProviderRepository m_providerRepository; QList<Conversation> m_sessions; QList<ProviderConfig> m_providers;
    QHash<QString,AgentEngine*> m_engines; QHash<QString,QString> m_streams;
    QHash<QString,QStringList> m_toolEvents;
    QString m_currentId;
    QWidget *m_sidebar,*m_chat;
    QListWidget* m_agents;QLineEdit* m_search;
    QTextBrowser* m_transcript;Composer* m_composer;QComboBox *m_models,*m_permissions;
    QToolButton *m_send,*m_stop,*m_workspace;
    QLabel* m_status;QSplitter* m_splitter;QTabWidget* m_tabs;QTreeView* m_files;
    QFileSystemModel* m_fileModel;
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
    QSet<QString> m_gitMutatingPaths,m_worktreesRemoving,m_worktreeSetups;
    QSet<QString> m_repositoryWatchOwners;
    QSet<QString> m_liveConversationOwners;
    QHash<QString,QStringList> m_removalAffectedSessions;
    PluginCatalog m_pluginCatalog;
};
}
