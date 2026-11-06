#include "GitService.h"
#include "GitRepositoryWatcher.h"
#include "WorktreeUsageIdentity.h"
#include "GitParsing.h"
#include "ProcessRunner.h"
#include "SetupScriptText.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>
#include <QThreadPool>
#include <QUrl>
#include <algorithm>
#include <functional>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#endif

namespace ade {
using namespace gitdetail;
namespace {
struct Failure { GitError error; };
[[noreturn]] void fail(GitErrorCode code, const QString &message, int exitCode=-1) { throw Failure{{code,message,exitCode}}; }
void requireNotCancelled(const Cancellation &cancel){if(cancel->load())fail(GitErrorCode::Cancelled,"Operation cancelled.");}
QString managedRootAuthority(const QString &configured){
    // Resolve the configured parent once, including legitimate profile-parent
    // junctions. Do not follow the managed-root entry itself: it must remain a
    // directory at this authorized name, not redirect future scans elsewhere.
    const QFileInfo target(QDir::cleanPath(QFileInfo(configured).absoluteFilePath()));
    QString ancestor=target.absolutePath(),relative=target.fileName();
    if(relative.isEmpty())return {};
    while(!QFileInfo(ancestor).exists()&&!QFileInfo(ancestor).isSymLink()&&!QFileInfo(ancestor).isJunction()){
        const QFileInfo part(ancestor);if(part.absolutePath()==ancestor)return {};
        relative=part.fileName()+'/'+relative;ancestor=part.absolutePath();
    }
    const auto resolved=canonical(ancestor);
    if(resolved.isEmpty()||!safeRelativePath(relative))return {};
    return QDir::cleanPath(QDir(resolved).filePath(relative));
}
QString currentManagedRoot(const QString &authority){
    const auto resolved=canonical(authority);
    return !authority.isEmpty()&&key(resolved)==key(authority)?resolved:QString();
}
#ifdef Q_OS_WIN
QString openedPath(HANDLE handle){
    const DWORD length=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!length)return {};
    std::wstring buffer(length+1,L'\0');const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size())return {};
    auto path=QString::fromWCharArray(buffer.data(),int(written));
    if(path.startsWith("\\\\?\\UNC\\"))path="\\\\"+path.mid(8);else if(path.startsWith("\\\\?\\"))path=path.mid(4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}
#endif
QByteArray readSetupBytes(const QString &root,const QString &path,const Cancellation &cancel){
    requireNotCancelled(cancel);QByteArray bytes;constexpr qint64 limit=1024*1024;
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();HANDLE handle=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle==INVALID_HANDLE_VALUE)fail(GitErrorCode::IoError,"Cannot open setup script.");
    const auto close=qScopeGuard([&]{CloseHandle(handle);});BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!GetFileSizeEx(handle,&size))fail(GitErrorCode::IoError,"Setup script is not a readable regular file.");
    if(!inside(root,openedPath(handle)))fail(GitErrorCode::InvalidPath,"Opened setup script resolves outside the repository.");
    if(size.QuadPart<0||size.QuadPart>limit)fail(GitErrorCode::OutputLimit,"Setup script exceeds 1 MiB.");
    QByteArray chunk(65536,Qt::Uninitialized);
    while(true){requireNotCancelled(cancel);DWORD count=0;if(!ReadFile(handle,chunk.data(),DWORD(chunk.size()),&count,nullptr))fail(GitErrorCode::IoError,"Cannot read setup script completely.");if(!count)break;if(bytes.size()+count>limit)fail(GitErrorCode::OutputLimit,"Setup script grew beyond 1 MiB.");bytes.append(chunk.constData(),count);}
#else
    QFile file(path);if(!QFileInfo(path).isFile()||!inside(root,canonical(path))||!file.open(QIODevice::ReadOnly))fail(GitErrorCode::IoError,"Cannot open scoped setup script.");
    bytes=file.read(limit+1);if(bytes.size()>limit||!file.atEnd()||file.error()!=QFileDevice::NoError)fail(GitErrorCode::IoError,"Cannot read setup script within its 1 MiB limit.");
