#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include "git/WorktreeUsage.h"
#include "git/WorktreeUsageJournal.h"
#include "git/WorktreeUsageIdentity.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace ade;
namespace {
WorktreeUsageIdentity identity(const QString &instance="fixture-1",const QString &path="C:/fixture/worktree"){return {"C:/fixture/main/.git",path,instance,true};}
WorktreeCleanupFacts facts(){WorktreeCleanupFacts f;f.identity=identity();f.authorized=f.managed=f.registered=f.statusKnown=f.merged=true;f.branch="feature";f.mergedInto=f.defaultBranch="main";return f;}
WorktreeUsageRecord used(){WorktreeUsageRecord r;r.identity=identity();r.evidence=WorktreeUsageEvidence::VerifiedLiveUse;r.firstObservedAt=1000;r.lastUsedAt=2000;return r;}
QByteArray read(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
bool write(const QString &path,const QByteArray &bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.flush();}
struct GitFixture{
    QTemporaryDir base;QString root,linked,common,admin,git,error;
    GitFixture(){root=base.filePath("repository");linked=base.filePath("linked");common=root+"/.git";admin=common+"/worktrees/linked";git=QStandardPaths::findExecutable("git");QDir().mkpath(root);run({"init","-b","main"});run({"config","user.name","ADE usage fixture"});run({"config","user.email","fixture@example.invalid"});run({"config","commit.gpgsign","false"});run({"config","core.hooksPath",base.filePath("hooks")});write(root+"/file.txt","base\n");run({"add","."});run({"commit","-m","base"});run({"worktree","add","-b","feature",linked});}
    bool run(const QStringList &args,const QString &directory={}){const auto cwd=directory.isEmpty()?root:directory;if(!QDir::cleanPath(cwd).startsWith(base.path()+'/',Qt::CaseInsensitive)){error="Fixture command escaped temporary root.";return false;}auto env=QProcessEnvironment::systemEnvironment();for(const auto &name:env.keys())if(name.startsWith("GIT_")||name=="GCM_INTERACTIVE")env.remove(name);env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global-config"));env.insert("GIT_TERMINAL_PROMPT","0");QProcess process;process.setProcessEnvironment(env);process.setWorkingDirectory(cwd);process.start(git,args);if(!process.waitForFinished(15000)||process.exitCode()!=0){error=QString::fromUtf8(process.readAllStandardError());return false;}return true;}
    GitSnapshot snapshot()const{GitSnapshot s;s.repoRoot=linked;s.commonGitDir=common;return s;}
};
}
class UsageTests:public QObject{
    Q_OBJECT
private slots:
    void exactAgeAndSnoozeBoundary(){auto r=used();auto f=facts();WorktreeUsageHealth h{true,true,-1};QCOMPARE(evaluateWorktreeCleanup(f,&r,h,2000+WorktreeUnusedMs-1).reason,"recent-use");QVERIFY(evaluateWorktreeCleanup(f,&r,h,2000+WorktreeUnusedMs).eligible);r.dismissedAt=2000+WorktreeUnusedMs;QCOMPARE(evaluateWorktreeCleanup(f,&r,h,r.dismissedAt+WorktreeUnusedMs-1).reason,"snoozed");QVERIFY(evaluateWorktreeCleanup(f,&r,h,r.dismissedAt+WorktreeUnusedMs).eligible);}
    void unknownNeverBecomesNeverUsed(){auto r=used();r.evidence=WorktreeUsageEvidence::UnknownImportedHistory;QCOMPARE(evaluateWorktreeCleanup(facts(),&r,{true,true,-1},WorktreeUnusedMs*100).reason,"usage-history-unknown");}
    void newInstanceDoesNotInheritOldPathHistory(){auto r=used();auto f=facts();f.identity.instanceToken="new-instance";QCOMPARE(evaluateWorktreeCleanup(f,&r,{true,true,-1},WorktreeUnusedMs*100).reason,"usage-history-unknown");}
    void liveGuardsOverridePreviouslyEligible(){auto r=used();auto f=facts();const auto now=WorktreeUnusedMs*2;QVERIFY(evaluateWorktreeCleanup(f,&r,{true,true,-1},now).eligible);f.mounted=true;QCOMPARE(evaluateWorktreeCleanup(f,&r,{true,true,-1},now).reason,"mounted");f.mounted=false;f.busy=true;QCOMPARE(evaluateWorktreeCleanup(f,&r,{true,true,-1},now).reason,"workspace-busy");f.busy=false;f.dirty=true;QCOMPARE(evaluateWorktreeCleanup(f,&r,{true,true,-1},now).reason,"dirty-or-unknown-status");}
    void unknownStorageScopeAndFutureTimeBlock(){auto r=used();const auto now=WorktreeUnusedMs*2;QCOMPARE(evaluateWorktreeCleanup(facts(),&r,{false,true,-1},now).reason,"usage-storage-unavailable");QCOMPARE(evaluateWorktreeCleanup(facts(),&r,{true,false,-1},now).reason,"usage-tracking-incomplete");r.lastUsedAt=now+1;QCOMPARE(evaluateWorktreeCleanup(facts(),&r,{true,true,-1},now).reason,"future-clock-value");r=used();QCOMPARE(evaluateWorktreeCleanup(facts(),&r,{true,true,now},now).reason,"recovery-protection");}
    void mountRegistrySupportsMultipleOwnersAndMounts(){WorktreeMountRegistry mounts;const auto a=identity(),b=identity("fixture-2","C:/fixture/second");QVERIFY(mounts.replace("chat-one",{a,b}));QVERIFY(mounts.replace("chat-two",{a}));QCOMPARE(mounts.ownerCount(a),2);QCOMPARE(mounts.ownerCount(b),1);mounts.release("chat-one");QCOMPARE(mounts.ownerCount(a),1);QVERIFY(!mounts.isMounted(b));mounts.release("chat-two");QVERIFY(!mounts.isMounted(a));}
    void archivedHistoryIsNotAnImplicitMount(){WorktreeMountRegistry mounts;const auto archivedRecord=used();QVERIFY(!mounts.isMounted(archivedRecord.identity));QVERIFY(mounts.replace("host-retained-archived-mount",{archivedRecord.identity}));QVERIFY(mounts.isMounted(archivedRecord.identity));mounts.release("host-retained-archived-mount");QVERIFY(!mounts.isMounted(archivedRecord.identity));}
    void failedMountReplacementPreservesCurrent(){WorktreeMountRegistry mounts;const auto a=identity();QVERIFY(mounts.replace("chat",{a}));auto invalid=identity("second","C:/fixture/second");invalid.verified=false;QVERIFY(!mounts.replace("chat",{invalid}));QVERIFY(mounts.isMounted(a));QVERIFY(!mounts.replace("chat",{a,a}));QCOMPARE(mounts.ownerCount(a),1);}
    void staleMountedPathProtectsRecreatedInstance(){WorktreeMountRegistry mounts;QVERIFY(mounts.replace("old-owner",{identity()}));QVERIFY(mounts.isMounted(identity("new-instance")));}
    void asynchronousMountResultsCannotRestoreAnOldWorkspace(){WorktreeMountRegistry mounts;const auto a=identity(),b=identity("b","C:/fixture/b"),c=identity("c","C:/fixture/c");QVERIFY(mounts.replace("chat",{a}));const auto first=mounts.beginUpdate("chat",{b.path});QVERIFY(first);QVERIFY(mounts.isMounted(a));QVERIFY(mounts.isMounted(b));const auto second=mounts.beginUpdate("chat",{c.path});QVERIFY(second>first);QVERIFY(!mounts.completeUpdate("chat",first,{b}));QVERIFY(mounts.isMounted(a));QVERIFY(mounts.isMounted(c));QCOMPARE(mounts.pendingOwnerCount(),1);QVERIFY(mounts.completeUpdate("chat",second,{c}));QVERIFY(!mounts.isMounted(a));QVERIFY(mounts.isMounted(c));QCOMPARE(mounts.pendingOwnerCount(),0);QVERIFY(!mounts.completeUpdate("chat",second,{c}));}
    void releasedAndRecreatedOwnerRejectsOldCompletion(){WorktreeMountRegistry mounts;const auto a=identity(),b=identity("b","C:/fixture/b");const auto stale=mounts.beginUpdate("chat",{a.path});mounts.release("chat");const auto fresh=mounts.beginUpdate("chat",{b.path});QVERIFY(fresh>stale);QVERIFY(!mounts.completeUpdate("chat",stale,{a}));QVERIFY(!mounts.completeUpdate("chat",fresh,{a}));QVERIFY(mounts.isMounted(b));QVERIFY(mounts.completeUpdate("chat",fresh,{b}));QVERIFY(!mounts.isMounted(a));}
    void journalRetainsReconnectedWorkspacesAndMaximumUse(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(1000));const auto a=identity(),b=identity("second","C:/fixture/second");QVERIFY(journal.recordLiveUse(a,2000));QVERIFY(journal.recordLiveUse(b,3000));QVERIFY(journal.recordLiveUse(a,1500));QCOMPARE(journal.record(a).lastUsedAt,2000);QCOMPARE(journal.record(b).lastUsedAt,3000);QCOMPARE(journal.records().size(),2);QVERIFY(!journal.health().trackingScopeComplete);}
    void journalImportsRemainUnknownAndPathReuseIsIsolated(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(1000));const auto old=identity(),fresh=identity("replacement");QVERIFY(journal.recordLiveUse(old,2000));QVERIFY(journal.observeImported(fresh,3000,500));QCOMPARE(journal.record(old).lastUsedAt,2000);QCOMPARE(journal.record(fresh).lastUsedAt,500);QCOMPARE(journal.record(fresh).evidence,WorktreeUsageEvidence::UnknownImportedHistory);auto f=facts();f.identity=fresh;auto r=journal.record(fresh);QVERIFY(!evaluateWorktreeCleanup(f,&r,journal.validatedHealth(true),WorktreeUnusedMs*100).eligible);}
    void verifiedNewInstanceUsesCreationProtection(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(0));QVERIFY(journal.recordNewInstance(identity(),2000));const auto record=journal.record(identity());QCOMPARE(record.createdAt,2000);QCOMPARE(record.lastUsedAt,-1);QCOMPARE(evaluateWorktreeCleanup(facts(),&record,journal.validatedHealth(true),2000+WorktreeUnusedMs-1).reason,"recent-use");QVERIFY(evaluateWorktreeCleanup(facts(),&record,journal.validatedHealth(true),2000+WorktreeUnusedMs).eligible);}
    void cleanRestartPreservesUsageAndCrashStartsRecoveryFloor(){QTemporaryDir dir;const QByteArray history(32,'h');{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(1000,history));QVERIFY(journal.recordLiveUse(identity(),2000));QVERIFY(journal.closeClean(history));}const qint64 later=WorktreeUnusedMs*2;{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(later,history));const auto record=journal.record(identity());QVERIFY(evaluateWorktreeCleanup(facts(),&record,journal.validatedHealth(true),later).eligible);/* no clean close: simulate interrupted run */}{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(later+100,history));QCOMPARE(journal.health().recoveryProtectionSince,later+100);const auto record=journal.record(identity());QCOMPARE(evaluateWorktreeCleanup(facts(),&record,journal.validatedHealth(true),later+100).reason,"recovery-protection");}}
    void changedOrUnknownHistoryRevisionStartsRecoveryFloor(){QTemporaryDir dir;const QByteArray oldRevision(32,'a');{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(0,oldRevision));QVERIFY(journal.recordLiveUse(identity(),0));QVERIFY(journal.closeClean(oldRevision));}{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(WorktreeUnusedMs*2,QByteArray(32,'b')));QCOMPARE(journal.health().recoveryProtectionSince,WorktreeUnusedMs*2);QVERIFY(journal.closeClean(QByteArray(32,'b')));}{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(WorktreeUnusedMs*3));QCOMPARE(journal.health().recoveryProtectionSince,WorktreeUnusedMs*3);}}
    void journalLockBlocksSecondWriter(){QTemporaryDir dir;WorktreeUsageJournal one(dir.path()),two(dir.path());QVERIFY(one.open(0));const auto before=read(dir.filePath("worktree-usage.json"));QString error;QVERIFY(!two.open(1,{},&error));QVERIFY(error.contains("another application"));QVERIFY(!two.health().storageHealthy);QCOMPARE(read(dir.filePath("worktree-usage.json")),before);QVERIFY(one.recordLiveUse(identity(),2));}
    void storageAliasRetargetCannotRedirectWrites(){
#ifdef Q_OS_WIN
        QTemporaryDir base;const auto original=base.filePath("original"),other=base.filePath("other"),alias=base.filePath("alias");QVERIFY(QDir().mkpath(original));QVERIFY(QDir().mkpath(other));
        auto junction=[&](const QString &target){QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(alias),QDir::toNativeSeparators(target)});return process.waitForFinished(5000)&&process.exitCode()==0;};
        QVERIFY(junction(original));WorktreeUsageJournal journal(alias);QVERIFY(journal.open(0));const auto before=read(original+"/worktree-usage.json");QVERIFY(QFile::copy(original+"/worktree-usage.json",other+"/worktree-usage.json"));
        QVERIFY(RemoveDirectoryW(QDir::toNativeSeparators(alias).toStdWString().c_str()));QVERIFY(junction(other));QVERIFY(journal.recordLiveUse(identity(),1000));
        QCOMPARE(read(other+"/worktree-usage.json"),before);QVERIFY(read(original+"/worktree-usage.json")!=before);QVERIFY(RemoveDirectoryW(QDir::toNativeSeparators(alias).toStdWString().c_str()));
