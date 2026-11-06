#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QStringList>

namespace ade {
enum class GitOperation { Inspect, Status, Diff, FileDiff, Branches, Worktrees, SwitchBranch, CreateBranch, CreateWorktree, DeletionInfo, RemoveWorktree, CleanupCandidates, SetupRepositories, SaveSetupScript };
enum class GitRemovePolicy { Explicit, Settings, Cleanup };
enum class GitErrorCode { None, Cancelled, UntrustedPath, NotRepository, InvalidName, InvalidPath, BranchNotFound, BranchExists, BranchCheckedOut, WorktreeExists, MainWorktree, DirtyWorktree, MountedWorktree, UnsafeCleanup, ProcessFailed, Timeout, OutputLimit, IoError, Conflict };
struct GitError { GitErrorCode code=GitErrorCode::None; QString message; int exitCode=-1; };
struct GitStatus { bool known=false, dirty=false; int stagedCount=0, unstagedCount=0, untrackedCount=0; };
struct GitWorktree { QString path, branch, headSha; bool detached=false, main=false, current=false, locked=false, prunable=false; qint64 createdAt=-1; };
struct GitBranch { QString name, remoteName, remoteBranchName, checkedOutPath; bool remote=false, current=false, checkedOut=false; };
struct GitBranchList { QString current, defaultBranch, defaultRemoteBranch; QList<GitBranch> branches; bool refreshSucceeded=false; };
struct GitSnapshot { QString repositoryId, repoRoot, commonGitDir, mainWorktreePath, branch, headSha, remoteUrl; bool isWorktree=false; GitStatus status; };
struct GitDiffEntry { QString path, oldPath; char status='M'; qint64 added=0, deleted=0; bool staged=false, binary=false; };
struct GitDiffSummary { QList<GitDiffEntry> entries; qint64 added=0, deleted=0; };
// Raw bytes are deliberate: the native editor must preserve BOMs and line endings.
struct GitFileDiff { QString path, oldPath; QByteArray original, modified; bool staged=false, originalExists=false, modifiedExists=false, binary=false; qint64 mtimeMs=-1; };
struct GitDeletionInfo { QString path, branch; bool main=false, mounted=false, detached=false; GitStatus status; };
struct GitCleanupCandidate { GitWorktree worktree; QString mergedInto; qint64 lastUsedAt=-1; };
struct WorktreeSetupMetadata { QString workspacePath, sourceWorktreePath, mainWorktreePath; };
enum class GitSetupVariant { Posix, PowerShell };
struct GitScriptRevision { bool exists=false; QByteArray sha256; bool operator==(const GitScriptRevision &) const = default; };
struct GitSetupScript { QString path, content, error; bool exists=false; GitScriptRevision revision; QByteArray originalBytes; };
struct GitManagedWorktree { GitWorktree worktree; bool clean=false, removable=false; QString disabledReason; qint64 lastUsedAt=-1; };
struct GitSetupRepository { QString id, name, mainWorktreePath, trustedWorkspaceAnchor; GitSetupScript posix, powershell; bool hasCodexSetupScript=false; QList<GitManagedWorktree> worktrees; };
struct GitResult {
    GitOperation operation=GitOperation::Inspect; bool ok=false, mayHaveMutated=false; GitError error;
    GitSnapshot snapshot; GitStatus status; GitBranchList branches; QList<GitWorktree> worktrees;
    GitDiffSummary diff; GitFileDiff fileDiff; GitDeletionInfo deletion;
    QList<GitCleanupCandidate> cleanup; QList<GitSetupRepository> setupRepositories;
    QString path, branch; QStringList warnings; WorktreeSetupMetadata setup; GitSetupScript setupScript;
};
// Host supplies only paths the user opened/mounted. Unknown usage blocks cleanup.
struct GitWorkspaceContext { QStringList trustedRoots, mountedPaths; QMap<QString,qint64> lastUsedAt; bool usageKnown=false; };
struct GitServiceOptions { QString gitExecutable, managedWorktreeRoot; int readTimeoutMs=2000, mutationTimeoutMs=30000; qint64 outputLimit=32*1024*1024; };
struct WorktreeSetupRun {
    enum class Status { Running, Succeeded, Failed, Skipped };
    quint64 id=0; WorktreeSetupMetadata metadata; QString scriptPath, stdoutTail, stderrTail, message;
    Status status=Status::Running; qint64 startedAt=0, finishedAt=-1; int exitCode=-1;
};
}
Q_DECLARE_METATYPE(ade::GitResult)
Q_DECLARE_METATYPE(ade::WorktreeSetupRun)