#endif
    requireNotCancelled(cancel);return bytes;
}
QString text(const QByteArray &bytes) { return removeLineEnding(QString::fromUtf8(bytes)); }
QString displayError(const ProcessResult &result) {
    auto message=!result.error.isEmpty() ? result.error : QString::fromUtf8(result.err).trimmed();
    static const QRegularExpression urls("https?://[^\\s]+",QRegularExpression::CaseInsensitiveOption);
    auto matches=urls.globalMatch(message); QList<QRegularExpressionMatch> all;
    while(matches.hasNext()) all.append(matches.next());
    for(auto i=all.crbegin();i!=all.crend();++i) {
        QUrl url(i->captured()); url.setUserInfo({}); url.setQuery({}); url.setFragment({});
        message.replace(i->capturedStart(),i->capturedLength(),url.toString());
    }
    return message.left(2048);
}
void requireProcess(const ProcessResult &result) {
    if(result.ok()) return;
    fail(result.cancelled?GitErrorCode::Cancelled:result.timedOut?GitErrorCode::Timeout:result.limited?GitErrorCode::OutputLimit:GitErrorCode::ProcessFailed,
         displayError(result).isEmpty()?"Git command failed.":displayError(result),result.exitCode);
}
QString webRemote(QString remote) {
    remote=remote.trimmed();
    if(!remote.contains("://")) {
        static const QRegularExpression scp("^(?:[^@/]+@)?([^:/]+):(.+)$");
        const auto match=scp.match(remote);
        if(!match.hasMatch()) return {};
        remote="https://"+match.captured(1)+'/'+match.captured(2);
    }
    QUrl url(remote);
    if(!url.isValid() || url.host().isEmpty()) return {};
    if(url.scheme()=="ssh" || url.scheme()=="git") url.setScheme("https");
    if(url.scheme()!="https" && url.scheme()!="http") return {};
    url.setUserInfo({}); url.setQuery({}); url.setFragment({});
    auto path=url.path(); if(path.endsWith(".git")) path.chop(4); url.setPath(path);
    return url.toString();
}
struct Shared {
    QMutex locksMutex; QMap<QString,std::shared_ptr<QMutex>> locks;
    QMutex contextMutex; GitWorkspaceContext context;
    std::shared_ptr<QMutex> lockFor(const QString &path) {
        QMutexLocker locker(&locksMutex); auto &lock=locks[key(path)];
        if(!lock) lock=std::make_shared<QMutex>();
        return lock;
    }
};
struct Lock {
    std::shared_ptr<QMutex> mutex;
    Lock(std::shared_ptr<QMutex> value,const Cancellation &cancel):mutex(std::move(value)) {
        while(!mutex->tryLock(25)) if(cancel->load()) fail(GitErrorCode::Cancelled,"Operation cancelled.");
        if(cancel->load()) { mutex->unlock(); mutex.reset(); fail(GitErrorCode::Cancelled,"Operation cancelled."); }
    }
    ~Lock() { if(mutex) mutex->unlock(); }
};
class Work {
public:
    GitServiceOptions options; GitWorkspaceContext context; Cancellation cancel; std::shared_ptr<Shared> shared;
    bool mutationStarted=false; WorktreeCleanupPermit cleanupPermit;
    QProcessEnvironment environment=safeEnvironment();
    ProcessResult command(const QString &cwd,const QStringList &args,bool mutation=false) {
        QStringList finalArgs{"--literal-pathspecs","-c","color.ui=false","-c","core.quotePath=false"};
#ifdef Q_OS_WIN
        // Git for Windows defaults to MAX_PATH-limited checkout operations.
        // Managed worktrees can exceed that limit even when the source clone
        // fits. Opt in per command without changing any repository/user config.
        finalArgs<<"-c"<<"core.longpaths=true";
#endif
        finalArgs+=args;
        if(mutation) mutationStarted=true;
        return runProcess(options.gitExecutable,finalArgs,cwd,environment,cancel,mutation?options.mutationTimeoutMs:options.readTimeoutMs,options.outputLimit);
    }
    QByteArray checked(const QString &cwd,const QStringList &args,bool mutation=false) {
        const auto result=command(cwd,args,mutation); requireProcess(result); return result.out;
    }
    void refreshContext() { QMutexLocker lock(&shared->contextMutex); context=shared->context; }
    QString managedRoot() const { return currentManagedRoot(options.managedWorktreeRoot); }
    bool managed(const QString &path) const { return inside(managedRoot(),canonical(path)); }
    bool trusted(const QString &path) const {
        const auto resolved=canonical(path); if(resolved.isEmpty()) return false;
        if(inside(managedRoot(),resolved)) return true;
        for(const auto &root:context.trustedRoots) if(key(canonical(root))==key(resolved)) return true;
        for(const auto &root:context.mountedPaths) if(key(canonical(root))==key(resolved)) return true;
        return false;
    }
    bool mounted(const QString &path) const {
        for(const auto &entry:context.mountedPaths) if(key(canonical(entry))==key(path)) return true;
        return false;
    }
    GitSnapshot repository(const QString &requested,bool enforceTrust=true) {
        const QString path=canonical(requested);
        if(path.isEmpty() || !QFileInfo(path).isDir()) fail(GitErrorCode::NotRepository,"Workspace is unavailable.");
        if(enforceTrust && !trusted(path)) fail(GitErrorCode::UntrustedPath,"Workspace has not been opened or mounted.");
        const auto top=command(path,{"rev-parse","--show-toplevel"});
        if(top.cancelled||top.timedOut||top.limited) requireProcess(top);
        if(!top.ok()) fail(GitErrorCode::NotRepository,"Workspace is not a Git repository root.",top.exitCode);
        const QString root=canonical(text(top.out));
        if(root.isEmpty() || key(root)!=key(path)) fail(GitErrorCode::NotRepository,"Open the exact repository or worktree root.");
        const QString commonRaw=text(checked(root,{"rev-parse","--git-common-dir"}));
        const QString common=canonical(QDir::isAbsolutePath(commonRaw)?commonRaw:QDir(root).filePath(commonRaw));
        if(common.isEmpty()) fail(GitErrorCode::NotRepository,"Git common directory is unavailable.");
        GitSnapshot result; result.repoRoot=root; result.commonGitDir=common; result.repositoryId=common; return result;
    }
    QList<GitWorktree> treeList(const GitSnapshot &repo) {
        QList<GitWorktree> result;
        if(!parseWorktrees(checked(repo.repoRoot,{"worktree","list","--porcelain","-z"}),&result)) fail(GitErrorCode::ProcessFailed,"Invalid Git worktree list.");
        QList<GitWorktree> existing;
        for(auto item:result) {
            const auto path=canonical(item.path);
            if(path.isEmpty() && item.prunable) continue;
            item.path=path.isEmpty()?QDir::fromNativeSeparators(item.path):path;
            item.current=key(item.path)==key(repo.repoRoot);
            const QFileInfo marker(QDir(item.path).filePath(".git"));
            const auto date=marker.birthTime().isValid()?marker.birthTime():marker.lastModified();
            item.createdAt=marker.exists() && date.isValid()?date.toMSecsSinceEpoch():-1;
            existing.append(item);
        }
        return existing;
    }
    GitStatus getStatus(const QString &path) {
        GitStatus result;
        if(!parseStatus(checked(path,{"status","--porcelain=v1","-z","--untracked-files=all"}),&result)) fail(GitErrorCode::ProcessFailed,"Invalid Git status output.");
        return result;
    }
    GitSnapshot snapshot(GitSnapshot result) {
        const auto trees=treeList(result);
        for(const auto &tree:trees) {
            if(tree.main) result.mainWorktreePath=tree.path;
            if(tree.current) { result.branch=tree.branch; result.headSha=tree.headSha; result.isWorktree=!tree.main; }
        }
        if(result.mainWorktreePath.isEmpty()) fail(GitErrorCode::NotRepository,"Main worktree is unavailable.");
        result.status=getStatus(result.repoRoot);
        const auto remotes=text(checked(result.repoRoot,{"remote"})).split('\n',Qt::SkipEmptyParts);
        if(!remotes.isEmpty()) {
            const auto remote=remotes.contains("origin")?QStringLiteral("origin"):remotes.first();
            const auto url=command(result.repoRoot,{"remote","get-url",remote}); if(url.ok()) result.remoteUrl=webRemote(text(url.out));
        }
        return result;
    }
    void snapshotAfterMutation(GitResult &result,const GitSnapshot &repo) {
        try { result.snapshot=snapshot(repo); }
        catch(const Failure &failure) { result.warnings.append("Git mutation completed, but its refreshed snapshot failed: "+failure.error.message); }
    }
    GitBranchList branchList(const GitSnapshot &repo,bool refresh) {
        GitBranchList result;
        if(refresh) { const auto fetched=command(repo.repoRoot,{"fetch","--prune","--all"},true); if(fetched.cancelled) requireProcess(fetched); result.refreshSucceeded=fetched.ok(); }
        const auto trees=treeList(repo);
        const auto remotes=text(checked(repo.repoRoot,{"remote"})).split('\n',Qt::SkipEmptyParts);
        const auto lines=text(checked(repo.repoRoot,{"for-each-ref","--format=%(refname)%09%(HEAD)","refs/heads","refs/remotes"})).split('\n',Qt::SkipEmptyParts);
        for(const auto &line:lines) {
            const auto fields=line.split('\t'); if(fields.size()!=2) fail(GitErrorCode::ProcessFailed,"Invalid Git branch list.");
            const auto ref=fields[0]; GitBranch branch;
            branch.remote=ref.startsWith("refs/remotes/"); branch.name=ref.mid(branch.remote?13:11);
            if(branch.remote && branch.name.endsWith("/HEAD")) continue;
            branch.current=fields[1]=="*";
            if(branch.current) result.current=branch.name;
            if(branch.remote) {
                for(const auto &remote:remotes) if(branch.name.startsWith(remote+'/') && remote.size()>branch.remoteName.size()) branch.remoteName=remote;
                branch.remoteBranchName=branch.name.mid(branch.remoteName.size()+1);
            } else for(const auto &tree:trees) if(tree.branch==branch.name) { branch.checkedOut=true; branch.checkedOutPath=tree.path; }
            result.branches.append(branch);
        }
        auto has=[&](const QString &name,bool remote=false) { return std::any_of(result.branches.begin(),result.branches.end(),[&](const auto &b){return b.name==name && b.remote==remote;}); };
        const auto originHead=command(repo.repoRoot,{"symbolic-ref","--quiet","--short","refs/remotes/origin/HEAD"});
        const auto origin=originHead.ok()?text(originHead.out).mid(7):QString();
        if(!origin.isEmpty() && has(origin)) result.defaultBranch=origin;
        else if(has("main")) result.defaultBranch="main";
        else if(has("master")) result.defaultBranch="master";
        else if(!result.current.isEmpty()) result.defaultBranch=result.current;
        else for(const auto &b:result.branches) if(!b.remote) { result.defaultBranch=b.name; break; }
        QString selectedRemote;
        for(const auto &name:QStringList{result.current,result.defaultBranch}) {
            if(name.isEmpty()) continue;
            const auto config=command(repo.repoRoot,{"config","--get","branch."+name+".remote"});
            if(config.ok() && remotes.contains(text(config.out))) { selectedRemote=text(config.out); break; }
        }
        if(selectedRemote.isEmpty() && remotes.contains("origin")) selectedRemote="origin";
        if(selectedRemote.isEmpty() && !remotes.isEmpty()) selectedRemote=remotes.first();
        if(!selectedRemote.isEmpty()) {
            const auto head=command(repo.repoRoot,{"symbolic-ref","--quiet","--short","refs/remotes/"+selectedRemote+"/HEAD"});
            if(head.ok() && has(text(head.out),true)) result.defaultRemoteBranch=text(head.out);
            for(const auto &name:QStringList{selectedRemote+"/main",selectedRemote+"/master"}) if(result.defaultRemoteBranch.isEmpty() && has(name,true)) result.defaultRemoteBranch=name;
            if(result.defaultRemoteBranch.isEmpty()) for(const auto &b:result.branches) if(b.remoteName==selectedRemote) { result.defaultRemoteBranch=b.name; break; }
        }
        return result;
    }
    GitBranch sourceBranch(const GitSnapshot &repo,const QString &name) {
        for(const auto &branch:branchList(repo,false).branches) if(branch.name==name) return branch;
        fail(GitErrorCode::BranchNotFound,"Source branch does not exist.");
    }
    QString validNewName(const GitSnapshot &repo,const QString &name) {
        const auto trimmed=name.trimmed();
        if(trimmed.isEmpty() || trimmed.startsWith('-') || trimmed.contains(QChar::Null) || !safeRelativePath(trimmed)) fail(GitErrorCode::InvalidName,"Invalid branch or worktree name.");
        const auto result=command(repo.repoRoot,{"check-ref-format","--branch",trimmed});
        if(!result.ok() || text(result.out)!=trimmed) fail(GitErrorCode::InvalidName,"Invalid branch or worktree name.");
        for(const auto &branch:branchList(repo,false).branches) if(!branch.remote && branch.name==trimmed) fail(GitErrorCode::BranchExists,"A local branch with this name already exists.");
        return trimmed;
    }
    QString sourceRef(const GitSnapshot &repo,const GitBranch &source) {
        if(!source.remote) return "refs/heads/"+source.name;
        if(source.remoteName.isEmpty() || source.remoteName.startsWith('-')) fail(GitErrorCode::InvalidName,"Invalid remote name.");
        checked(repo.repoRoot,{"fetch","--prune","--",source.remoteName,"refs/heads/"+source.remoteBranchName+":refs/remotes/"+source.name},true);
        return "refs/remotes/"+source.name;
    }
    QByteArray diskBytes(const QString &root,const QString &relative,bool *exists=nullptr,qint64 *mtime=nullptr) {
        const auto path=safeChildPath(root,relative);
        if(path.isEmpty()) fail(GitErrorCode::InvalidPath,"File path resolves outside the workspace.");
        const QFileInfo info(path);
        if(exists) *exists=info.exists();
        if(mtime) *mtime=info.exists()?info.lastModified().toMSecsSinceEpoch():-1;
        if(!info.exists()) return {};
        if(info.isSymLink() || info.isJunction()) fail(GitErrorCode::InvalidPath,"Symbolic-link file content is not editable through a Git diff.");
        QFile file(path); if(!file.open(QIODevice::ReadOnly)) fail(GitErrorCode::IoError,file.errorString());
        if(file.size()>options.outputLimit) fail(GitErrorCode::OutputLimit,"File exceeds the Git content limit.");
        auto bytes=file.readAll(); if(file.error()!=QFileDevice::NoError) fail(GitErrorCode::IoError,file.errorString()); return bytes;
    }
    QByteArray blob(const GitSnapshot &repo,const QString &spec,bool *exists) {
        *exists=false;
        const bool index=spec.startsWith(':'); const auto path=spec.mid(index?1:5);
        QByteArray records;
        if(index) records=checked(repo.repoRoot,{"ls-files","--stage","-z","--",path});
        else {
            const auto head=command(repo.repoRoot,{"rev-parse","--verify","--quiet","HEAD"});
            if(!head.ok()) {
                // An unborn branch has no committed blob. Other command errors stay errors.
                if(head.error.isEmpty() && head.exitCode==1) return {};
                requireProcess(head);
            }
            records=checked(repo.repoRoot,{"ls-tree","-z","HEAD","--",path});
        }
        for(const auto &record:records.split('\0')) {
            if(record.isEmpty()) continue;
            const auto tab=record.indexOf('\t'); if(tab<0) fail(GitErrorCode::ProcessFailed,"Invalid Git blob metadata.");
            if(QString::fromUtf8(record.mid(tab+1))!=path) continue;
            const auto fields=record.left(tab).split(' '); if(fields.size()!=3) fail(GitErrorCode::ProcessFailed,"Invalid Git blob metadata.");
            if(index && fields[2]!="0") fail(GitErrorCode::ProcessFailed,"File has unmerged index entries.");
            if(!index && fields[1]!="blob") fail(GitErrorCode::InvalidPath,"Git entry is not a regular file blob.");
            const auto oid=QString::fromLatin1(fields[index?1:2]);
            *exists=true; return checked(repo.repoRoot,{"cat-file","blob",oid});
        }
        return {};
    }
    GitFileDiff content(const GitSnapshot &repo,const QString &path,bool staged,const QString &oldPath) {
        if(!safeRelativePath(path) || (!oldPath.isEmpty() && !safeRelativePath(oldPath))) fail(GitErrorCode::InvalidPath,"Invalid repository-relative file path.");
        GitFileDiff result; result.path=path; result.oldPath=oldPath; result.staged=staged;
        result.original=blob(repo,"HEAD:"+(oldPath.isEmpty()?path:oldPath),&result.originalExists);
        if(staged) result.modified=blob(repo,":"+path,&result.modifiedExists);
        else result.modified=diskBytes(repo.repoRoot,path,&result.modifiedExists,&result.mtimeMs);
        result.binary=result.original.left(8000).contains('\0')||result.modified.left(8000).contains('\0'); return result;
    }
    GitDiffSummary diffSummary(const GitSnapshot &repo) {
        GitDiffSummary result; QMap<QString,GitDiffEntry> combined;
        for(const bool staged:{false,true}) {
            QStringList base{"diff","--find-renames","--no-ext-diff","--no-textconv"}; if(staged) base<<"--cached";
            auto statArgs=base; statArgs<<"--numstat"<<"-z"<<"--";
            auto nameArgs=base; nameArgs<<"--name-status"<<"-z"<<"--";
            QList<GitDiffEntry> entries;
            if(!parseDiff(checked(repo.repoRoot,statArgs),checked(repo.repoRoot,nameArgs),staged,&entries)) fail(GitErrorCode::ProcessFailed,"Invalid Git diff output.");
            for(const auto &entry:entries) {
                if(!combined.contains(entry.path)) combined.insert(entry.path,entry);
                else {
                    auto &item=combined[entry.path]; item.added+=entry.added; item.deleted+=entry.deleted; item.staged|=entry.staged; item.binary|=entry.binary;
                    if(item.oldPath.isEmpty()) item.oldPath=entry.oldPath;
                    if(entry.status=='D' || item.status=='D') item.status='D'; else if(entry.status=='A'||item.status=='A') item.status='A';
                }
            }
        }
        const auto untracked=checked(repo.repoRoot,{"ls-files","--others","--exclude-standard","-z"}).split('\0');
        for(const auto &name:untracked) {
            if(name.isEmpty()) continue;
            const auto path=QString::fromUtf8(name); const auto bytes=diskBytes(repo.repoRoot,path);
            GitDiffEntry entry; entry.path=path; entry.status='U'; entry.binary=bytes.left(8000).contains('\0');
            if(!entry.binary && !bytes.isEmpty()) entry.added=bytes.count('\n')+(bytes.endsWith('\n')?0:1);
            if(combined.contains(path)) { auto &item=combined[path]; item.status='M'; item.added+=entry.added; item.binary|=entry.binary; }
            else combined.insert(path,entry);
        }
        for(const auto &entry:combined) { result.entries.append(entry); result.added+=entry.added; result.deleted+=entry.deleted; }
        return result;
    }
    GitDeletionInfo deletion(const GitSnapshot &repo) {
        GitDeletionInfo result; result.path=repo.repoRoot; result.mounted=mounted(repo.repoRoot);
        bool found=false;
        for(const auto &tree:treeList(repo)) if(tree.current) { found=true; result.main=tree.main; result.branch=tree.branch; result.detached=tree.detached; }
        if(!found) fail(GitErrorCode::NotRepository,"Worktree is no longer registered.");
        result.status=getStatus(repo.repoRoot); return result;
    }
    qint64 lastUsed(const QString &path) const {
        for(auto it=context.lastUsedAt.begin();it!=context.lastUsedAt.end();++it) if(key(canonical(it.key()))==key(path)) return it.value();
        return -1;
    }
    QString mergedTarget(const GitSnapshot &repo,const QString &branch) {
        const auto branches=branchList(repo,false);
        const QStringList targets{branches.defaultBranch,"main","master","develop","dev"}; QSet<QString> seen;
        for(const auto &target:targets) {
            if(target.isEmpty() || target==branch || seen.contains(target)) continue;
            seen.insert(target);
            bool exists=false; for(const auto &b:branches.branches) if(!b.remote && b.name==target) exists=true;
            if(!exists) continue;
            const auto result=command(repo.repoRoot,{"merge-base","--is-ancestor","refs/heads/"+branch,"refs/heads/"+target});
            if(result.ok()) return target;
            if(result.cancelled||result.timedOut||result.limited) requireProcess(result);
        }
        return {};
    }
    WorktreeCleanupFacts usageFacts(const GitSnapshot &repo,bool withMerge) {
        WorktreeCleanupFacts facts;const auto info=deletion(repo);
        const auto raw=text(checked(repo.repoRoot,{"rev-parse","--absolute-git-dir"}));
        QString error;facts.identity=probeWorktreeUsageIdentity(repo,canonical(raw),&error);
        if(!facts.identity.valid())fail(GitErrorCode::UnsafeCleanup,"Cannot verify worktree identity: "+error);
        facts.authorized=trusted(repo.repoRoot);facts.managed=managed(repo.repoRoot);facts.registered=true;
        facts.main=info.main;facts.detached=info.detached;facts.branch=info.branch;
        facts.statusKnown=info.status.known;facts.dirty=info.status.dirty;facts.mounted=info.mounted;
        // Status may invoke filesystem-monitor hooks or run concurrently with
        // another Git client. Do not combine its result with an earlier HEAD.
        bool currentFound=false;
        for(const auto &tree:treeList(repo))if(tree.current){
            currentFound=true;
            if(tree.branch!=info.branch||tree.detached!=info.detached)
                fail(GitErrorCode::Conflict,"Worktree branch changed during status inspection.");
            facts.headSha=tree.headSha;facts.busy=tree.locked||tree.prunable;
        }
        if(!currentFound)fail(GitErrorCode::NotRepository,"Worktree registration changed during inspection.");
        if(withMerge&&!facts.main&&!facts.detached&&!facts.branch.isEmpty()) {
            facts.defaultBranch=branchList(repo,false).defaultBranch;
            facts.mergedInto=mergedTarget(repo,facts.branch);facts.merged=!facts.mergedInto.isEmpty();
        }
        return facts;
    }
    void verifyCleanupPermit() {
        const auto &permit=cleanupPermit;
        if(!permit.valid||!permit.valid->load()||!permit.record.identity.valid()||permit.journalRevision.size()!=32||permit.branch.isEmpty()||permit.headSha.isEmpty())
            fail(GitErrorCode::UnsafeCleanup,"Review expired. Refresh unused worktrees before removing anything.");
        QFile file(permit.journalPath);
        if(!file.open(QIODevice::ReadOnly))fail(GitErrorCode::UnsafeCleanup,"Worktree usage history is unavailable.");
#ifdef Q_OS_WIN
        if(file.handle()<0||key(openedPath(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()))))!=key(QDir::cleanPath(permit.journalPath)))
            fail(GitErrorCode::UnsafeCleanup,"Worktree usage history path changed.");
