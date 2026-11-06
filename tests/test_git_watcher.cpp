#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QProcess>
#include <QSaveFile>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "git/GitRepositoryWatcher.h"
#include "git/GitService.h"
#include "git/ProcessRunner.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace ade;
namespace {
bool write(const QString &path,const QByteArray &bytes) {
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))return false;
    QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.flush();
}
bool atomicWrite(const QString &path,const QByteArray &bytes,quint32* nativeFailure=nullptr) {
    if(nativeFailure)*nativeFailure=0;
#ifdef Q_OS_WIN
    // QSaveFile uses MoveFileExW, which can fail with ERROR_ACCESS_DENIED
    // while a reader is open even when it grants FILE_SHARE_DELETE. Use one
    // same-directory replacement that actually supports those readers. No
    // retry is used: an incompatible reader must still fail this fixture.
    QString replacementPath;
    {
        QTemporaryFile file(path+".atomic-XXXXXX");
        if(!file.open()||file.write(bytes)!=bytes.size()||!file.flush()) {
            qWarning().noquote()<<"Atomic fixture preparation failed:"<<path<<file.errorString();
            return false;
        }
        replacementPath=file.fileName();
        file.setAutoRemove(false);
        // QTemporaryFile may retain its native handle after close() so it can
        // reopen safely. Destroy it before the native replacement operation.
    }
    const auto replacement=QDir::toNativeSeparators(replacementPath).toStdWString();
    const auto destination=QDir::toNativeSeparators(path).toStdWString();
    if(!ReplaceFileW(destination.c_str(),replacement.c_str(),nullptr,0,nullptr,nullptr)) {
        const DWORD nativeError=GetLastError();
        if(nativeFailure)*nativeFailure=nativeError;
        QFile::remove(replacementPath);
        qWarning().noquote()<<"Atomic fixture ReplaceFileW failed:"<<path<<"native error"<<nativeError;
        return false;
    }
    return true;
#else
    QSaveFile file(path);
    const auto failed=[&](const char* stage){
        qWarning().noquote()<<"Atomic fixture write failed:"<<stage<<path<<"Qt error"<<file.error()<<file.errorString();
        return false;
    };
    if(!file.open(QIODevice::WriteOnly))return failed("open");
    if(file.write(bytes)!=bytes.size())return failed("write");
    if(!file.commit())return failed("commit");
    return true;
#endif
}
struct Fixture {
    QTemporaryDir base;QString root,common,git,error;
    Fixture(){
        root=base.filePath("repository with spaces");git=QStandardPaths::findExecutable("git");QDir().mkpath(root);
        if(!run({"init","-b","main"}))return;
        run({"config","user.name","ADE watcher fixture"});run({"config","user.email","fixture@example.invalid"});
        run({"config","commit.gpgsign","false"});run({"config","core.autocrlf","false"});run({"config","core.hooksPath",base.filePath("hooks")});
        write(root+"/file.txt","base\n");run({"add","--","."});run({"commit","-m","base"});common=gitdetail::canonical(root+"/.git");
    }
    bool run(const QStringList &args,const QString &cwd={}) {
        const QString dir=cwd.isEmpty()?root:cwd;
        if(!gitdetail::inside(base.path(),dir,true)){error="Fixture command escaped temporary root.";return false;}
        QProcess process;auto env=gitdetail::safeEnvironment();env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global-config"));
        process.setProcessEnvironment(env);process.setWorkingDirectory(dir);process.start(git,args);
        if(!process.waitForFinished(15000)||process.exitCode()!=0){error=QString::fromUtf8(process.readAllStandardError());return false;}return true;
    }
    GitSnapshot snapshot(const QString &workspace={})const{GitSnapshot value;value.repoRoot=workspace.isEmpty()?root:workspace;value.commonGitDir=common;return value;}
};
GitRepositoryWatchOptions fast(){GitRepositoryWatchOptions options;options.debounceMs=160;options.scanDelayMs=15;options.retryMs=150;options.availabilityCheckMs=100;return options;}
bool containsPath(const QStringList &paths,const QString &path){for(const auto &item:paths)if(gitdetail::key(item)==gitdetail::key(path))return true;return false;}
}
class GitWatcherTests:public QObject {
    Q_OBJECT
private slots:
    void sharedMainAndLinkedOwners(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));const QString linked=f.base.filePath("linked");QVERIFY(f.run({"worktree","add","-b","linked",linked}));
        GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged),coverage(&watcher,&GitRepositoryWatcher::coverageChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QVERIFY(watcher.acquire("linked",f.snapshot(linked)));QCOMPARE(watcher.repositoryCount(),1);QCOMPARE(watcher.ownerCount(f.common),2);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(coverage.last()[1].toBool());QVERIFY(containsPath(watcher.watchedPaths(f.common),f.common+"/worktrees/linked/HEAD"));QCOMPARE(changed.size(),0);
        watcher.release("main");QCOMPARE(watcher.ownerCount(f.common),1);QVERIFY(f.run({"checkout","-b","linked-next"},linked));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);
        watcher.release("linked");QCOMPARE(watcher.repositoryCount(),0);QVERIFY(watcher.watchedPaths(f.common).isEmpty());QVERIFY(f.run({"checkout","-b","after-release"},linked));QTest::qWait(350);QCOMPARE(changed.size(),1);
    }
    void externalAddAndRemove(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        const QString linked=f.base.filePath("added");QVERIFY(f.run({"worktree","add","-b","added",linked}));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);
        QVERIFY(containsPath(watcher.watchedPaths(f.common),f.common+"/worktrees/added/HEAD"));QVERIFY(f.run({"worktree","remove",linked}));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),2,5000);
        QVERIFY(!containsPath(watcher.watchedPaths(f.common),f.common+"/worktrees/added/HEAD"));QCOMPARE(changed.last()[1].toULongLong(),quint64(2));
    }
    void atomicHeadReplacementReattaches(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        QVERIFY(atomicWrite(f.common+"/HEAD","ref: refs/heads/replaced\n"));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);
        QVERIFY(containsPath(watcher.watchedPaths(f.common),f.common+"/HEAD"));QVERIFY(write(f.common+"/HEAD","ref: refs/heads/edited-again\n"));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),2,5000);
    }
    void unrelatedMetadataDoesNotPublish(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QTest::qWait(100);
        QVERIFY(write(f.common+"/index.lock","noise"));QVERIFY(write(f.common+"/objects/aa/fixture","noise"));QVERIFY(write(f.common+"/refs/heads/noise","noise"));QVERIFY(write(f.common+"/ORIG_HEAD","noise"));
        watcher.refresh();QTest::qWait(500);QCOMPARE(changed.size(),0);
        const auto paths=watcher.watchedPaths(f.common);for(const auto &path:paths){QVERIFY(!path.contains("objects"));QVERIFY(!path.contains("index"));QVERIFY(!path.contains("refs"));}
    }
    void changedFingerprintDebouncesBurst(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        for(int i=0;i<3;++i){QVERIFY(atomicWrite(f.common+"/HEAD","ref: refs/heads/burst"+QByteArray::number(i)+"\n"));QTest::qWait(50);}
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);QTest::qWait(300);QCOMPARE(changed.size(),1);
    }
    void atomicReplacementSharesDeleteWithReaders(){
#ifdef Q_OS_WIN
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));const auto path=f.common+"/HEAD";const auto native=QDir::toNativeSeparators(path).toStdWString();
        const auto close=[](void* handle){if(handle&&handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);};
        std::unique_ptr<void,decltype(close)> reader(CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr),close);QVERIFY(reader.get()!=INVALID_HANDLE_VALUE);
        // This is the Windows sharing contract used by the production scanner:
        // an opened immutable read must coexist with a first-attempt replacement.
        QVERIFY(atomicWrite(path,"ref: refs/heads/shared-reader\n"));char bytes[128];DWORD count=0;QVERIFY(ReadFile(reader.get(),bytes,sizeof(bytes),&count,nullptr));QCOMPARE(QByteArray(bytes,int(count)),QByteArray("ref: refs/heads/main\n"));reader.reset();
        reader.reset(CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));QVERIFY(reader.get()!=INVALID_HANDLE_VALUE);
        quint32 failure=0;QVERIFY(!atomicWrite(path,"must not replace\n",&failure));QCOMPARE(failure,quint32(ERROR_SHARING_VIOLATION));
        reader.reset();QFile unchanged(path);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(unchanged.readAll(),QByteArray("ref: refs/heads/shared-reader\n"));unchanged.close();QVERIFY(atomicWrite(path,"ref: refs/heads/recovered\n"));
