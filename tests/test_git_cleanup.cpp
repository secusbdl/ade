#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QScopeGuard>
#include <QThread>
#include <cstdio>
#include "git/GitService.h"
#include "git/ProcessRunner.h"
#include "git/WorktreeCleanupController.h"
using namespace ade;
namespace {
bool write(const QString& path,const QByteArray& bytes){QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size()&&f.flush();}
QByteArray read(const QString& path){QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray();}
GitResult waitFor(GitService& service,quint64 id){QSignalSpy spy(&service,&GitService::completed);QElapsedTimer timer;timer.start();while(timer.elapsed()<15000){for(const auto& row:spy)if(row[0].toULongLong()==id)return qvariant_cast<GitResult>(row[1]);QTest::qWait(5);}GitResult r;r.error.message="Fixture request timed out";return r;}
struct Fixture {
    QTemporaryDir temp;QString root,managed,linked,data,git,error;QByteArray revision=QByteArray(32,'h');
    Fixture(){root=temp.filePath("repository");managed=temp.filePath("managed");linked=managed+"/topic";data=temp.filePath("profile");git=QStandardPaths::findExecutable("git");QDir().mkpath(root);QDir().mkpath(managed);QDir().mkpath(data);run({"init","-b","main"});run({"config","user.name","ADE cleanup fixture"});run({"config","user.email","fixture@example.invalid"});run({"config","commit.gpgsign","false"});run({"config","core.autocrlf","false"});run({"config","core.hooksPath",temp.filePath("no-hooks")});write(root+"/file.txt","original\n");run({"add","."});run({"commit","-m","fixture"});run({"worktree","add","-b","topic",linked,"main"});}
    bool run(const QStringList& args,QByteArray* output=nullptr){QProcess p;p.setWorkingDirectory(root);auto env=gitdetail::safeEnvironment();env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",temp.filePath("no-global"));p.setProcessEnvironment(env);p.start(git,args);if(!p.waitForFinished(15000)||p.exitCode()!=0){error=QString::fromUtf8(p.readAllStandardError());return false;}if(output)*output=p.readAllStandardOutput();return true;}
    GitServiceOptions options()const{GitServiceOptions o;o.gitExecutable=git;o.managedWorktreeRoot=managed;return o;}
    GitWorkspaceContext context()const{GitWorkspaceContext c;c.trustedRoots={root};return c;}
    bool seed(GitService& service,bool known=true){const auto r=waitFor(service,service.inspectUsage(linked));if(!r.ok||r.usage.size()!=1){error=r.error.message;return false;}const auto old=QDateTime::currentMSecsSinceEpoch()-10LL*24*60*60*1000;WorktreeUsageJournal journal(data);if(!journal.open(old,revision,&error))return false;const bool recorded=known?journal.recordLiveUse(r.usage.first().identity,old,&error):journal.observeImported(r.usage.first().identity,old,old,&error);return recorded&&journal.closeClean(revision,&error);}
};
}
class CleanupTests:public QObject {
    Q_OBJECT
private slots:
    void reviewDoesNotDeleteAndExplicitRemovalRetainsBranch(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitService service(f.options());service.setWorkspaceContext(f.context());QVERIFY2(f.seed(service),qPrintable(f.error));
        WorktreeCleanupController controller(&service,f.data,f.revision);controller.refresh();QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),15000);QCOMPARE(controller.rows().size(),1);QVERIFY(controller.rows().first().decision.eligible);QVERIFY(QFileInfo::exists(f.linked+"/file.txt"));
        QSignalSpy removed(&controller,&WorktreeCleanupController::worktreeRemovalFinished);controller.removeReviewed({controller.rows().first().facts.identity.key()});QTRY_COMPARE_WITH_TIMEOUT(removed.size(),1,15000);QVERIFY(removed.first()[1].toBool());QVERIFY(!QFileInfo::exists(f.linked));QByteArray branch;QVERIFY(f.run({"branch","--list","topic"},&branch));QVERIFY(branch.contains("topic"));QVERIFY(controller.closeClean(f.revision));
    }
    void unknownHistoryAndRecoveryNeverAuthorizeRemoval_data(){QTest::addColumn<bool>("known");QTest::addColumn<bool>("revisionMatches");QTest::newRow("unknown")<<false<<true;QTest::newRow("changed-history")<<true<<false;}
    void unknownHistoryAndRecoveryNeverAuthorizeRemoval(){QFETCH(bool,known);QFETCH(bool,revisionMatches);Fixture f;GitService service(f.options());service.setWorkspaceContext(f.context());QVERIFY(f.seed(service,known));WorktreeCleanupController controller(&service,f.data,revisionMatches?f.revision:QByteArray(32,'x'));controller.refresh();QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),15000);QCOMPARE(controller.rows().size(),1);QVERIFY(!controller.rows().first().decision.eligible);QCOMPARE(controller.rows().first().decision.reason,known?"recovery-protection":"usage-history-unknown");QVERIFY(QFileInfo::exists(f.linked));}
    void freshGuardsRejectReviewedChange_data(){QTest::addColumn<QString>("change");for(const auto* name:{"dirty","mounted","journal","instance"})QTest::newRow(name)<<QString::fromLatin1(name);}
    void freshGuardsRejectReviewedChange(){
        QFETCH(QString,change);Fixture f;GitService service(f.options());auto context=f.context();service.setWorkspaceContext(context);QVERIFY(f.seed(service));WorktreeCleanupController controller(&service,f.data,f.revision);controller.refresh();QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),15000);QCOMPARE(controller.rows().size(),1);const auto key=controller.rows().first().facts.identity.key();QVERIFY(controller.rows().first().decision.eligible);
        if(change=="dirty")QVERIFY(write(f.linked+"/file.txt","user edit\n"));
        else if(change=="mounted"){context.mountedPaths={f.linked};service.setWorkspaceContext(context);}
        else if(change=="journal")QVERIFY(write(f.data+"/worktree-usage.json",read(f.data+"/worktree-usage.json")+" "));
        else {QVERIFY(f.run({"worktree","remove",f.linked}));QVERIFY(f.run({"worktree","add","-b","replacement",f.linked,"main"}));}
        QSignalSpy errors(&controller,&WorktreeCleanupController::errorOccurred);controller.removeReviewed({key});QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),15000);QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(),15000);QVERIFY(QFileInfo::exists(f.linked+"/file.txt"));if(change=="dirty")QCOMPARE(read(f.linked+"/file.txt"),"user edit\n");
    }
    void snoozeSurvivesCleanRestart(){Fixture f;GitService service(f.options());service.setWorkspaceContext(f.context());QVERIFY(f.seed(service));{WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QVERIFY(c.rows().first().decision.eligible);QString error;QVERIFY2(c.snooze({c.rows().first().facts.identity.key()},&error),qPrintable(error));QVERIFY(c.closeClean(f.revision));}{WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QCOMPARE(c.rows().first().decision.reason,"snoozed");}QVERIFY(QFileInfo::exists(f.linked));}
    void cleanShutdownRecordsFinalLiveUse(){Fixture f;GitService service(f.options());auto context=f.context();context.mountedPaths={f.linked};service.setWorkspaceContext(context);QVERIFY(f.seed(service));qint64 shutdown=0;{WorktreeCleanupController c(&service,f.data,f.revision);c.updateOwners({{"background",{f.linked,1}}});QTRY_VERIFY_WITH_TIMEOUT(c.trackingReady(),15000);QTest::qWait(30);shutdown=QDateTime::currentMSecsSinceEpoch();QVERIFY(c.closeClean(f.revision));}context.mountedPaths.clear();service.setWorkspaceContext(context);WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QCOMPARE(c.rows().size(),1);QVERIFY2(c.rows().first().decision.lastUsedAt>=shutdown,"A live background mount must be recorded at final shutdown, not only initial activation.");QCOMPARE(c.rows().first().decision.reason,"recent-use");}
    void newLiveMountRevokesReviewAndReleaseRecordsUse(){Fixture f;GitService service(f.options());auto context=f.context();service.setWorkspaceContext(context);QVERIFY(f.seed(service));WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);const auto key=c.rows().first().facts.identity.key();context.mountedPaths={f.linked};service.setWorkspaceContext(context);c.updateOwners({{"chat",{f.linked,1}}});QVERIFY(c.rows().isEmpty());QSignalSpy errors(&c,&WorktreeCleanupController::errorOccurred);c.removeReviewed({key});QVERIFY(!errors.isEmpty());QTRY_VERIFY_WITH_TIMEOUT(c.trackingReady(),15000);context.mountedPaths.clear();service.setWorkspaceContext(context);c.updateOwners({});c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QCOMPARE(c.rows().first().decision.reason,"recent-use");}
    void destructionDuringStartedSignalPreventsDispatch(){Fixture f;GitService service(f.options());service.setWorkspaceContext(f.context());QVERIFY(f.seed(service));QPointer<WorktreeCleanupController> c=new WorktreeCleanupController(&service,f.data,f.revision);c->refresh();QTRY_VERIFY_WITH_TIMEOUT(!c->busy(),15000);const auto key=c->rows().first().facts.identity.key();connect(c,&WorktreeCleanupController::mutationStarted,&service,[&]{delete c;});c->removeReviewed({key});QVERIFY(!c);QTest::qWait(50);QVERIFY(QFileInfo::exists(f.linked));}
    void mountDuringStartedSignalRevokesBeforeDispatch(){Fixture f;GitService service(f.options());auto context=f.context();service.setWorkspaceContext(context);QVERIFY(f.seed(service));WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);const auto key=c.rows().first().facts.identity.key();connect(&c,&WorktreeCleanupController::mutationStarted,&service,[&]{context.mountedPaths={f.linked};service.setWorkspaceContext(context);c.updateOwners({{"chat",{f.linked,1}}});});QSignalSpy finished(&c,&WorktreeCleanupController::worktreeRemovalFinished);c.removeReviewed({key});QCOMPARE(finished.size(),1);QVERIFY(!finished.first()[1].toBool());QVERIFY(QFileInfo::exists(f.linked));QTRY_VERIFY_WITH_TIMEOUT(c.trackingReady(),15000);}
    void booleanHistoryFlagCannotAuthorizeCleanup(){Fixture f;GitService service(f.options());auto context=f.context();context.usageKnown=true;service.setWorkspaceContext(context);QCOMPARE(waitFor(service,service.cleanupCandidates()).error.code,GitErrorCode::UnsafeCleanup);QCOMPARE(waitFor(service,service.removeWorktree(f.linked,GitRemovePolicy::Cleanup)).error.code,GitErrorCode::UnsafeCleanup);QVERIFY(QFileInfo::exists(f.linked));}
    void competingJournalAndIncompleteScanFailClosed(){Fixture f;GitService service(f.options());service.setWorkspaceContext(f.context());QVERIFY(f.seed(service));WorktreeCleanupController c(&service,f.data,f.revision);WorktreeCleanupController second(&service,f.data,f.revision);QVERIFY(!second.trackingReady());QVERIFY(QDir().mkpath(f.managed+"/broken"));QVERIFY(write(f.managed+"/broken/.git","invalid\n"));QSignalSpy errors(&c,&WorktreeCleanupController::errorOccurred);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QVERIFY(!errors.isEmpty());QVERIFY(c.rows().isEmpty());QVERIFY(QFileInfo::exists(f.linked));}
    void checkoutAfterFinalStatusCannotRemoveUnmergedBranch_data(){QTest::addColumn<QString>("mode");QTest::newRow("different-branch")<<QString("checkout");QTest::newRow("same-branch-new-head")<<QString("head");}
    void checkoutAfterFinalStatusCannotRemoveUnmergedBranch(){
        QFETCH(QString,mode);
        Fixture f;QVERIFY(f.run({"-C",f.linked,"checkout","-b","unmerged"}));QVERIFY(write(f.linked+"/file.txt","unmerged committed change\n"));QVERIFY(f.run({"-C",f.linked,"add","."}));QVERIFY(f.run({"-C",f.linked,"commit","-m","unmerged"}));QVERIFY(f.run({"-C",f.linked,"checkout","topic"}));
        const auto trigger=f.temp.filePath("wrapper-trigger");
        qputenv("ADE_CLEANUP_REAL_GIT",f.git.toUtf8());qputenv("ADE_CLEANUP_TRIGGER",trigger.toUtf8());qputenv("ADE_CLEANUP_LINKED",f.linked.toUtf8());qputenv("ADE_CLEANUP_MODE",mode.toUtf8());
        const auto clear=qScopeGuard([]{qunsetenv("ADE_CLEANUP_REAL_GIT");qunsetenv("ADE_CLEANUP_TRIGGER");qunsetenv("ADE_CLEANUP_LINKED");qunsetenv("ADE_CLEANUP_MODE");});
        auto options=f.options();options.gitExecutable=QCoreApplication::applicationFilePath();options.readTimeoutMs=5000;GitService service(options);service.setWorkspaceContext(f.context());QVERIFY2(f.seed(service),qPrintable(f.error));
        WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QCOMPARE(c.rows().size(),1);QVERIFY(c.rows().first().decision.eligible);const auto key=c.rows().first().facts.identity.key();
        // The helper performs a real checkout after the third removal-time
        // status process has returned clean bytes, before ADE receives them.
        QVERIFY(write(trigger,"0"));QSignalSpy removed(&c,&WorktreeCleanupController::worktreeRemovalFinished);c.removeReviewed({key});QTRY_COMPARE_WITH_TIMEOUT(removed.size(),1,20000);
        QCOMPARE(read(trigger),"3");QVERIFY2(!removed.first()[1].toBool(),"A changed HEAD/branch invalidates the earlier merged-branch review.");QVERIFY(QFileInfo::exists(f.linked));QCOMPARE(read(f.linked+"/file.txt"),"unmerged committed change\n");
    }
    void destructionCancelsInFlightPreRemovalInspection(){
        Fixture f;const auto trigger=f.temp.filePath("wrapper-trigger");qputenv("ADE_CLEANUP_REAL_GIT",f.git.toUtf8());qputenv("ADE_CLEANUP_TRIGGER",trigger.toUtf8());qputenv("ADE_CLEANUP_MODE","hold");
        const auto clear=qScopeGuard([]{qunsetenv("ADE_CLEANUP_REAL_GIT");qunsetenv("ADE_CLEANUP_TRIGGER");qunsetenv("ADE_CLEANUP_MODE");});
        auto options=f.options();options.gitExecutable=QCoreApplication::applicationFilePath();options.readTimeoutMs=15000;GitService service(options);service.setWorkspaceContext(f.context());QVERIFY(f.seed(service));QPointer<WorktreeCleanupController> c=new WorktreeCleanupController(&service,f.data,f.revision);c->refresh();QTRY_VERIFY_WITH_TIMEOUT(!c->busy(),15000);QVERIFY(c->rows().first().decision.eligible);const auto key=c->rows().first().facts.identity.key();QVERIFY(write(trigger,"0"));QSignalSpy completed(&service,&GitService::completed);c->removeReviewed({key});QTRY_COMPARE_WITH_TIMEOUT(read(trigger),"1",6000);delete c;QVERIFY(!c);QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty(),6000);const auto result=qvariant_cast<GitResult>(completed.last()[1]);QCOMPARE(result.error.code,GitErrorCode::Cancelled);QVERIFY(!result.mayHaveMutated);QVERIFY(QFileInfo::exists(f.linked));
    }
    void managedRootRetargetCannotAdoptKnownExternalWorktree(){
#ifndef Q_OS_WIN
        QSKIP("Windows junction authorization boundary.");
#else
        Fixture f;QVERIFY(f.run({"worktree","remove",f.linked}));const auto outside=f.temp.filePath("external-root");QVERIFY(QDir().mkpath(outside));f.linked=outside+"/external";QVERIFY(f.run({"worktree","add","-b","external",f.linked,"main"}));
        GitService service(f.options());auto context=f.context();context.trustedRoots.append(f.linked);service.setWorkspaceContext(context);QVERIFY(f.seed(service));WorktreeCleanupController c(&service,f.data,f.revision);
        const auto previous=f.temp.filePath("original-managed");QVERIFY(QDir().rename(f.managed,previous));QProcess link;link.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(f.managed),QDir::toNativeSeparators(outside)});QVERIFY(link.waitForFinished(5000));QCOMPARE(link.exitCode(),0);
        const auto removeLink=qScopeGuard([&]{QDir().rmdir(f.managed);});c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);
        for(const auto& row:c.rows()){QVERIFY2(!row.decision.eligible,"Replacing the managed-root path with a junction cannot authorize an external, previously used worktree.");}QVERIFY(QFileInfo::exists(f.linked+"/file.txt"));