#endif
        const auto bytes=file.read(4*1024*1024+1);
        if(bytes.size()>4*1024*1024||!file.atEnd()||file.error()!=QFileDevice::NoError||QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)!=permit.journalRevision)
            fail(GitErrorCode::UnsafeCleanup,"Worktree usage history changed. Review again.");
        requireNotCancelled(cancel);
        if(!permit.valid->load())fail(GitErrorCode::UnsafeCleanup,"Review was revoked by workspace activity.");
    }
    GitCleanupCandidate cleanup(const GitSnapshot &repo) {
        verifyCleanupPermit();
        const auto facts=usageFacts(repo,true);
        if(facts.branch!=cleanupPermit.branch||facts.headSha!=cleanupPermit.headSha)
            fail(GitErrorCode::UnsafeCleanup,"Worktree branch or HEAD changed after review.");
        const auto decision=evaluateWorktreeCleanup(facts,&cleanupPermit.record,cleanupPermit.health,QDateTime::currentMSecsSinceEpoch());
        if(!decision.eligible)fail(GitErrorCode::UnsafeCleanup,"Worktree cannot be cleaned: "+decision.reason);
        GitCleanupCandidate result;result.lastUsedAt=decision.lastUsedAt;result.mergedInto=facts.mergedInto;
        for(const auto &tree:treeList(repo))if(tree.current)result.worktree=tree;
        return result;
    }
    QStringList managedPaths() const {
        const auto root=managedRoot(); if(root.isEmpty()) return {};
        QStringList paths; QList<QString> pending{root}; int visited=0;
        while(!pending.isEmpty()) {
            if(cancel->load()) fail(GitErrorCode::Cancelled,"Operation cancelled.");
            if(++visited>20000) fail(GitErrorCode::OutputLimit,"Managed worktree directory scan exceeds 20,000 directories.");
            const auto directory=pending.takeLast();
            for(const auto &info:QDir(directory).entryInfoList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden|QDir::System)) {
                if(info.isSymLink() || info.isJunction() || info.fileName()==".git") continue;
                const auto path=canonical(info.absoluteFilePath()); if(!inside(root,path)) continue;
                if(QFileInfo::exists(path+"/.git")) paths.append(path); else pending.append(path);
            }
        }
        return paths;
    }
    GitSetupScript script(const QString &root,bool powershell) {
        GitSetupScript result; const auto relative=powershell?".stagewise/worktree-setup.ps1":".stagewise/worktree-setup.sh";
        result.path=safeChildPath(root,relative); if(result.path.isEmpty()) fail(GitErrorCode::InvalidPath,"Setup script resolves outside the repository.");
        result.exists=QFileInfo::exists(result.path);
        if(result.exists) {
            result.originalBytes=readSetupBytes(root,result.path,cancel);
            QString error;if(!decodeSetupText(result.originalBytes,&result.content,&error))fail(GitErrorCode::IoError,error);
        }
        result.revision={result.exists,result.exists?QCryptographicHash::hash(result.originalBytes,QCryptographicHash::Sha256):QByteArray()};
        return result;
    }
    GitResult dispatch(GitOperation operation,const QString &workspace,const QString &a,const QString &b,bool flag,GitRemovePolicy policy,const GitScriptRevision &expected);
};
}