#else
        QSKIP("Windows junction fixture.");
#endif
    }
    void corruptAndOversizedStorageArePreserved_data(){QTest::addColumn<QByteArray>("bytes");QTest::newRow("json")<<QByteArray("{broken");QTest::newRow("oversize")<<QByteArray(4*1024*1024+1,'x');QTest::newRow("wrong-schema")<<QByteArray("{\"schemaVersion\":2}");}
    void corruptAndOversizedStorageArePreserved(){QFETCH(QByteArray,bytes);QTemporaryDir dir;const auto path=dir.filePath("worktree-usage.json");QVERIFY(write(path,bytes));WorktreeUsageJournal journal(dir.path());QString error;QVERIFY(!journal.open(1000,{},&error));QVERIFY(!error.isEmpty());QVERIFY(!journal.recordLiveUse(identity(),2000));QCOMPARE(read(path),bytes);}
    void duplicateIdentitiesAreRejected(){QTemporaryDir dir;{WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(1000));QVERIFY(journal.recordLiveUse(identity(),2000));QVERIFY(journal.closeClean());}const auto path=dir.filePath("worktree-usage.json");auto object=QJsonDocument::fromJson(read(path)).object();auto records=object["records"].toArray();records.append(records.first());object["records"]=records;const auto bytes=QJsonDocument(object).toJson();QVERIFY(write(path,bytes));WorktreeUsageJournal journal(dir.path());QVERIFY(!journal.open(3000));QCOMPARE(read(path),bytes);}
    void externalConflictPreservesDiskAndMemory(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(1000));QVERIFY(journal.recordLiveUse(identity(),2000));const auto path=dir.filePath("worktree-usage.json");const QByteArray external="foreign writer bytes";QVERIFY(write(path,external));QVERIFY(!journal.recordLiveUse(identity(),3000));QCOMPARE(journal.record(identity()).lastUsedAt,2000);QVERIFY(!journal.health().storageHealthy);QCOMPARE(read(path),external);QVERIFY(!journal.closeClean());QCOMPARE(read(path),external);}
    void cleanupHealthMustValidateTheCurrentJournalRevision(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(0));QVERIFY(journal.recordLiveUse(identity(),1000));const auto record=journal.record(identity());QVERIFY(evaluateWorktreeCleanup(facts(),&record,journal.validatedHealth(true),WorktreeUnusedMs*2).eligible);QVERIFY(!journal.health().trackingScopeComplete);const auto path=dir.filePath("worktree-usage.json");auto object=QJsonDocument::fromJson(read(path)).object();auto records=object["records"].toArray();auto altered=records[0].toObject();altered["lastUsedAt"]=WorktreeUnusedMs*2;records[0]=altered;object["records"]=records;const auto external=QJsonDocument(object).toJson();QVERIFY(write(path,external));QString error;const auto health=journal.validatedHealth(true,&error);QVERIFY(!health.storageHealthy);QVERIFY(!error.isEmpty());QVERIFY(!evaluateWorktreeCleanup(facts(),&record,health,WorktreeUnusedMs*2).eligible);QCOMPARE(read(path),external);}
    void snoozesPruneOnlyAfterCompleteSuccessfulScan(){QTemporaryDir dir;WorktreeUsageJournal journal(dir.path());QVERIFY(journal.open(0));const auto a=identity(),b=identity("second","C:/fixture/second");QVERIFY(journal.recordLiveUse(a,1000));QVERIFY(journal.recordLiveUse(b,1000));QVERIFY(journal.snooze({a,b},2000));const auto path=dir.filePath("worktree-usage.json");const auto before=read(path);QVERIFY(!journal.pruneSnoozes({},false,3000));QCOMPARE(read(path),before);QVERIFY(journal.pruneSnoozes({a},true,3000));QCOMPARE(journal.record(a).dismissedAt,2000);QCOMPARE(journal.record(b).dismissedAt,-1);QVERIFY(journal.pruneSnoozes({a},true,2000+WorktreeUnusedMs));QCOMPARE(journal.record(a).dismissedAt,-1);}
    void physicalIdentitySurvivesCheckoutButChangesAfterRecreation(){
#ifdef Q_OS_WIN
        GitFixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));QString error;const auto old=probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error);QVERIFY2(old.valid(),qPrintable(error));
        QVERIFY(f.run({"checkout","-b","next"},f.linked));QCOMPARE(probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error).instanceToken,old.instanceToken);
        QVERIFY(f.run({"worktree","remove",f.linked}));QVERIFY(f.run({"worktree","add","-b","recreated",f.linked}));const auto fresh=probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error);QVERIFY2(fresh.valid(),qPrintable(error));QVERIFY(fresh.instanceToken!=old.instanceToken);QCOMPARE(fresh.path,old.path);
        WorktreeUsageJournal journal(f.base.filePath("journal"));QVERIFY(journal.open(0));QVERIFY(journal.recordLiveUse(old,1000));QVERIFY(journal.observeImported(fresh,2000));QCOMPARE(journal.record(fresh).evidence,WorktreeUsageEvidence::UnknownImportedHistory);
