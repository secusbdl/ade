#pragma once
#include "GitTypes.h"
#include <QObject>
#include <memory>

namespace ade {
// All public methods are called on this QObject's owning thread. Work runs on a
// private worker pool; commands for the same Git common directory are serialized.
class GitService final : public QObject {
    Q_OBJECT
public:
    explicit GitService(GitServiceOptions options={}, QObject *parent=nullptr);
    ~GitService() override;
    void setWorkspaceContext(const GitWorkspaceContext &context);
    // Stable host owner IDs (for example chat/mount IDs) share one native
    // metadata watcher after an authorized asynchronous repository inspection.
    bool watchWorkspace(const QString &ownerId,const QString &workspace);
    void unwatchWorkspace(const QString &ownerId);
    quint64 inspect(const QString &workspace);
    quint64 status(const QString &workspace);
    quint64 diff(const QString &workspace);
    quint64 fileDiff(const QString &workspace, const QString &path, bool staged, const QString &oldPath={});
    quint64 branches(const QString &workspace, bool refresh=false);
    quint64 worktrees(const QString &workspace);
    quint64 switchBranch(const QString &workspace, const QString &branch);
    quint64 createBranch(const QString &workspace, const QString &branch, const QString &sourceBranch);
    quint64 createWorktree(const QString &workspace, const QString &name, const QString &sourceBranch);
    quint64 deletionInfo(const QString &workspace);
    // Branches are always retained. Explicit force never propagates to Settings/Cleanup.
    quint64 removeWorktree(const QString &workspace, GitRemovePolicy policy=GitRemovePolicy::Explicit, bool force=false);
    quint64 cleanupCandidates();
    quint64 setupRepositories();
    // Resolve the main root from this currently trusted anchor; compare the
    // exact loaded bytes before saving. A missing baseline is explicit.
    quint64 saveSetupScript(const QString &trustedWorkspaceAnchor, GitSetupVariant variant,
                            const QString &content, const GitScriptRevision &expected);
    void cancel(quint64 requestId);
    void cancelAll();
    static QString canonicalPath(const QString &path);
    static QString pathKey(const QString &path);
signals:
    void completed(quint64 requestId, const ade::GitResult &result);
    // Only emitted after a confirmed successful removal. Host detaches surviving
    // chats and may then delete chats if the user separately requested that.
    void worktreeRemoved(const QString &path);
    void repositoryTopologyChanged(const QString &commonGitDir,quint64 revision);
    void repositoryWatchCoverageChanged(const QString &commonGitDir,bool complete,const QString &reason);
    void repositoryWatchError(const QString &ownerId,const QString &reason);
private:
    struct Private; std::unique_ptr<Private> d;
    bool watchAuthorized(const QString &workspace) const;
    void completeWatch(quint64 id,const GitResult &result);
    quint64 submit(GitOperation operation, const QString &workspace={}, const QString &a={}, const QString &b={}, bool flag=false, GitRemovePolicy policy=GitRemovePolicy::Explicit, const GitScriptRevision &expected={});
};
}