struct GitService::Private {
    GitServiceOptions options; QThreadPool pool;
    std::shared_ptr<Shared> shared=std::make_shared<Shared>();
    QMap<quint64,Cancellation> requests; quint64 nextId=0;
    struct WatchOwner { QString workspace,common;quint64 request=0; };
    GitRepositoryWatcher *watcher=nullptr;
    QHash<QString,WatchOwner> watchOwners;
    QHash<quint64,QString> watchRequests;
};
GitService::GitService(GitServiceOptions options,QObject *parent):QObject(parent),d(std::make_unique<Private>()) {
    qRegisterMetaType<GitResult>();
    if(options.gitExecutable.isEmpty()) options.gitExecutable=QStandardPaths::findExecutable("git");
    if(options.managedWorktreeRoot.isEmpty()) options.managedWorktreeRoot=QDir::homePath()+"/.ade/worktrees";
    options.managedWorktreeRoot=managedRootAuthority(options.managedWorktreeRoot);
    options.readTimeoutMs=std::clamp(options.readTimeoutMs,50,300000);
    options.mutationTimeoutMs=std::clamp(options.mutationTimeoutMs,50,1200000);
    options.outputLimit=std::clamp<qint64>(options.outputLimit,1024,256*1024*1024);
    d->options=std::move(options); d->pool.setMaxThreadCount(4);
    d->watcher=new GitRepositoryWatcher({},this);
    connect(d->watcher,&GitRepositoryWatcher::topologyChanged,this,&GitService::repositoryTopologyChanged);
    connect(d->watcher,&GitRepositoryWatcher::coverageChanged,this,&GitService::repositoryWatchCoverageChanged);
    connect(this,&GitService::completed,this,&GitService::completeWatch);
}
GitService::~GitService() { cancelAll(); d->pool.waitForDone();delete d->watcher;d->watcher=nullptr; }
void GitService::setWorkspaceContext(const GitWorkspaceContext &context) {
    {QMutexLocker lock(&d->shared->contextMutex);d->shared->context=context;}
    // Revocation removes pending request bookkeeping before its queued result
    // can acquire a watcher. Unchanged authorized owners keep their baseline.
    for(const auto &owner:d->watchOwners.keys())if(!watchAuthorized(d->watchOwners.value(owner).workspace))unwatchWorkspace(owner);
}
bool GitService::watchAuthorized(const QString &workspace) const {
    const QString resolved=canonical(workspace);if(resolved.isEmpty())return false;
    if(inside(currentManagedRoot(d->options.managedWorktreeRoot),resolved))return true;
    GitWorkspaceContext context;{QMutexLocker lock(&d->shared->contextMutex);context=d->shared->context;}
    for(const auto &root:context.trustedRoots+context.mountedPaths)if(key(canonical(root))==key(resolved))return true;
    return false;
}
bool GitService::watchWorkspace(const QString &ownerId,const QString &workspace) {
    auto fail=[&](const QString &reason){emit repositoryWatchError(ownerId,reason);return false;};
    if(ownerId.isEmpty()||ownerId.size()>4096||ownerId.contains(QChar::Null))return fail("A valid repository watcher owner ID is required.");
    if(!watchAuthorized(workspace))return fail("Workspace has not been opened or mounted, or is unavailable.");
    const QString resolved=canonical(workspace);const auto existing=d->watchOwners.constFind(ownerId);
    if(existing!=d->watchOwners.cend()&&key(existing->workspace)==key(resolved)&&(existing->request||!existing->common.isEmpty()))return true;
    if(existing==d->watchOwners.cend()&&d->watchOwners.size()>=4096)return fail("Repository watcher owner limit reached.");
    unwatchWorkspace(ownerId);
    const quint64 request=inspect(resolved);d->watchOwners.insert(ownerId,{resolved,{},request});d->watchRequests.insert(request,ownerId);return true;
}
void GitService::unwatchWorkspace(const QString &ownerId) {
    const auto found=d->watchOwners.find(ownerId);if(found==d->watchOwners.end())return;
    if(found->request){cancel(found->request);d->watchRequests.remove(found->request);}
    d->watchOwners.erase(found);d->watcher->release(ownerId);
}
void GitService::completeWatch(quint64 id,const GitResult &result) {
    const QString owner=d->watchRequests.take(id);if(owner.isEmpty())return;
    auto found=d->watchOwners.find(owner);if(found==d->watchOwners.end()||found->request!=id)return;
    found->request=0;
    if(!result.ok){emit repositoryWatchError(owner,result.error.message);return;}
    if(key(found->workspace)!=key(result.snapshot.repoRoot)||!watchAuthorized(found->workspace)) {
        unwatchWorkspace(owner);emit repositoryWatchError(owner,"Workspace watcher authorization changed during inspection.");return;
    }
    QString error;
    if(!d->watcher->acquire(owner,result.snapshot,&error)){emit repositoryWatchError(owner,error);return;}
    found->common=result.snapshot.commonGitDir;
}
QString GitService::canonicalPath(const QString &path) { return canonical(path); }
QString GitService::pathKey(const QString &path) { const auto resolved=canonical(path); return key(resolved.isEmpty()?path:resolved); }
void GitService::cancel(quint64 id) { const auto flag=d->requests.value(id); if(flag) flag->store(true); }
void GitService::cancelAll() { for(const auto &flag:d->requests) flag->store(true); }
quint64 GitService::submit(GitOperation operation,const QString &workspace,const QString &a,const QString &b,bool flag,GitRemovePolicy policy,const GitScriptRevision &expected,const WorktreeCleanupPermit &permit) {
    const auto id=++d->nextId; const auto cancellation=std::make_shared<std::atomic_bool>(false); d->requests.insert(id,cancellation);
    const QPointer<GitService> receiver(this); const auto options=d->options; const auto shared=d->shared;
    d->pool.start([receiver,options,shared,cancellation,id,operation,workspace,a,b,flag,policy,expected,permit] {
        Work work; work.options=options; work.cancel=cancellation; work.shared=shared; work.cleanupPermit=permit;
        GitResult result; result.operation=operation;
        try { result=work.dispatch(operation,workspace,a,b,flag,policy,expected); }
        catch(const Failure &failure) { result.error=failure.error; }
        catch(const std::exception &) { result.error={GitErrorCode::ProcessFailed,"Unexpected Git operation failure.",-1}; }
        result.mayHaveMutated=work.mutationStarted;
        if(receiver) QMetaObject::invokeMethod(receiver,[receiver,id,result] {
            if(!receiver) return;
            receiver->d->requests.remove(id);
            if(result.ok && result.operation==GitOperation::RemoveWorktree) emit receiver->worktreeRemoved(result.path);
            if(receiver) emit receiver->completed(id,result);
        },Qt::QueuedConnection);
    });
    return id;
}
quint64 GitService::inspect(const QString &w) { return submit(GitOperation::Inspect,w); }
quint64 GitService::status(const QString &w) { return submit(GitOperation::Status,w); }
quint64 GitService::diff(const QString &w) { return submit(GitOperation::Diff,w); }
quint64 GitService::fileDiff(const QString &w,const QString &p,bool staged,const QString &old) { return submit(GitOperation::FileDiff,w,p,old,staged); }
quint64 GitService::branches(const QString &w,bool refresh) { return submit(GitOperation::Branches,w,{},{},refresh); }
quint64 GitService::worktrees(const QString &w) { return submit(GitOperation::Worktrees,w); }
quint64 GitService::switchBranch(const QString &w,const QString &b) { return submit(GitOperation::SwitchBranch,w,b); }
quint64 GitService::createBranch(const QString &w,const QString &b,const QString &source) { return submit(GitOperation::CreateBranch,w,b,source); }
quint64 GitService::createWorktree(const QString &w,const QString &name,const QString &source) { return submit(GitOperation::CreateWorktree,w,name,source); }
quint64 GitService::deletionInfo(const QString &w) { return submit(GitOperation::DeletionInfo,w); }
quint64 GitService::removeWorktree(const QString &w,GitRemovePolicy policy,bool force) { return submit(GitOperation::RemoveWorktree,w,{},{},force,policy); }
quint64 GitService::cleanupCandidates() { return submit(GitOperation::CleanupCandidates); }
quint64 GitService::inspectUsage(const QString &w) { return submit(GitOperation::UsageInspect,w); }
quint64 GitService::scanUsage() { return submit(GitOperation::UsageScan); }
quint64 GitService::removeReviewedWorktree(const WorktreeCleanupPermit &permit) { return submit(GitOperation::RemoveWorktree,permit.record.identity.path,{},{},false,GitRemovePolicy::Cleanup,{},permit); }
quint64 GitService::setupRepositories() { return submit(GitOperation::SetupRepositories); }
quint64 GitService::saveSetupScript(const QString &w,GitSetupVariant variant,const QString &content,const GitScriptRevision &expected) { return submit(GitOperation::SaveSetupScript,w,content,QString::number(int(variant)),variant==GitSetupVariant::PowerShell,GitRemovePolicy::Explicit,expected); }
}

