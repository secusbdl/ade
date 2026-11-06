#include <QtTest>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QTemporaryDir>
#include <QTreeWidget>
#include "git/GitPanel.h"
#include "git/GitRepositoryWatcher.h"
#include "git/ProcessRunner.h"
#include "git/WorktreeSettingsController.h"
using namespace ade;
namespace {
struct Fixture {
    QTemporaryDir base;QString root,managed,error;
    Fixture(){root=base.filePath("repository");managed=base.filePath("managed");QDir().mkpath(root);run({"init","-b","main"});run({"config","user.name","ADE watcher UI"});run({"config","user.email","fixture@example.invalid"});run({"config","commit.gpgsign","false"});run({"config","core.hooksPath",base.filePath("hooks")});QFile file(root+"/file.txt");if(file.open(QIODevice::WriteOnly)){file.write("base\n");file.close();}run({"add","."});run({"commit","-m","base"});}
    bool run(const QStringList &args){QProcess process;auto env=gitdetail::safeEnvironment();env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global"));process.setProcessEnvironment(env);process.setWorkingDirectory(root);process.start("git",args);if(!process.waitForFinished(15000)||process.exitCode()!=0){error=QString::fromUtf8(process.readAllStandardError());return false;}return true;}
    GitServiceOptions options()const{GitServiceOptions o;o.managedWorktreeRoot=managed;return o;}
    GitWorkspaceContext context()const{GitWorkspaceContext c;c.trustedRoots={root};return c;}
};
}
class GitWatcherUiTests:public QObject {
    Q_OBJECT
private slots:
    void externalTopologyRefreshesHiddenPanelAndDrafts(){
        Fixture f;QVERIFY2(f.error.isEmpty(),qPrintable(f.error));GitPanel panel(f.options());panel.setWorkspaceContext(f.context());panel.setWorkspace(f.root,"chat");
        GitService *service=panel.service();QVERIFY(service);WorktreeSettingsController settings(service);settings.refresh();
        auto *watcher=service->findChild<GitRepositoryWatcher*>();QVERIFY(watcher);QSignalSpy ready(watcher,&GitRepositoryWatcher::ready),changed(service,&GitService::repositoryTopologyChanged),completed(service,&GitService::completed);
        QVERIFY(service->watchWorkspace("chat",f.root));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);QTRY_VERIFY_WITH_TIMEOUT(!settings.isLoading()&&!settings.repositoryIds().isEmpty(),15000);
        const QString id=settings.repositoryIds().first();settings.setDraft(id,GitSetupVariant::Posix,"# unsaved draft\necho local\n");
        auto *tree=panel.findChild<QTreeWidget*>("gitWorktrees");QVERIFY(tree);QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),1,15000);
        QVERIFY(!panel.isVisible());const QString linked=f.managed+"/fixture/external";QVERIFY(QDir().mkpath(QFileInfo(linked).absolutePath()));QVERIFY(f.run({"worktree","add","-b","external",linked}));
        QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,10000);QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),2,15000);QTRY_COMPARE_WITH_TIMEOUT(settings.repository(id).worktrees.size(),1,15000);
        QCOMPARE(settings.draft(id,GitSetupVariant::Posix).text,QString("# unsaved draft\necho local\n"));QVERIFY(settings.hasUnsavedDrafts());
        // Initial panel reads (4), owner inspection (1), settings read (1),
        // then topology panel/settings refresh (5) must finish before checking
        // quiescence. A late branch query is legitimate, not a feedback loop.
        QTRY_VERIFY_WITH_TIMEOUT(completed.size()>=11,15000);const auto settled=completed.size();QTest::qWait(900);QCOMPARE(completed.size(),settled);QCOMPARE(changed.size(),1);
        QVERIFY(f.run({"checkout","-b","next"}));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),2,10000);
        auto *heading=panel.findChild<QLabel*>("gitHeading");QVERIFY(heading);QTRY_VERIFY_WITH_TIMEOUT(heading->text().contains("next"),15000);
    }
    void stalePanelContextDoesNotRefreshWrongRepository(){
        Fixture a,b;QVERIFY(a.error.isEmpty());QVERIFY(b.error.isEmpty());GitPanel panel(a.options());auto context=a.context();context.trustedRoots.append(b.root);panel.setWorkspaceContext(context);panel.setWorkspace(b.root,"b");
        auto *service=panel.service();auto *watcher=service->findChild<GitRepositoryWatcher*>();QVERIFY(watcher);QSignalSpy ready(watcher,&GitRepositoryWatcher::ready),changed(service,&GitService::repositoryTopologyChanged),done(service,&GitService::completed);
        QVERIFY(service->watchWorkspace("a",a.root));QTRY_COMPARE_WITH_TIMEOUT(ready.size(),1,15000);
        auto *tree=panel.findChild<QTreeWidget*>("gitWorktrees");QVERIFY(tree);QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),1,15000);
        // The panel's four initial reads and the separate owner inspection
        // must all settle before attributing any new read to another repo.
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),5,15000);const auto count=done.size();
        QVERIFY(a.run({"checkout","-b","a-changed"}));QTRY_COMPARE_WITH_TIMEOUT(changed.size(),1,10000);QTest::qWait(500);QCOMPARE(done.size(),count);
        auto *heading=panel.findChild<QLabel*>("gitHeading");QVERIFY(heading);QVERIFY(heading->text().contains("main"));
    }
};
QTEST_MAIN(GitWatcherUiTests)
#include "test_git_watcher_ui.moc"