#else
        QSKIP("Windows physical identity implementation.");
#endif
    }
    void identityProbeRejectsChangedMetadataRelationships(){
#ifdef Q_OS_WIN
        GitFixture f;QVERIFY(f.error.isEmpty());QString error;QVERIFY(probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error).valid());
        QVERIFY(write(f.admin+"/commondir","../../different\n"));QVERIFY(!probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error).valid());QVERIFY(error.contains("relationships"));
        QVERIFY(write(f.admin+"/commondir","../..\n"));QVERIFY(write(f.linked+"/.git",("gitdir: "+f.common+"\n").toUtf8()));QVERIFY(!probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error).valid());QVERIFY(error.contains("pointer"));
#else
        QSKIP("Windows physical identity implementation.");
#endif
    }
    void recreatedRootWithSameAdministrativeDirectoryIsANewInstance(){
#ifdef Q_OS_WIN
        GitFixture f;QVERIFY(f.error.isEmpty());QString error;const auto old=probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error);QVERIFY2(old.valid(),qPrintable(error));const auto priorRoot=f.base.filePath("prior-root");
        QVERIFY(QDir().rename(f.linked,priorRoot));QVERIFY(QDir().mkpath(f.linked));QVERIFY(QFile::copy(priorRoot+"/.git",f.linked+"/.git"));QVERIFY(QFile::copy(priorRoot+"/file.txt",f.linked+"/file.txt"));
        const auto fresh=probeWorktreeUsageIdentity(f.snapshot(),f.admin,&error);QVERIFY2(fresh.valid(),qPrintable(error));QVERIFY(fresh.instanceToken!=old.instanceToken);
#else
        QSKIP("Windows physical identity implementation.");
#endif
    }
};
QTEST_GUILESS_MAIN(UsageTests)
#include "test_git_usage.moc"