namespace ade { namespace {
GitResult Work::dispatch(GitOperation operation,const QString &workspace,const QString &a,const QString &b,bool flag,GitRemovePolicy policy,const GitScriptRevision &expected) {
    GitResult result; result.operation=operation;
    refreshContext();
    if(options.gitExecutable.isEmpty()) fail(GitErrorCode::ProcessFailed,"System Git is not installed or could not be located.");
    if(cancel->load()) fail(GitErrorCode::Cancelled,"Operation cancelled.");
    if(operation==GitOperation::CleanupCandidates)
        fail(GitErrorCode::UnsafeCleanup,"Cleanup requires a durable usage review and explicit removal permit.");
    if(operation==GitOperation::UsageScan || operation==GitOperation::SetupRepositories) {
        const auto paths=managedPaths(); QStringList roots=paths;
        if(operation==GitOperation::SetupRepositories) roots+=context.trustedRoots+context.mountedPaths;
        QSet<QString> seen;
        for(const auto &path:roots) {
            try {
                auto repo=repository(path); Lock lock(shared->lockFor(repo.commonGitDir),cancel);
                refreshContext(); repo=repository(path);
                if(operation==GitOperation::UsageScan) {
                    if(seen.contains(key(repo.repoRoot)))continue;
                    seen.insert(key(repo.repoRoot));result.usage.append(usageFacts(repo,true));continue;
                }
                if(seen.contains(key(repo.commonGitDir))) continue;
                seen.insert(key(repo.commonGitDir));
                const auto snap=snapshot(repo);
                GitSetupRepository settings; settings.id=repo.repositoryId; settings.mainWorktreePath=snap.mainWorktreePath;
                settings.name=QFileInfo(snap.mainWorktreePath).fileName();settings.trustedWorkspaceAnchor=repo.repoRoot;
                auto readScript=[&](bool powershell){
                    try{return script(snap.mainWorktreePath,powershell);}
                    catch(const Failure &failure){if(failure.error.code==GitErrorCode::Cancelled)throw;GitSetupScript unavailable;unavailable.error=failure.error.message;
                        unavailable.path=safeChildPath(snap.mainWorktreePath,powershell?".stagewise/worktree-setup.ps1":".stagewise/worktree-setup.sh");
                        unavailable.exists=QFileInfo::exists(unavailable.path);result.warnings.append(settings.name+": "+failure.error.message);return unavailable;}
                };
                settings.posix=readScript(false); settings.powershell=readScript(true);
                const auto codexPath=safeChildPath(snap.mainWorktreePath,".codex/environments/environment.toml");
                QString codexError;
                if(!codexPath.isEmpty()) settings.hasCodexSetupScript=!readCodexScript(codexPath,&codexError).isEmpty();
                if(!codexError.isEmpty()) result.warnings.append(settings.name+": "+codexError);
                for(const auto &tree:treeList(repo)) {
                    if(!managed(tree.path) || tree.main) continue;
                    GitManagedWorktree item; item.worktree=tree; item.lastUsedAt=lastUsed(tree.path);
                    try { item.clean=!getStatus(tree.path).dirty; }
                    catch(const Failure &failure) { if(failure.error.code==GitErrorCode::Cancelled) throw; item.disabledReason="Cannot verify worktree status."; }
                    if(mounted(tree.path)) item.disabledReason="Worktree is currently mounted.";
                    else if(tree.detached || tree.branch.isEmpty()) item.disabledReason="Detached worktrees cannot be deleted.";
                    else if(!item.clean) item.disabledReason="Worktree has uncommitted changes or unknown status.";
                    item.removable=item.disabledReason.isEmpty(); settings.worktrees.append(item);
                }
                std::sort(settings.worktrees.begin(),settings.worktrees.end(),[](const auto &x,const auto &y){return x.lastUsedAt>y.lastUsedAt;});
                result.setupRepositories.append(settings);
            } catch(const Failure &failure) {
                if(failure.error.code==GitErrorCode::Cancelled) throw;
                if(operation!=GitOperation::CleanupCandidates || failure.error.code!=GitErrorCode::UnsafeCleanup) result.warnings.append(failure.error.message);
            }
        }
        result.ok=true; return result;
    }
    auto repo=repository(workspace);
    Lock lock(shared->lockFor(repo.commonGitDir),cancel);
    refreshContext(); repo=repository(workspace); // Earlier queued mutations or host mounts may have changed the scope.
    result.path=repo.repoRoot;
    switch(operation) {
    case GitOperation::Inspect: result.snapshot=snapshot(repo); break;
    case GitOperation::UsageInspect: result.usage.append(usageFacts(repo,false)); break;
    case GitOperation::Status: result.status=getStatus(repo.repoRoot); break;
    case GitOperation::Diff: result.diff=diffSummary(repo); break;
    case GitOperation::FileDiff: result.fileDiff=content(repo,a,flag,b); break;
    case GitOperation::Branches: result.branches=branchList(repo,flag); break;
    case GitOperation::Worktrees: result.worktrees=treeList(repo); break;
    case GitOperation::DeletionInfo: result.deletion=deletion(repo); break;
    case GitOperation::SwitchBranch: {
        const auto source=sourceBranch(repo,a);
        if(source.remote) fail(GitErrorCode::BranchNotFound,"Only local branches can be checked out directly.");
        if(source.checkedOut && key(source.checkedOutPath)!=key(repo.repoRoot)) fail(GitErrorCode::BranchCheckedOut,"Branch is checked out in another worktree.");
        checked(repo.repoRoot,{"checkout","--no-guess",source.name,"--"},true);
        result.branch=source.name; snapshotAfterMutation(result,repo); break;
    }
    case GitOperation::CreateBranch: {
        const auto name=validNewName(repo,a); const auto source=sourceBranch(repo,b); const auto ref=sourceRef(repo,source);
        checked(repo.repoRoot,{"checkout","--no-guess","-b",name,ref,"--"},true);
        result.branch=name; snapshotAfterMutation(result,repo); break;
    }
    case GitOperation::CreateWorktree: {
        const auto name=validNewName(repo,a); const auto source=sourceBranch(repo,b); const auto snap=snapshot(repo);
        const auto ref=sourceRef(repo,source);
        if(!QDir().mkpath(options.managedWorktreeRoot)) fail(GitErrorCode::IoError,"Cannot create managed worktree directory.");
        const auto root=managedRoot(); if(root.isEmpty()) fail(GitErrorCode::InvalidPath,"Managed worktree directory is unavailable.");
        const auto hash=QString::fromLatin1(QCryptographicHash::hash(repo.repositoryId.toUtf8(),QCryptographicHash::Sha256).toHex().left(12));
        const QString relative=hash+'/'+name;
        auto target=safeChildPath(root,relative);
        if(target.isEmpty()) fail(GitErrorCode::InvalidPath,"Worktree target escapes the managed directory.");
        if(QFileInfo::exists(target) || QFileInfo(target).isSymLink() || QFileInfo(target).isJunction()) fail(GitErrorCode::WorktreeExists,"Worktree directory already exists.");
        if(!QDir().mkpath(QFileInfo(target).absolutePath())) fail(GitErrorCode::IoError,"Cannot create worktree parent directory.");
        target=safeChildPath(root,relative); if(target.isEmpty()) fail(GitErrorCode::InvalidPath,"Worktree target changed outside the managed directory.");
        QStringList args{"worktree","add","-b",name}; if(source.remote) args<<"--no-track";
        args<<"--"<<target<<ref; checked(snap.mainWorktreePath,args,true);
        result.path=canonical(target); result.branch=name;
        result.setup={result.path,repo.repoRoot,snap.mainWorktreePath};
        auto createdRepo=repo; createdRepo.repoRoot=result.path;
        snapshotAfterMutation(result,createdRepo); break;
    }
    case GitOperation::RemoveWorktree: {
        refreshContext();
        const auto info=deletion(repo); result.deletion=info; result.branch=info.branch;
        if(info.main) fail(GitErrorCode::MainWorktree,"Cannot delete the main worktree.");
        if(flag && policy!=GitRemovePolicy::Explicit) fail(GitErrorCode::InvalidPath,"Force removal is only available for explicit worktree deletion.");
        if(policy!=GitRemovePolicy::Explicit) {
            if(!managed(repo.repoRoot)) fail(GitErrorCode::UntrustedPath,"Worktree is not ADE-managed.");
            if(info.mounted) fail(GitErrorCode::MountedWorktree,"Worktree is currently mounted.");
            if(info.detached || info.branch.isEmpty()) fail(GitErrorCode::UnsafeCleanup,"Detached worktrees cannot be removed by this action.");
        }
        if((!info.status.known || info.status.dirty) && !flag) fail(GitErrorCode::DirtyWorktree,"Worktree has uncommitted changes. Confirm force deletion.");
        if(policy==GitRemovePolicy::Cleanup) result.cleanup.append(cleanup(repo));
        QStringList args{"worktree","remove"}; if(flag) args<<"--force"; args<<"--"<<repo.repoRoot;
        // Run from the main root; Git must not remove its own process current directory.
        QString main; for(const auto &tree:treeList(repo)) if(tree.main) main=tree.path;
        if(main.isEmpty()) fail(GitErrorCode::NotRepository,"Main worktree is unavailable.");
        if(policy==GitRemovePolicy::Cleanup) {
            // Recheck after the last Git subprocess; mounted contexts can change
            // while earlier status/merge commands were running.
            refreshContext();const auto fresh=usageFacts(repo,false);
            if(fresh.identity.key()!=cleanupPermit.record.identity.key()||fresh.branch!=cleanupPermit.branch||fresh.headSha!=cleanupPermit.headSha||fresh.busy||fresh.mounted||!fresh.statusKnown||fresh.dirty)
                fail(GitErrorCode::UnsafeCleanup,"Worktree changed after review.");
            verifyCleanupPermit();
        }
        checked(main,args,true); break;
    }
    case GitOperation::SaveSetupScript: {
        if(b!="0"&&b!="1")fail(GitErrorCode::InvalidPath,"Unknown setup script variant.");
        const auto snap=snapshot(repo);
        const auto main=repository(snap.mainWorktreePath,false);
        if(key(main.commonGitDir)!=key(repo.commonGitDir))fail(GitErrorCode::InvalidPath,"Main worktree no longer belongs to the trusted repository.");
        if((expected.exists&&expected.sha256.size()!=32)||(!expected.exists&&!expected.sha256.isEmpty()))fail(GitErrorCode::InvalidPath,"Setup script baseline revision is invalid.");
        const auto old=script(main.repoRoot,flag);
        if(old.revision!=expected)fail(GitErrorCode::Conflict,"Setup script changed outside this editor. Reload it before saving; your draft was preserved.");
        QByteArray bytes;QString encodingError;
        if(!encodeSetupText(old.originalBytes,a,&bytes,&encodingError))fail(GitErrorCode::IoError,encodingError);
        requireNotCancelled(cancel);
        if(!QDir().mkpath(QFileInfo(old.path).absolutePath())) fail(GitErrorCode::IoError,"Cannot create setup script directory.");
        const auto fresh=script(main.repoRoot,flag);
        if(fresh.revision!=expected)fail(GitErrorCode::Conflict,"Setup script changed while saving. Reload it before saving; your draft was preserved.");
        QSaveFile file(fresh.path); file.setDirectWriteFallback(false);
        requireNotCancelled(cancel);
        if(!file.open(QIODevice::WriteOnly))fail(GitErrorCode::IoError,file.errorString());
#ifdef Q_OS_WIN
        if(file.handle()<0||!inside(main.repoRoot,openedPath(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())))))fail(GitErrorCode::InvalidPath,"Opened setup-script temporary file resolves outside the repository.");
#endif
        if(file.write(bytes)!=bytes.size())fail(GitErrorCode::IoError,file.errorString());
        requireNotCancelled(cancel);
        mutationStarted=true;
        if(!file.commit()) fail(GitErrorCode::IoError,file.errorString());
#ifndef Q_OS_WIN
        if(!flag) QFile::setPermissions(fresh.path,QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner|QFileDevice::ReadGroup|QFileDevice::ExeGroup|QFileDevice::ReadOther|QFileDevice::ExeOther);
#endif
        result.path=fresh.path;result.setupScript=fresh;result.setupScript.exists=true;result.setupScript.content=a;result.setupScript.originalBytes=bytes;result.setupScript.revision={true,QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)}; break;
    }
    default: fail(GitErrorCode::ProcessFailed,"Unsupported Git operation.");
    }
    result.ok=true; return result;
}
}}
