#include "WorktreeSetupRunner.h"
#include "ProcessRunner.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QThreadPool>
#include <algorithm>

namespace ade {
using namespace gitdetail;
struct WorktreeSetupRunner::Private {
    struct Active { quint64 id=0; Cancellation cancel; };
    QMap<QString,Active> active; QThreadPool pool; quint64 nextId=0;
    int timeoutMs=20*60*1000; QString shell;
};
WorktreeSetupRunner::WorktreeSetupRunner(QObject *parent):QObject(parent),d(std::make_unique<Private>()) {
    qRegisterMetaType<WorktreeSetupRun>(); d->pool.setMaxThreadCount(4);
}
WorktreeSetupRunner::~WorktreeSetupRunner() { cancelAll(); d->pool.waitForDone(); }
void WorktreeSetupRunner::setTimeoutMs(int timeoutMs) { d->timeoutMs=std::clamp(timeoutMs,50,20*60*1000); }
void WorktreeSetupRunner::setShellExecutable(const QString &executable) { d->shell=executable; }
void WorktreeSetupRunner::cancel(const QString &path) {
    const auto active=d->active.value(key(canonical(path))); if(active.cancel) active.cancel->store(true);
}
void WorktreeSetupRunner::cancelAll() { for(const auto &active:d->active) active.cancel->store(true); }
quint64 WorktreeSetupRunner::startAfterMount(const WorktreeSetupMetadata &metadata) {
    WorktreeSetupMetadata resolved{canonical(metadata.workspacePath),canonical(metadata.sourceWorktreePath),canonical(metadata.mainWorktreePath)};
    const auto pathKey=key(resolved.workspacePath);
    if(d->active.contains(pathKey)) return d->active.value(pathKey).id;
    const auto id=++d->nextId; const auto cancel=std::make_shared<std::atomic_bool>(false);
    d->active.insert(pathKey,{id,cancel});
    const auto timeout=d->timeoutMs; const auto overrideShell=d->shell; const QPointer<WorktreeSetupRunner> receiver(this);
    d->pool.start([resolved,pathKey,id,cancel,timeout,overrideShell,receiver] {
        WorktreeSetupRun run; run.id=id; run.metadata=resolved; run.startedAt=QDateTime::currentMSecsSinceEpoch();
        auto post=[&](bool final=false) {
            if(receiver) QMetaObject::invokeMethod(receiver,[receiver,pathKey,run,final] {
                if(!receiver) return;
                const auto found=receiver->d->active.constFind(pathKey);
                if(found==receiver->d->active.cend() || found->id!=run.id) return;
                if(final) receiver->d->active.remove(pathKey);
                emit receiver->updated(run);
            },Qt::QueuedConnection);
        };
        auto finish=[&](WorktreeSetupRun::Status status,const QString &message,int code=-1) {
            run.status=status; run.message=message; run.exitCode=code; run.finishedAt=QDateTime::currentMSecsSinceEpoch(); post(true);
        };
        if(resolved.workspacePath.isEmpty() || resolved.sourceWorktreePath.isEmpty() || resolved.mainWorktreePath.isEmpty()) {
            finish(WorktreeSetupRun::Status::Failed,"Worktree setup paths are unavailable."); return;
        }
        auto find=[&](const QString &relative,QString *error) -> QString {
            for(const auto &root:QStringList{resolved.workspacePath,resolved.mainWorktreePath}) {
                const auto path=safeChildPath(root,relative);
                if(path.isEmpty()) { *error="Setup configuration resolves outside its worktree."; return {}; }
                const QFileInfo info(path);
                if(!info.exists()) continue;
                if(!info.isFile() || info.size()>1024*1024) { *error="Setup configuration is not a file or exceeds 1 MiB."; return {}; }
                return path;
            }
            return {};
        };
        QString error,inlineScript;
#ifdef Q_OS_WIN
        run.scriptPath=find(".stagewise/worktree-setup.ps1",&error);
#else
        run.scriptPath=find(".stagewise/worktree-setup.sh",&error);
#endif
        bool codex=false;
        if(run.scriptPath.isEmpty() && error.isEmpty()) {
            run.scriptPath=find(".codex/environments/environment.toml",&error);
            if(!run.scriptPath.isEmpty()) { inlineScript=readCodexScript(run.scriptPath,&error); codex=true; }
        }
        if(!error.isEmpty()) { finish(WorktreeSetupRun::Status::Failed,error); return; }
        if(inlineScript.contains(QChar::Null)) { finish(WorktreeSetupRun::Status::Failed,"Setup script contains a null character."); return; }
        if(run.scriptPath.isEmpty() || (codex && inlineScript.isEmpty())) { finish(WorktreeSetupRun::Status::Skipped,"No worktree setup script is configured."); return; }
        if(cancel->load()) { finish(WorktreeSetupRun::Status::Failed,"Worktree setup was interrupted."); return; }
        auto environment=safeEnvironment();
        auto set=[&](const QString &name,const QString &value) {
            for(const auto &old:environment.keys()) if(old.compare(name,Qt::CaseInsensitive)==0) environment.remove(old);
            environment.insert(name,value);
        };
        set("STAGEWISE_SOURCE_WORKTREE_PATH",resolved.sourceWorktreePath);
        set("STAGEWISE_TARGET_WORKTREE_PATH",resolved.workspacePath);
        set("STAGEWISE_MAIN_WORKTREE_PATH",resolved.mainWorktreePath);
        if(codex) { set("CODEX_SOURCE_TREE_PATH",resolved.sourceWorktreePath); set("CODEX_WORKTREE_PATH",resolved.workspacePath); }
        QString program=overrideShell; QStringList args;
#ifdef Q_OS_WIN
        if(program.isEmpty()) program=QStandardPaths::findExecutable("pwsh");
        if(program.isEmpty()) program=QStandardPaths::findExecutable("powershell.exe");
        args={"-NoProfile","-NonInteractive"};
        if(codex) args<<"-Command"<<inlineScript+"\nexit $LASTEXITCODE";
        else args<<"-ExecutionPolicy"<<"Bypass"<<"-File"<<run.scriptPath;
#else
        if(program.isEmpty()) program=codex?QStringLiteral("/bin/bash"):QStringLiteral("/bin/sh");
        if(codex) args={"-xeo","pipefail","-c",inlineScript}; else args={run.scriptPath};
#endif
        if(program.isEmpty()) { finish(WorktreeSetupRun::Status::Failed,"No setup shell could be located."); return; }
        post(); QElapsedTimer flush; flush.start();
        QStringDecoder outDecoder(QStringDecoder::Utf8), errDecoder(QStringDecoder::Utf8);
        auto tail=[](QString &destination,QString text) {
            destination+=text;
            if(destination.size()>12000) {
                destination=destination.right(12000);
                if(!destination.isEmpty() && destination.front().isLowSurrogate()) destination.remove(0,1);
            }
        };
        const auto result=runProcess(program,args,resolved.workspacePath,environment,cancel,timeout,48000,
            [&](const QByteArray &out,const QByteArray &err) {
                tail(run.stdoutTail,outDecoder(out)); tail(run.stderrTail,errDecoder(err));
                if(flush.elapsed()>=150) { post(); flush.restart(); }
            },true);
        if(result.cancelled) finish(WorktreeSetupRun::Status::Failed,"Worktree setup was interrupted.");
        else if(result.timedOut) finish(WorktreeSetupRun::Status::Failed,"Worktree setup timed out.");
        else if(!result.error.isEmpty()) finish(WorktreeSetupRun::Status::Failed,"Failed to run worktree setup: "+result.error,result.exitCode);
        else if(result.exitCode!=0) finish(WorktreeSetupRun::Status::Failed,QString("Worktree setup exited with code %1.").arg(result.exitCode),result.exitCode);
        else finish(WorktreeSetupRun::Status::Succeeded,{},0);
    });
    return id;
}
}
