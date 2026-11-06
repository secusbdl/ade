#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include "git/GitService.h"
#include "git/GitParsing.h"
#include "git/ProcessRunner.h"
#include "git/WorktreeSetupRunner.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace ade;
using namespace Qt::Literals::StringLiterals;
namespace {
bool write(const QString &path,const QByteArray &bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size();
}
QByteArray read(const QString &path) { QFile file(path); if(!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
struct Fixture {
    QTemporaryDir base; QString root,managed,git,error;
    Fixture(bool commit=true) {
        root=base.filePath("repository with spaces"); managed=base.filePath("managed"); git=QStandardPaths::findExecutable("git");
        QDir().mkpath(root);
        if(!run({"init","-b","main"})) return;
        run({"config","user.name","ADE fixture"}); run({"config","user.email","fixture@example.invalid"});
        run({"config","commit.gpgsign","false"}); run({"config","core.autocrlf","false"});
        run({"config","core.hooksPath",base.filePath("hooks")});
        if(commit) { write(root+"/one.txt","base\n"); write(root+"/two.txt","two\n"); run({"add","--","."}); run({"commit","-m","base"}); }
    }
    bool run(const QStringList &args,const QString &cwd={},QByteArray *out=nullptr) {
        const auto dir=cwd.isEmpty()?root:cwd;
        if(!gitdetail::inside(base.path(),dir,true)) { error="Fixture command escaped temporary root."; return false; }
        auto env=gitdetail::safeEnvironment(); env.insert("GIT_CONFIG_NOSYSTEM","1"); env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global-config"));
        QProcess process; process.setProcessEnvironment(env); process.setWorkingDirectory(dir); process.start(git,args);
        if(!process.waitForFinished(15000) || process.exitCode()!=0) { error=QString::fromUtf8(process.readAllStandardError()); return false; }
        if(out) *out=process.readAllStandardOutput();
        return true;
    }
    GitServiceOptions options() const { GitServiceOptions o; o.gitExecutable=git; o.managedWorktreeRoot=managed; return o; }
    GitWorkspaceContext context() const { GitWorkspaceContext c; c.trustedRoots={root}; c.usageKnown=true; return c; }
};
GitResult waitFor(GitService &service,quint64 id,int timeout=20000) {
    QSignalSpy spy(&service,&GitService::completed); QElapsedTimer timer; timer.start();
    do {
        for(const auto &row:spy) if(row[0].toULongLong()==id) return qvariant_cast<GitResult>(row[1]);
        QCoreApplication::processEvents(); QTest::qWait(5);
    } while(timer.elapsed()<timeout);
    GitResult result; result.error={GitErrorCode::Timeout,"Test wait timed out.",-1}; return result;
}
WorktreeSetupRun waitSetup(QSignalSpy &spy,quint64 id,int timeout=15000) {
    QElapsedTimer timer; timer.start();
    do {
        for(const auto &row:spy) { const auto run=qvariant_cast<WorktreeSetupRun>(row[0]); if(run.id==id && run.status!=WorktreeSetupRun::Status::Running) return run; }
        QCoreApplication::processEvents(); QTest::qWait(5);
    } while(timer.elapsed()<timeout);
    WorktreeSetupRun run; run.message="Test wait timed out."; return run;
}
bool ok(const GitResult &result) { if(!result.ok) qWarning().noquote()<<result.error.message; return result.ok; }
}
class GitTest:public QObject {
    Q_OBJECT
private slots:
    void nulDelimitedParsers();
    void regularUnbornAndLinkedRoots();
    void windowsLongWorktreePaths();
    void statusDiffAndRawBytes();
    void renameAndStagedDeletionRecreated();
    void dirtyCheckoutProtection();
    void branchAndWorktreeNames();
    void removalPolicies();
    void cleanupRules();
    void detachedUnmanagedAndLockedWorktrees();
    void remoteRefreshAndSourceFetch();
    void remoteUrlsAndEnvironment();
    void postMutationRefreshFailure();
    void junctionEscape();
    void queuedMutationsAndCancellation();
    void processBoundsAndTeardown();
    void setupPersistenceAndExecution();
    void setupCodexFallbackAndErrors();
    void setupCancellationAndOutputTails();
};
void GitTest::nulDelimitedParsers() {
    GitStatus status;
    QVERIFY(gitdetail::parseStatus("MM new\nline\0?? odd\tfile\0R  new name\0old name\0"_ba,&status));
    QCOMPARE(status.stagedCount,2); QCOMPARE(status.unstagedCount,1); QCOMPARE(status.untrackedCount,1);
    QVERIFY(!gitdetail::parseStatus(" M missing-null",&status));
    QList<GitWorktree> trees;
    const QByteArray tree="worktree /repo\nname\0HEAD 123\0branch refs/heads/main\0\0"_ba;
    QVERIFY(gitdetail::parseWorktrees(tree,&trees)); QCOMPARE(trees.size(),1); QCOMPARE(trees[0].path,"/repo\nname"); QCOMPARE(trees[0].branch,"main");
    QList<GitDiffEntry> entries;
    QVERIFY(gitdetail::parseDiff("2\t1\t\0old\nname\0new\tname\0"_ba,"R100\0old\nname\0new\tname\0"_ba,true,&entries));
    QCOMPARE(entries.size(),1); QCOMPARE(entries[0].path,"new\tname"); QCOMPARE(entries[0].oldPath,"old\nname"); QCOMPARE(entries[0].added,2);
    QVERIFY(!gitdetail::parseDiff("2\t1\t\0old\0"_ba,""_ba,false,&entries));
}
void GitTest::regularUnbornAndLinkedRoots() {
    Fixture f; QVERIFY2(f.error.isEmpty(),qPrintable(f.error)); GitService service(f.options()); service.setWorkspaceContext(f.context());
    auto result=waitFor(service,service.inspect(f.root)); QVERIFY(ok(result)); QCOMPARE(result.snapshot.branch,"main"); QVERIFY(!result.snapshot.isWorktree); QVERIFY(!result.snapshot.status.dirty);
    QDir().mkpath(f.root+"/nested"); auto context=f.context(); context.trustedRoots<<f.root+"/nested"; service.setWorkspaceContext(context);
    result=waitFor(service,service.inspect(f.root+"/nested")); QCOMPARE(result.error.code,GitErrorCode::NotRepository);
    result=waitFor(service,service.createWorktree(f.root,"linked/topic","main")); QVERIFY(ok(result));
    const auto linked=result.path; QVERIFY(QFileInfo::exists(linked+"/.git")); QCOMPARE(result.setup.sourceWorktreePath,GitService::canonicalPath(f.root));
    result=waitFor(service,service.inspect(linked)); QVERIFY(ok(result)); QVERIFY(result.snapshot.isWorktree); QCOMPARE(result.snapshot.branch,"linked/topic");
    result=waitFor(service,service.worktrees(f.root)); QVERIFY(ok(result)); QCOMPARE(result.worktrees.size(),2);
    QCOMPARE(waitFor(service,service.switchBranch(f.root,"linked/topic")).error.code,GitErrorCode::BranchCheckedOut);
    Fixture unborn(false); GitService empty(unborn.options()); empty.setWorkspaceContext(unborn.context());
    result=waitFor(empty,empty.inspect(unborn.root)); QVERIFY(ok(result)); QVERIFY(!result.snapshot.status.dirty);
}
void GitTest::windowsLongWorktreePaths() {
#ifndef Q_OS_WIN
    QSKIP("Git for Windows long-path command policy.");
#else
    Fixture f; QVERIFY2(f.error.isEmpty(),qPrintable(f.error));
    QVERIFY(f.run({"config","core.longpaths","false"}));
    QVERIFY(f.run({"branch","before-long-file","main"}));
    const auto relative=QString("nested/")+QString(80,'x')+".txt";
    const QByteArray original="Deep worktree path bytes\r\nlast";
    QVERIFY(f.root.size()+1+relative.size()<260);
    QVERIFY(write(f.root+'/'+relative,original)); QVERIFY(f.run({"add","--",relative})); QVERIFY(f.run({"commit","-m","deep tracked file"}));
    const auto configBefore=read(f.root+"/.git/config"); QVERIFY(!configBefore.isEmpty());
    // Keep the worktree root below Git's separate explicit $GIT_DIR bound;
    // the tracked file itself must exceed MAX_PATH after checkout.
    auto options=f.options(); options.managedWorktreeRoot=f.base.path()+"/managed-"+QString(qMax(1,160-int(f.base.path().size())),'m');
    GitService service(options); service.setWorkspaceContext(f.context());
    auto created=waitFor(service,service.createWorktree(f.root,"windows-long-path","main")); QVERIFY(ok(created));
    const auto linked=created.path; const auto longFile=linked+'/'+relative; QVERIFY(longFile.size()>260);
    QCOMPARE(read(longFile),original); QVERIFY(created.snapshot.isWorktree); QVERIFY(!created.snapshot.status.dirty);
    auto content=waitFor(service,service.fileDiff(linked,relative,false)); QVERIFY(ok(content)); QCOMPARE(content.fileDiff.original,original); QCOMPARE(content.fileDiff.modified,original);
    auto switched=waitFor(service,service.switchBranch(linked,"before-long-file")); QVERIFY(ok(switched)); QVERIFY(!QFileInfo::exists(longFile));
    switched=waitFor(service,service.switchBranch(linked,"windows-long-path")); QVERIFY(ok(switched)); QCOMPARE(read(longFile),original);
    const QByteArray changed="Changed long-path bytes\n"; QVERIFY(write(longFile,changed));
    auto state=waitFor(service,service.status(linked)); QVERIFY(ok(state)); QVERIFY(state.status.known); QVERIFY(state.status.dirty); QCOMPARE(state.status.unstagedCount,1);
    QCOMPARE(waitFor(service,service.removeWorktree(linked)).error.code,GitErrorCode::DirtyWorktree); QCOMPARE(read(longFile),changed);
    QVERIFY(write(longFile,original)); state=waitFor(service,service.status(linked)); QVERIFY(ok(state)); QVERIFY(!state.status.dirty);
    QVERIFY(ok(waitFor(service,service.removeWorktree(linked)))); QVERIFY(!QFileInfo::exists(linked));
    const auto trees=waitFor(service,service.worktrees(f.root)); QVERIFY(ok(trees)); QCOMPARE(trees.worktrees.size(),1);
    QCOMPARE(read(f.root+"/.git/config"),configBefore);
#endif
}
void GitTest::statusDiffAndRawBytes() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context());
    QVERIFY(write(f.root+"/.gitignore","ignored.tmp\n")); QVERIFY(f.run({"add","--",".gitignore"})); QVERIFY(f.run({"commit","-m","ignore"}));
    QVERIFY(write(f.root+"/ignored.tmp","keep ignored\n")); QVERIFY(write(f.root+"/one.txt","staged\n")); QVERIFY(f.run({"add","--","one.txt"}));
    QVERIFY(write(f.root+"/one.txt","working\r\nlast")); QVERIFY(write(f.root+"/new ü.txt",QString::fromUtf8("héllo\nlast").toUtf8()));
    QVERIFY(write(f.root+"/binary.bin",QByteArray("a\0b",3)));
    auto result=waitFor(service,service.status(f.root)); QVERIFY(ok(result)); QCOMPARE(result.status.stagedCount,1); QCOMPARE(result.status.unstagedCount,1); QCOMPARE(result.status.untrackedCount,2);
    result=waitFor(service,service.fileDiff(f.root,"one.txt",true)); QVERIFY(ok(result)); QCOMPARE(result.fileDiff.original,"base\n"); QCOMPARE(result.fileDiff.modified,"staged\n"); QCOMPARE(result.fileDiff.mtimeMs,-1);
    result=waitFor(service,service.fileDiff(f.root,"one.txt",false)); QVERIFY(ok(result)); QCOMPARE(result.fileDiff.original,"base\n"); QCOMPARE(result.fileDiff.modified,"working\r\nlast"); QVERIFY(result.fileDiff.mtimeMs>0);
    result=waitFor(service,service.diff(f.root)); QVERIFY(ok(result)); QCOMPARE(result.diff.entries.size(),3);
    bool binary=false; for(const auto &entry:result.diff.entries) { QVERIFY(entry.path!="ignored.tmp"); if(entry.path=="binary.bin") { binary=true; QVERIFY(entry.binary); QCOMPARE(entry.added,0); } }
    QVERIFY(binary);
    const auto utf16=QByteArray::fromHex("fffe61000d000a00b103"); QVERIFY(write(f.root+"/utf16.txt",utf16));
    result=waitFor(service,service.fileDiff(f.root,"utf16.txt",false)); QVERIFY(ok(result)); QCOMPARE(result.fileDiff.modified,utf16); QVERIFY(!result.fileDiff.originalExists);
    QCOMPARE(waitFor(service,service.fileDiff(f.root,"../outside.txt",false)).error.code,GitErrorCode::InvalidPath);
}
void GitTest::renameAndStagedDeletionRecreated() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context());
    QVERIFY(f.run({"mv","--","one.txt","renamed ü.txt"}));
    auto result=waitFor(service,service.diff(f.root)); QVERIFY(ok(result)); QCOMPARE(result.diff.entries.size(),1); QCOMPARE(result.diff.entries[0].oldPath,"one.txt");
    result=waitFor(service,service.fileDiff(f.root,"renamed ü.txt",true,"one.txt")); QVERIFY(ok(result)); QCOMPARE(result.fileDiff.original,"base\n"); QCOMPARE(result.fileDiff.modified,"base\n");
    QVERIFY(f.run({"rm","--","two.txt"})); QVERIFY(write(f.root+"/two.txt","replacement\n"));
    result=waitFor(service,service.diff(f.root)); QVERIFY(ok(result));
    bool found=false; for(const auto &entry:result.diff.entries) if(entry.path=="two.txt") { found=true; QCOMPARE(entry.status,'M'); QVERIFY(entry.staged); QCOMPARE(entry.added,1); QCOMPARE(entry.deleted,1); }
    QVERIFY(found);
}
void GitTest::dirtyCheckoutProtection() {
    Fixture f;
    QVERIFY(f.run({"checkout","-b","other"})); QVERIFY(write(f.root+"/one.txt","other\n")); QVERIFY(f.run({"commit","-am","other"})); QVERIFY(f.run({"checkout","main"}));
    GitService service(f.options()); service.setWorkspaceContext(f.context());
    QVERIFY(write(f.root+"/one.txt","unsaved user bytes\n"));
    auto result=waitFor(service,service.switchBranch(f.root,"other")); QVERIFY(!result.ok); QCOMPARE(read(f.root+"/one.txt"),"unsaved user bytes\n");
    QVERIFY(write(f.root+"/one.txt","base\n")); QVERIFY(write(f.root+"/two.txt","non-overlapping local change\n"));
    result=waitFor(service,service.switchBranch(f.root,"other")); QVERIFY(ok(result)); QCOMPARE(result.snapshot.branch,"other"); QCOMPARE(read(f.root+"/two.txt"),"non-overlapping local change\n");
}
void GitTest::branchAndWorktreeNames() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context());
    for(const auto &name:QStringList{"-B","../escape","@{-1}","has space","CON","a:stream","bad\\name"}) {
        const auto result=waitFor(service,service.createWorktree(f.root,name,"main")); QVERIFY2(!result.ok,qPrintable(name));
    }
    auto result=waitFor(service,service.createBranch(f.root,"feature;literal&name","main")); QVERIFY(ok(result)); QCOMPARE(result.snapshot.branch,"feature;literal&name");
    QCOMPARE(waitFor(service,service.createBranch(f.root,"feature;literal&name","main")).error.code,GitErrorCode::BranchExists);
    QCOMPARE(waitFor(service,service.createWorktree(f.root,"good","--upload-pack=bad")).error.code,GitErrorCode::BranchNotFound);
    QVERIFY(!QFileInfo::exists(f.base.filePath("escape")));
}
void GitTest::removalPolicies() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context()); QSignalSpy removed(&service,&GitService::worktreeRemoved);
    QCOMPARE(waitFor(service,service.removeWorktree(f.root,GitRemovePolicy::Explicit,true)).error.code,GitErrorCode::MainWorktree);
    auto created=waitFor(service,service.createWorktree(f.root,"remove-me","main")); QVERIFY(ok(created)); const auto path=created.path;
    QVERIFY(write(path+"/untracked.txt","precious"));
    QCOMPARE(waitFor(service,service.removeWorktree(path)).error.code,GitErrorCode::DirtyWorktree); QVERIFY(QFileInfo::exists(path+"/untracked.txt")); QCOMPARE(removed.size(),0);
    QCOMPARE(waitFor(service,service.removeWorktree(path,GitRemovePolicy::Settings,true)).error.code,GitErrorCode::InvalidPath);
    QVERIFY(ok(waitFor(service,service.removeWorktree(path,GitRemovePolicy::Explicit,true)))); QVERIFY(!QFileInfo::exists(path)); QCOMPARE(removed.size(),1);
    QByteArray branches; QVERIFY(f.run({"branch","--list","remove-me"},{},&branches)); QVERIFY(branches.contains("remove-me"));
    created=waitFor(service,service.createWorktree(f.root,"mounted","main")); QVERIFY(ok(created)); auto context=f.context(); context.mountedPaths={created.path}; service.setWorkspaceContext(context);
    QCOMPARE(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Settings)).error.code,GitErrorCode::MountedWorktree);
    service.setWorkspaceContext(f.context()); QVERIFY(ok(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Settings))));
    // Git itself defines ignored-file removal behavior; record the successful safe path.
    QVERIFY(write(f.root+"/.gitignore","ignored.tmp\n")); QVERIFY(f.run({"add","."})); QVERIFY(f.run({"commit","-m","ignore"}));
    created=waitFor(service,service.createWorktree(f.root,"ignored","main")); QVERIFY(ok(created)); QVERIFY(write(created.path+"/ignored.tmp","ignored artifact"));
    auto info=waitFor(service,service.deletionInfo(created.path)); QVERIFY(ok(info)); QVERIFY(!info.deletion.status.dirty);
    auto deletion=waitFor(service,service.removeWorktree(created.path)); QVERIFY(ok(deletion));
}
void GitTest::cleanupRules() {
    Fixture f; GitService service(f.options()); auto context=f.context(); service.setWorkspaceContext(context);
    auto created=waitFor(service,service.createWorktree(f.root,"merged","main")); QVERIFY(ok(created));
    auto result=waitFor(service,service.cleanupCandidates()); QVERIFY(ok(result)); QCOMPARE(result.cleanup.size(),1); QCOMPARE(result.cleanup[0].mergedInto,"main");
    context.lastUsedAt[created.path]=QDateTime::currentMSecsSinceEpoch(); service.setWorkspaceContext(context);
    result=waitFor(service,service.cleanupCandidates()); QVERIFY(ok(result)); QVERIFY(result.cleanup.isEmpty());
    QCOMPARE(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Cleanup)).error.code,GitErrorCode::UnsafeCleanup);
    context.lastUsedAt[created.path]=QDateTime::currentMSecsSinceEpoch()-8LL*24*60*60*1000; context.usageKnown=false; service.setWorkspaceContext(context);
    result=waitFor(service,service.cleanupCandidates()); QVERIFY(ok(result)); QVERIFY(result.cleanup.isEmpty());
    context.usageKnown=true; service.setWorkspaceContext(context); QVERIFY(write(created.path+"/late.txt","do not lose"));
    QCOMPARE(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Cleanup)).error.code,GitErrorCode::DirtyWorktree);
    QVERIFY(QFile::remove(created.path+"/late.txt")); QVERIFY(ok(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Cleanup))));
    created=waitFor(service,service.createWorktree(f.root,"unmerged","main")); QVERIFY(ok(created));
    QVERIFY(write(created.path+"/new-commit.txt","unique commit")); QVERIFY(f.run({"add","."},created.path)); QVERIFY(f.run({"commit","-m","unmerged"},created.path));
    result=waitFor(service,service.cleanupCandidates()); QVERIFY(ok(result)); QVERIFY(result.cleanup.isEmpty());
    QCOMPARE(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Cleanup)).error.code,GitErrorCode::UnsafeCleanup);
}
void GitTest::detachedUnmanagedAndLockedWorktrees() {
    Fixture f; GitService service(f.options()); auto context=f.context(); service.setWorkspaceContext(context);
    auto created=waitFor(service,service.createWorktree(f.root,"detachable","main")); QVERIFY(ok(created)); QVERIFY(f.run({"checkout","--detach"},created.path));
    auto result=waitFor(service,service.deletionInfo(created.path)); QVERIFY(ok(result)); QVERIFY(result.deletion.detached);
    QCOMPARE(waitFor(service,service.removeWorktree(created.path,GitRemovePolicy::Settings)).error.code,GitErrorCode::UnsafeCleanup);
    QVERIFY(ok(waitFor(service,service.removeWorktree(created.path))));
    const auto outside=f.base.filePath("external linked"); QVERIFY(f.run({"worktree","add","-b","external",outside,"main"}));
    QCOMPARE(waitFor(service,service.inspect(outside)).error.code,GitErrorCode::UntrustedPath);
    context.trustedRoots<<outside; service.setWorkspaceContext(context);
    QVERIFY(ok(waitFor(service,service.inspect(outside))));
    QCOMPARE(waitFor(service,service.removeWorktree(outside,GitRemovePolicy::Settings)).error.code,GitErrorCode::UntrustedPath);
    QVERIFY(f.run({"worktree","lock","--reason","fixture",outside}));
    auto locked=waitFor(service,service.removeWorktree(outside,GitRemovePolicy::Explicit,true)); QVERIFY(!locked.ok); QVERIFY(locked.mayHaveMutated); QVERIFY(QFileInfo::exists(outside));
    QVERIFY(f.run({"worktree","unlock",outside})); QVERIFY(ok(waitFor(service,service.removeWorktree(outside))));
}
void GitTest::remoteRefreshAndSourceFetch() {
    Fixture f; const auto bare=f.base.filePath("origin.git"); QVERIFY(f.run({"clone","--bare",f.root,bare})); QVERIFY(f.run({"remote","add","origin",bare})); QVERIFY(f.run({"fetch","origin"}));
    QVERIFY(f.run({"symbolic-ref","refs/remotes/origin/HEAD","refs/remotes/origin/main"}));
    GitService service(f.options()); service.setWorkspaceContext(f.context());
    auto result=waitFor(service,service.branches(f.root)); QVERIFY(ok(result)); QVERIFY(!result.branches.refreshSucceeded); QCOMPARE(result.branches.defaultRemoteBranch,"origin/main");
    result=waitFor(service,service.branches(f.root,true)); QVERIFY(ok(result)); QVERIFY(result.branches.refreshSucceeded);
    result=waitFor(service,service.createWorktree(f.root,"remote-copy","origin/main")); QVERIFY(ok(result));
    QByteArray upstream; QVERIFY(f.run({"config","--get-regexp","branch.remote-copy"},{},&upstream)==false); // --no-track leaves no branch tracking config.
    QVERIFY(f.run({"remote","set-url","origin",f.base.filePath("missing.git")}));
    result=waitFor(service,service.createBranch(f.root,"must-not-exist","origin/main")); QVERIFY(!result.ok);
    QByteArray names; QVERIFY(f.run({"branch","--list","must-not-exist"},{},&names)); QVERIFY(names.trimmed().isEmpty());
}
void GitTest::remoteUrlsAndEnvironment() {
    Fixture f; QVERIFY(f.run({"remote","add","origin","https://user:fake-secret@example.invalid/project.git?token=fake#frag"}));
    GitService service(f.options()); service.setWorkspaceContext(f.context());
    auto result=waitFor(service,service.inspect(f.root)); QVERIFY(ok(result)); QCOMPARE(result.snapshot.remoteUrl,"https://example.invalid/project");
    QVERIFY(f.run({"remote","set-url","origin","git@example.invalid:team/project.git"}));
    result=waitFor(service,service.inspect(f.root)); QVERIFY(ok(result)); QCOMPARE(result.snapshot.remoteUrl,"https://example.invalid/team/project");
    qputenv("ADE_FIXTURE_API_KEY","dummy-never-exported"); qputenv("GIT_INDEX_FILE","dummy-index");
    const auto env=gitdetail::safeEnvironment(); qunsetenv("ADE_FIXTURE_API_KEY"); qunsetenv("GIT_INDEX_FILE");
    QVERIFY(!env.contains("ADE_FIXTURE_API_KEY")); QVERIFY(!env.contains("GIT_INDEX_FILE")); QCOMPARE(env.value("GIT_TERMINAL_PROMPT"),"0");
}
void GitTest::postMutationRefreshFailure() {
    Fixture f; QVERIFY(write(f.base.filePath("hooks/post-checkout"),"#!/bin/sh\nprintf 'broken index' > .git/index\nexit 0\n"));
    QFile::setPermissions(f.base.filePath("hooks/post-checkout"),QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
    GitService service(f.options()); service.setWorkspaceContext(f.context());
    const auto result=waitFor(service,service.createBranch(f.root,"actually-created","main"));
    QVERIFY(ok(result)); QVERIFY(result.mayHaveMutated); QCOMPARE(result.branch,"actually-created"); QVERIFY(!result.warnings.isEmpty());
    QByteArray branch; QVERIFY(f.run({"symbolic-ref","--short","HEAD"},{},&branch)); QCOMPARE(branch.trimmed(),"actually-created");
}
void GitTest::junctionEscape() {
#ifdef Q_OS_WIN
    Fixture f; QTemporaryDir outside; QVERIFY(write(outside.filePath("secret.txt"),"outside"));
    const auto link=f.root+"/junction"; QProcess process;
    process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside.path())}); QVERIFY(process.waitForFinished(5000)); QVERIFY(process.exitCode()==0);
    const bool rejected=gitdetail::safeChildPath(f.root,"junction/secret.txt").isEmpty();
    GitService service(f.options()); service.setWorkspaceContext(f.context()); const auto result=waitFor(service,service.fileDiff(f.root,"junction/secret.txt",false));
    QVERIFY(QDir().rmdir(link)); QVERIFY(rejected); QCOMPARE(result.error.code,GitErrorCode::InvalidPath); QCOMPARE(read(outside.filePath("secret.txt")),"outside");