#else
        QSKIP("Windows atomic replacement sharing contract.");
#endif
    }
    void scansDoNotBlockAtomicMetadataChurn(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));auto options=fast();options.scanDelayMs=1;options.maxHeadBytes=128*1024;options.maxTotalHeadBytes=128*1024;GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QByteArray payload(96*1024,'x');
        for(int i=0;i<200;++i){const auto bytes="ref: refs/heads/churn-"+QByteArray::number(i)+"\n"+payload;QVERIFY2(atomicWrite(f.common+"/HEAD",bytes),qPrintable(QString("First-attempt atomic replacement %1 failed while metadata scans were active.").arg(i)));watcher.refresh();QTest::qWait(2);}
        QVERIFY(atomicWrite(f.common+"/HEAD","ref: refs/heads/final\n"));QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(),5000);QFile final(f.common+"/HEAD");QVERIFY(final.open(QIODevice::ReadOnly));QCOMPARE(final.readAll(),QByteArray("ref: refs/heads/final\n"));
    }
    void realGitUpdatesWhileMetadataScansRun(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));auto options=fast();options.scanDelayMs=1;GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        for(int i=0;i<20;++i){watcher.refresh();QTest::qWait(2);QVERIFY2(f.run({"symbolic-ref","HEAD","refs/heads/git-churn-"+QString::number(i)}),qPrintable(f.error));}
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(),5000);QFile final(f.common+"/HEAD");QVERIFY(final.open(QIODevice::ReadOnly));QCOMPARE(final.readAll(),QByteArray("ref: refs/heads/git-churn-19\n"));
    }
    void indexChurnDoesNotStarveHead(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("main",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(atomicWrite(f.common+"/HEAD","ref: refs/heads/changed\n"));
        QElapsedTimer timer;timer.start();while(timer.elapsed()<1200&&changed.isEmpty()){QVERIFY(write(f.common+"/index.lock",QByteArray::number(timer.elapsed())));watcher.refresh();QTest::qWait(10);}
        QCOMPARE(changed.size(),1);
    }
    void rejectedRetargetPreservesSubscription(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));auto options=fast();options.maxOwners=1;GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready);
        QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);auto invalid=f.snapshot();invalid.commonGitDir=f.base.filePath("missing");QString error;
        QVERIFY(!watcher.acquire("owner",invalid,&error));QVERIFY(!error.isEmpty());QCOMPARE(watcher.ownerCount(f.common),1);QVERIFY(!watcher.acquire("extra",f.snapshot(),&error));QCOMPARE(watcher.ownerCount(f.common),1);
    }
    void repositoryLimitAllowsSoleOwnerRetarget(){
        Fixture a,b;QVERIFY(a.error.isEmpty());QVERIFY(b.error.isEmpty());auto options=fast();options.maxRepositories=1;GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready);
        QVERIFY(watcher.acquire("owner",a.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(watcher.acquire("owner",b.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),2,5000);
        QCOMPARE(watcher.repositoryCount(),1);QCOMPARE(watcher.ownerCount(a.common),0);QCOMPARE(watcher.ownerCount(b.common),1);
    }
    void canonicalAliasSharesExistingRepository(){
#ifdef Q_OS_WIN
        Fixture f;QVERIFY(f.error.isEmpty());const QString alias=f.base.filePath("metadata-alias");QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(alias),QDir::toNativeSeparators(f.common)});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
        GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready);auto other=f.snapshot();other.commonGitDir=alias;
        QVERIFY(watcher.acquire("original",f.snapshot()));QVERIFY(watcher.acquire("alias",other));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QCOMPARE(watcher.repositoryCount(),1);QCOMPARE(watcher.ownerCount(alias),2);
        watcher.release("original");watcher.release("alias");QVERIFY(RemoveDirectoryW(QDir::toNativeSeparators(alias).toStdWString().c_str()));