#endif
    }
    void managedRootUnderAuthorizedJunctionParentStillWorks(){
#ifndef Q_OS_WIN
        QSKIP("Windows junction authorization boundary.");
#else
        Fixture f;const auto alias=f.temp.filePath("parent-alias");QProcess link;link.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(alias),QDir::toNativeSeparators(f.temp.path())});QVERIFY(link.waitForFinished(5000));QCOMPARE(link.exitCode(),0);const auto cleanup=qScopeGuard([&]{QDir().rmdir(alias);});auto options=f.options();options.managedWorktreeRoot=alias+"/managed";GitService service(options);service.setWorkspaceContext(f.context());QVERIFY(f.seed(service));WorktreeCleanupController c(&service,f.data,f.revision);c.refresh();QTRY_VERIFY_WITH_TIMEOUT(!c.busy(),15000);QCOMPARE(c.rows().size(),1);QVERIFY(c.rows().first().decision.eligible);
#endif
    }
};
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    if(app.arguments().value(1)=="--literal-pathspecs"&&!qEnvironmentVariableIsEmpty("ADE_CLEANUP_REAL_GIT")){
        QProcess git;git.start(qEnvironmentVariable("ADE_CLEANUP_REAL_GIT"),app.arguments().mid(1));if(!git.waitForFinished(20000))return 125;
        const auto out=git.readAllStandardOutput(),err=git.readAllStandardError();
        const auto marker=qEnvironmentVariable("ADE_CLEANUP_TRIGGER");
        if(git.exitCode()==0&&app.arguments().contains("status")&&QFileInfo::exists(marker)){
            const int count=read(marker).toInt()+1;if(!write(marker,QByteArray::number(count)))return 126;
            if(qEnvironmentVariable("ADE_CLEANUP_MODE")=="hold"&&count==1){QElapsedTimer deadline;deadline.start();while(deadline.elapsed()<20000)QThread::msleep(10);return 124;}
            if(count==3){QProcess checkout;QStringList args{"-C",qEnvironmentVariable("ADE_CLEANUP_LINKED")};if(qEnvironmentVariable("ADE_CLEANUP_MODE")=="head")args<<"reset"<<"--hard"<<"unmerged";else args<<"checkout"<<"unmerged";checkout.start(qEnvironmentVariable("ADE_CLEANUP_REAL_GIT"),args);if(!checkout.waitForFinished(15000)||checkout.exitCode()!=0)return 127;}
        }
        std::fwrite(out.constData(),1,size_t(out.size()),stdout);std::fwrite(err.constData(),1,size_t(err.size()),stderr);return git.exitCode();
    }
    CleanupTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "test_git_cleanup.moc"