#endif
}
void GitTest::queuedMutationsAndCancellation() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context()); QSignalSpy spy(&service,&GitService::completed);
    const auto first=service.createBranch(f.root,"one","main"), second=service.createBranch(f.root,"two","main"), cancel=service.branches(f.root);
    service.cancel(cancel); QTRY_COMPARE_WITH_TIMEOUT(spy.size(),3,20000);
    QMap<quint64,GitResult> results; for(const auto &row:spy) results[row[0].toULongLong()]=qvariant_cast<GitResult>(row[1]);
    QVERIFY(ok(results[first])); QVERIFY(ok(results[second])); QCOMPARE(results[cancel].error.code,GitErrorCode::Cancelled);
    QCOMPARE(results[first].snapshot.branch,"one"); QCOMPARE(results[second].snapshot.branch,"two");
}
void GitTest::processBoundsAndTeardown() {
    const auto executable=QCoreApplication::applicationFilePath(); const auto cancel=std::make_shared<std::atomic_bool>(false); QTemporaryDir temp;
    auto result=gitdetail::runProcess(executable,{"--process-helper","flood"},temp.path(),gitdetail::safeEnvironment(),cancel,5000,4096);
    QVERIFY(result.limited); QVERIFY(result.out.size()<=4096);
    result=gitdetail::runProcess(executable,{"--process-helper","sleep"},temp.path(),gitdetail::safeEnvironment(),cancel,100,4096);
    QVERIFY(result.timedOut);
    result=gitdetail::runProcess(temp.filePath("missing.exe"),{},temp.path(),gitdetail::safeEnvironment(),cancel,1000,4096); QVERIFY(!result.error.isEmpty());
    result=gitdetail::runProcess(executable,{"--process-helper","spawn"},temp.path(),gitdetail::safeEnvironment(),cancel,5000,4096); QVERIFY(result.ok());
#ifdef Q_OS_WIN
    const DWORD pid=result.out.trimmed().toULong(); QVERIFY(pid>0);
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(child) { QCOMPARE(WaitForSingleObject(child,3000),DWORD(WAIT_OBJECT_0)); CloseHandle(child); }
#endif
}
void GitTest::setupPersistenceAndExecution() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context());
    const QString script="[IO.File]::WriteAllText((Join-Path $env:STAGEWISE_TARGET_WORKTREE_PATH 'prepared.txt'),$env:STAGEWISE_SOURCE_WORKTREE_PATH); Write-Output 'setup done'; exit 0";
    auto saved=waitFor(service,service.saveSetupScript(f.root,GitSetupVariant::PowerShell,script,{})); QVERIFY(ok(saved)); QCOMPARE(read(saved.path),script.toUtf8());
    auto created=waitFor(service,service.createWorktree(f.root,"setup","main")); QVERIFY(ok(created)); QVERIFY(!QFileInfo::exists(created.path+"/prepared.txt"));
    auto repositories=waitFor(service,service.setupRepositories()); QVERIFY(ok(repositories)); QCOMPARE(repositories.setupRepositories.size(),1); QCOMPARE(repositories.setupRepositories[0].powershell.content,script);
    WorktreeSetupRunner runner; QSignalSpy spy(&runner,&WorktreeSetupRunner::updated);
    const auto id=runner.startAfterMount(created.setup); QCOMPARE(runner.startAfterMount(created.setup),id);
    const auto run=waitSetup(spy,id); QCOMPARE(run.status,WorktreeSetupRun::Status::Succeeded); QVERIFY(run.stdoutTail.contains("setup done")); QCOMPARE(read(created.path+"/prepared.txt"),created.setup.sourceWorktreePath.toUtf8());
    // The target script wins over the main worktree fallback.
    QVERIFY(write(created.path+"/.stagewise/worktree-setup.ps1","Write-Output 'target wins'; exit 17"));
    const auto failed=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(failed.status,WorktreeSetupRun::Status::Failed); QCOMPARE(failed.exitCode,17); QVERIFY(failed.stdoutTail.contains("target wins")); QVERIFY(QFileInfo::exists(created.path));
}
void GitTest::setupCodexFallbackAndErrors() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context()); auto created=waitFor(service,service.createWorktree(f.root,"codex","main")); QVERIFY(ok(created));
    const auto config=f.root+"/.codex/environments/environment.toml";
    QVERIFY(write(config,"[setup]\nscript = \"exit 31\"\n[setup.win32]\nscript = '''Write-Output $env:CODEX_WORKTREE_PATH\nexit 0'''\n"));
    WorktreeSetupRunner runner; QSignalSpy spy(&runner,&WorktreeSetupRunner::updated);
    auto run=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(run.status,WorktreeSetupRun::Status::Succeeded); QVERIFY(run.stdoutTail.contains(created.path));
    QVERIFY(write(config,"[setup\ninvalid")); run=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(run.status,WorktreeSetupRun::Status::Failed); QVERIFY(run.message.contains("TOML"));
    QVERIFY(QFile::remove(config)); run=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(run.status,WorktreeSetupRun::Status::Skipped);
    QVERIFY(write(created.path+"/.stagewise/worktree-setup.ps1","exit 0")); runner.setShellExecutable(f.base.filePath("missing-shell.exe"));
    run=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(run.status,WorktreeSetupRun::Status::Failed); QVERIFY(run.message.contains("Failed to run"));
}
void GitTest::setupCancellationAndOutputTails() {
    Fixture f; GitService service(f.options()); service.setWorkspaceContext(f.context()); auto created=waitFor(service,service.createWorktree(f.root,"cancel","main")); QVERIFY(ok(created));
    const auto script=created.path+"/.stagewise/worktree-setup.ps1"; QVERIFY(write(script,"Write-Output ('x' * 20000); Write-Output 'TAIL'; Start-Sleep -Seconds 30"));
    WorktreeSetupRunner runner; runner.setTimeoutMs(1500); QSignalSpy spy(&runner,&WorktreeSetupRunner::updated);
    auto run=waitSetup(spy,runner.startAfterMount(created.setup)); QCOMPARE(run.status,WorktreeSetupRun::Status::Failed); QVERIFY(run.message.contains("timed out")); QVERIFY(run.stdoutTail.contains("TAIL")); QVERIFY(run.stdoutTail.size()<=12000);
    runner.setTimeoutMs(10000); const auto id=runner.startAfterMount(created.setup); runner.cancel(created.path); run=waitSetup(spy,id); QCOMPARE(run.status,WorktreeSetupRun::Status::Failed); QVERIFY(run.message.contains("interrupted"));
    QVERIFY(QFileInfo::exists(created.path));
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); const auto args=app.arguments();
    if(args.size()>=3 && args[1]=="--process-helper") {
        if(args[2]=="sleep") { QThread::sleep(30); return 0; }
        if(args[2]=="flood") { QFile out; if(!out.open(stdout,QIODevice::WriteOnly)) return 3; out.write(QByteArray(65536,'x')); out.flush(); QThread::sleep(30); return 0; }
        if(args[2]=="spawn") { QProcess child; child.start(app.applicationFilePath(),{"--process-helper","sleep"}); if(!child.waitForStarted(3000)) return 2; QFile out; if(!out.open(stdout,QIODevice::WriteOnly)) return 3; out.write(QByteArray::number(child.processId())+'\n'); out.flush(); QThread::msleep(150); std::_Exit(0); }
    }
    GitTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_git.moc"