#else
        QSKIP("Windows junction fixture.");
#endif
    }
    void releaseAndReacquireRejectsStaleScan(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        for(int i=0;i<12;++i){QVERIFY(watcher.acquire("owner",f.snapshot()));watcher.refresh();QCoreApplication::processEvents();watcher.release("owner");}
        ready.clear();QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QTest::qWait(300);QCOMPARE(ready.size(),1);QCOMPARE(changed.size(),0);QCOMPARE(watcher.repositoryCount(),1);
    }
    void boundsReportIncompleteAndRecover(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));auto options=fast();options.maxWorktrees=2;options.maxHeadBytes=64;GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),coverage(&watcher,&GitRepositoryWatcher::coverageChanged);
        QVERIFY(write(f.common+"/HEAD",QByteArray(65,'a')));QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(!coverage.last()[1].toBool());QVERIFY(coverage.last()[2].toString().contains("byte limit"));
        QVERIFY(write(f.common+"/HEAD","ref: refs/heads/main\n"));QTRY_VERIFY_WITH_TIMEOUT(coverage.last()[1].toBool(),5000);
        for(int i=0;i<3;++i)QVERIFY(write(f.common+"/worktrees/test"+QString::number(i)+"/HEAD","ref: refs/heads/main\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!coverage.last()[1].toBool(),5000);QVERIFY(coverage.last()[2].toString().contains("entry limit"));QVERIFY(watcher.watchedPaths(f.common).size()<=7);
        QVERIFY(QDir(f.common+"/worktrees/test2").removeRecursively());QTRY_VERIFY_WITH_TIMEOUT(coverage.last()[1].toBool(),5000);
    }
    void combinedBudgetIncludesOversizedHeads(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));auto options=fast();options.maxHeadBytes=16;options.maxTotalHeadBytes=32;
        for(int i=0;i<3;++i)QVERIFY(write(f.common+"/worktrees/test"+QString::number(i)+"/HEAD",QByteArray(32,'x')));
        GitRepositoryWatcher watcher(options);QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),coverage(&watcher,&GitRepositoryWatcher::coverageChanged);
        QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(!coverage.last()[1].toBool());
        for(const auto &path:watcher.watchedPaths(f.common))QVERIFY(!path.endsWith("/HEAD"));
    }
    void repositoryDisappearsAndReturns(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),coverage(&watcher,&GitRepositoryWatcher::coverageChanged),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        // Settle the post-attachment rescan. Otherwise that unrelated scan can
        // mask Windows' missing notification when a watched root is renamed.
        QTest::qWait(500);QVERIFY(QDir().rename(f.common,f.common+".away"));
        QTRY_VERIFY_WITH_TIMEOUT(!coverage.last()[1].toBool(),5000);QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);QVERIFY(QDir().rename(f.common+".away",f.common));
        QTRY_VERIFY_WITH_TIMEOUT(coverage.last()[1].toBool(),5000);QTRY_COMPARE_WITH_TIMEOUT(changed.size(),2,5000);
    }
    void observersCanDeleteOrRelease(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));QPointer<GitRepositoryWatcher> watcher=new GitRepositoryWatcher(fast());
        connect(watcher,&GitRepositoryWatcher::ready,this,[&]{delete watcher.data();});QVERIFY(watcher->acquire("owner",f.snapshot()));QTRY_VERIFY_WITH_TIMEOUT(watcher.isNull(),5000);
        GitRepositoryWatcher second(fast());QSignalSpy ready(&second,&GitRepositoryWatcher::ready);connect(&second,&GitRepositoryWatcher::topologyChanged,this,[&]{second.release("owner");});
        QVERIFY(second.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QVERIFY(write(f.common+"/HEAD","ref: refs/heads/next\n"));QTRY_COMPARE_WITH_TIMEOUT(second.repositoryCount(),0,5000);
    }
    void replacedRootReattachesEvenWhenHeadTextIsIdentical(){
        Fixture f;QVERIFY(f.error.isEmpty());GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QTest::qWait(500);
        QVERIFY(QDir().rename(f.common,f.common+".old"));QVERIFY(write(f.common+"/HEAD","ref: refs/heads/main\n"));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(),5000);QVERIFY(containsPath(watcher.watchedPaths(f.common),f.common+"/HEAD"));const auto before=changed.size();
        QVERIFY(atomicWrite(f.common+"/HEAD","ref: refs/heads/new-root\n"));QTRY_VERIFY_WITH_TIMEOUT(changed.size()>before,5000);
    }
    void availabilityProbeDoesNotPollHeadContents(){
        Fixture f;QVERIFY(f.error.isEmpty());GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),changed(&watcher,&GitRepositoryWatcher::topologyChanged);
        QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QTest::qWait(500);
        auto *native=watcher.findChild<QFileSystemWatcher*>();QVERIFY(native);native->blockSignals(true);
        QVERIFY(write(f.common+"/HEAD","ref: refs/heads/no-native-event\n"));QTest::qWait(500);QCOMPARE(changed.size(),0);
    }
    void availabilityCoversMoreRepositoriesThanWorkerSlots(){
        QTemporaryDir base;GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready);QSet<QString> missing;
        connect(&watcher,&GitRepositoryWatcher::coverageChanged,this,[&](const QString &path,bool complete,const QString&){if(!complete)missing.insert(path);});
        QStringList paths;
        for(int i=0;i<6;++i){const QString root=base.filePath("repository-"+QString::number(i)),common=root+"/.git";QVERIFY(write(common+"/HEAD","ref: refs/heads/main\n"));GitSnapshot snapshot;snapshot.repoRoot=root;snapshot.commonGitDir=common;QVERIFY(watcher.acquire(QString::number(i),snapshot));paths.append(gitdetail::canonical(common));}
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(),6,5000);QTest::qWait(500);
        for(auto *native:watcher.findChildren<QFileSystemWatcher*>())native->blockSignals(true);
        for(const auto &path:paths)QVERIFY(QDir().rename(path,path+".away"));
        QTRY_COMPARE_WITH_TIMEOUT(missing.size(),6,5000);
    }
    void junctionMetadataIsNotFollowed(){
#ifdef Q_OS_WIN
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));const QString outside=f.base.filePath("outside"),link=f.common+"/worktrees/redirect";QVERIFY(write(outside+"/HEAD","secret fixture bytes"));QVERIFY(QDir().mkpath(f.common+"/worktrees"));
        QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside)});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
        GitRepositoryWatcher watcher(fast());QSignalSpy ready(&watcher,&GitRepositoryWatcher::ready),coverage(&watcher,&GitRepositoryWatcher::coverageChanged);QVERIFY(watcher.acquire("owner",f.snapshot()));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);
        QVERIFY(!coverage.last()[1].toBool());QVERIFY(!containsPath(watcher.watchedPaths(f.common),link));QVERIFY(!containsPath(watcher.watchedPaths(f.common),link+"/HEAD"));
        watcher.release("owner");QVERIFY(RemoveDirectoryW(QDir::toNativeSeparators(link).toStdWString().c_str()));
