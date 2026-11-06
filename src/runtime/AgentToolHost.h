#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include "sandbox/SandboxHost.h"
#include "core/ToolExecutor.h"
#include "core/ContextLoader.h"
#include "network/NetworkBroker.h"
#include "attachments/AttachmentPipeline.h"
#include "apps/MiniAppHost.h"
#include "AttachmentFileBroker.h"
namespace ade {
class AgentEngine;
class BrowserToolBridge;
// Shares one browser registry while each conversation owns its JS/file state.
class AgentToolHost final:public QObject {
    Q_OBJECT
public:
    AgentToolHost(AgentEngine* engine,BrowserToolBridge* browser,const QString& agentId,
                 const QString& dataDirectory,QObject* parent=nullptr);
    ~AgentToolHost() override;
    bool setWorkspace(const QString& workspace,QString* error=nullptr);
    ContextSnapshot configureContext(const ContextOptions& options);
    QJsonObject captureAttachments(const QJsonArray& references,QString* error=nullptr);
    bool cloneFileContext(const QString& targetAgentId,QString* error=nullptr);
    bool removeFileContext(QString* error=nullptr);
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
private:
    QPointer<AgentEngine> m_engine;
    QPointer<BrowserToolBridge> m_browser;
    QString m_agentId;
    AttachmentStore m_attachmentStore;
    AttachmentPipeline m_attachmentPipeline;
    FileReadSnapshotStore m_readSnapshots;
    FileReadSnapshotStore::Lease m_readLease;
    AttachmentMounts m_attachmentMounts;
    QHash<QString,QJsonArray> m_toolAttachments;
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
};
}
