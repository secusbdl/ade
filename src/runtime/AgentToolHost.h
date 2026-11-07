#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include "sandbox/SandboxHost.h"
#include "core/ToolExecutor.h"
#include "core/ContextLoader.h"
#include "network/NetworkBroker.h"
#include "attachments/AttachmentPipeline.h"
#include "apps/MiniAppHost.h"
#include "AttachmentFileBroker.h"
#include "HarnessFileContext.h"
#include "harness/HarnessAttachmentStore.h"
#include "mutations/FileMutationJournal.h"
#include "shell/ShellService.h"
#include "shell/SessionLogStore.h"
namespace ade {
class AgentEngine;
class BrowserToolBridge;
// Shares one browser registry while each conversation owns its JS/file state.
class AgentToolHost final:public QObject {
    Q_OBJECT
public:
    AgentToolHost(AgentEngine* engine,BrowserToolBridge* browser,const QString& agentId,
                 const QString& dataDirectory,QObject* parent=nullptr,QNetworkAccessManager* network=nullptr);
    ~AgentToolHost() override;
    bool setWorkspace(const QString& workspace,QString* error=nullptr);
    ContextSnapshot configureContext(const ContextOptions& options);
    QJsonObject captureAttachments(const QJsonArray& references,QString* error=nullptr);
    bool cloneFileContext(const QString& targetAgentId,QString* error=nullptr);
    bool removeFileContext(QString* error=nullptr);
    QJsonArray mutationHistory(QString* error=nullptr,int limit=200)const;
    void requestTurnFiles(quint64 requestId,const QStringList& mutationIds,const QString& path={});
    void undoMutations(const QString& requestId,const QStringList& mutationIds);
    void recoverMutations(const QString& requestId);
    void cancelUndo(const QString& requestId);
    bool mutationBusy()const{return !m_undoRequests.isEmpty();}
    ShellService* shells(){return &m_shells;}
    SandboxHost* sandbox(){return &m_sandbox;}
    NetworkBroker* network(){return &m_network;}
    MiniAppHost* apps(){return &m_apps;}
    QString agentId() const{return m_agentId;}
    FileReadSnapshotStore::Lease fileContextLease() const{return m_fileContextRemoved?FileReadSnapshotStore::Lease{}:m_readLease;}
    AttachmentMounts fileContextMounts() const{return m_fileContextRemoved?AttachmentMounts{}:m_attachmentMounts;}
signals:
    void output(QString requestId,QString text);
    void fileChanged(QString path,QByteArray before,QByteArray after);
    void attachmentCreated(QString requestId,QString path,QString mimeType);
    void mutationUndoCompleted(QString requestId,QString output,bool error,QJsonObject metadata);
    void turnFilesReady(quint64 requestId,QJsonObject summary,QString error);
private:
    QPointer<AgentEngine> m_engine;
    QPointer<BrowserToolBridge> m_browser;
    QString m_agentId;
    QString m_dataDirectory;
    HarnessFileContext m_harnessFiles;
    HarnessAttachmentStore m_harnessAttachments;
    FileMutationJournal m_mutations;
    QHash<QString,std::shared_ptr<std::atomic_bool>> m_undoRequests;
    std::shared_ptr<SessionLogStore> m_shellLogs;
    ShellService m_shells;
    QSet<QString> m_deliveredWatcherSessions;
    QHash<QString,QString> m_pendingWatcherMessages;
    QTimer m_watcherRetryTimer;
    int m_watcherRetryDelay=250;
    AttachmentStore m_attachmentStore;
    AttachmentPipeline m_attachmentPipeline;
    FileReadSnapshotStore m_readSnapshots;
    FileReadSnapshotStore::Lease m_readLease;
    AttachmentMounts m_attachmentMounts;
    QHash<QString,QJsonArray> m_toolAttachments;
    QHash<QString,QJsonArray> m_toolMutations;
    QHash<QString,QString> m_sandboxFileOwners;
    QSet<QString> m_pendingFileMutations;
    QSet<quint64> m_turnFileRequests;
    struct TurnFileQuery {quint64 id;QStringList mutations;QString path;};
    QList<TurnFileQuery> m_turnFileQueue;
    int m_activeTurnFileQueries=0;
    void startTurnFileQueries();
    QHash<QString,QString> m_attachmentErrors;
    AttachmentFileBroker m_attachmentFiles;
    NetworkBroker m_network;
    MiniAppHost m_apps;
    SandboxHost m_sandbox;
    ToolExecutor m_files;
    ContextLoader m_context;
    QSet<QString> m_directBrowser,m_sandboxBrowser,m_sandboxNetwork;
    QSet<QString> m_sandboxApps;
    QSet<QString> m_sandboxFiles;
    QSet<QString> m_sandboxAttachmentFiles;
    bool m_workspaceChanging=false;
    bool m_fileContextRemoved=false;
    QHash<QString,QJsonObject> m_eventSubscriptions;
    void removeEventSubscription(const QString& id);
    void resetEvents();
    void deliverWatcherMessage(const QString& sessionId);
    void scheduleWatcherRetry();
    void runMutationAction(const QString& requestId,const QStringList& mutationIds,bool recover);
};
}