#else
        QSKIP("Windows junction fixture.");
#endif
    }
    void serviceSharesAuthorizedOwners(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));const auto linked=f.base.filePath("linked");QVERIFY(f.run({"worktree","add","-b","linked",linked}));
        GitServiceOptions options;options.gitExecutable=f.git;options.managedWorktreeRoot=f.base.filePath("managed");GitService service(options);
        GitWorkspaceContext context;context.trustedRoots={f.root,linked};service.setWorkspaceContext(context);
        auto *watcher=service.findChild<GitRepositoryWatcher*>();QVERIFY(watcher);QSignalSpy ready(watcher,&GitRepositoryWatcher::ready),changed(&service,&GitService::repositoryTopologyChanged);
        QVERIFY(service.watchWorkspace("main-chat",f.root));QVERIFY(service.watchWorkspace("linked-chat",linked));
        QTRY_COMPARE_WITH_TIMEOUT(watcher->ownerCount(f.common),2,15000);QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,5000);QCOMPARE(watcher->repositoryCount(),1);
        service.unwatchWorkspace("main-chat");QCOMPARE(watcher->ownerCount(f.common),1);QVERIFY(f.run({"checkout","-b","linked-next"},linked));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,5000);
        service.unwatchWorkspace("linked-chat");QCOMPARE(watcher->repositoryCount(),0);
    }
    void serviceRetargetRejectsStaleInspection(){
        Fixture a,b;QVERIFY2(a.error.isEmpty(),qPrintable(a.error));QVERIFY2(b.error.isEmpty(),qPrintable(b.error));
        GitServiceOptions options;options.gitExecutable=a.git;options.managedWorktreeRoot=a.base.filePath("managed");GitService service(options);GitWorkspaceContext context;context.trustedRoots={a.root,b.root};service.setWorkspaceContext(context);
        auto *watcher=service.findChild<GitRepositoryWatcher*>();QVERIFY(watcher);QSignalSpy done(&service,&GitService::completed);
        QVERIFY(service.watchWorkspace("chat",a.root));QVERIFY(service.watchWorkspace("chat",b.root));
        QTRY_COMPARE_WITH_TIMEOUT(watcher->ownerCount(b.common),1,15000);QTRY_VERIFY_WITH_TIMEOUT(done.size()>=2,15000);QCOMPARE(watcher->ownerCount(a.common),0);QCOMPARE(watcher->repositoryCount(),1);
    }
    void serviceRevocationDropsPendingAndActiveOwners(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitServiceOptions options;options.gitExecutable=f.git;options.managedWorktreeRoot=f.base.filePath("managed");GitService service(options);
        GitWorkspaceContext context;context.trustedRoots={f.root};service.setWorkspaceContext(context);auto *watcher=service.findChild<GitRepositoryWatcher*>();QVERIFY(watcher);QSignalSpy done(&service,&GitService::completed);
        QVERIFY(service.watchWorkspace("chat",f.root));service.setWorkspaceContext({});QTRY_VERIFY_WITH_TIMEOUT(!done.isEmpty(),15000);QCOMPARE(watcher->repositoryCount(),0);QVERIFY(!service.watchWorkspace("denied",f.root));
        service.setWorkspaceContext(context);QVERIFY(service.watchWorkspace("chat",f.root));QTRY_COMPARE_WITH_TIMEOUT(watcher->ownerCount(f.common),1,15000);
        service.setWorkspaceContext({});QCOMPARE(watcher->repositoryCount(),0);
    }
    void observerCanDeleteServiceOnWatchError(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));QPointer<GitService> service=new GitService;
        connect(service,&GitService::repositoryWatchError,this,[&]{delete service.data();});QVERIFY(!service->watchWorkspace("chat",f.root));QVERIFY(service.isNull());
    }
};
QTEST_GUILESS_MAIN(GitWatcherTests)
#include "test_git_watcher.moc"
