#pragma once
#include "GitService.h"
#include <QWidget>
#include <functional>
#include <memory>

namespace ade {
class GitPanel final : public QWidget {
    Q_OBJECT
public:
    explicit GitPanel(GitServiceOptions options={},QWidget *parent=nullptr);
    ~GitPanel() override;
    void setWorkspaceContext(const GitWorkspaceContext &context);
    void setWorkspace(const QString &workspace,const QString &contextId={});
    QString workspace() const;
    void refresh();
    GitService *service() const;
    bool isMutating() const;
    void setDarkTheme(bool dark);
    // Host closes/saves affected dirty editor tabs and checks active runs here.
    void setMutationGuard(std::function<bool(const QString &,GitOperation)> guard);
signals:
    void cleanupReviewRequested();
    void mutationStarted(const QString &path,ade::GitOperation operation);
    void mutationFinished(const QString &path,ade::GitOperation operation,bool success);
    void mountWorktreeRequested(const QString &path,const ade::WorktreeSetupMetadata &setup);
    void worktreeRemovalStarted(const QString &path);
    void worktreeRemovalFinished(const QString &path,bool success);
    void worktreeRemoved(const QString &path);
    void openDiffRequested(const QString &workspace,const QString &file,bool staged,const QString &oldPath);
    void repositoryChanged(const QString &workspace);
private:
    void refreshAfterTopology();
    struct Private; std::unique_ptr<Private> d;
    void finish(quint64 id,const GitResult &result);
    void updateActions();
    void loadBranches();
    void populateChanges();
    void populateWorktrees();
    void openSelectedDiff(bool staged);
    void createDialog(bool worktree);
    void switchDialog();
    void deletionDialog(const GitDeletionInfo &info);
    void startMutation(GitOperation operation,const QString &path,const QString &name={},const QString &source={},bool force=false);
    void setMessage(const QString &message,bool error=false);
};
}
Q_DECLARE_METATYPE(ade::WorktreeSetupMetadata)
Q_DECLARE_METATYPE(ade::GitOperation)
