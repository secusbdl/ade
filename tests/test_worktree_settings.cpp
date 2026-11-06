#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include "git/GitService.h"
#include "git/ProcessRunner.h"
#include "git/SetupScriptText.h"
#include "git/WorktreeSettingsController.h"
#include "git/WorktreeSettingsPane.h"
#include "editor/CodeEditor.h"
using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
QByteArray read(const QString &path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
struct Fixture {
    QTemporaryDir base;QString root,managed,error;QStringList junctions;
    Fixture(){root=base.filePath("repository");managed=base.filePath("managed");QDir().mkpath(root);run({"init","-b","main"});run({"config","user.name","ADE Settings Fixture"});run({"config","user.email","fixture@example.invalid"});run({"config","commit.gpgsign","false"});run({"config","core.autocrlf","false"});run({"config","core.hooksPath",base.filePath("hooks")});put(root+"/one.txt","base\n");run({"add","."});run({"commit","-m","base"});}
    ~Fixture(){for(const auto &junction:junctions)QDir().rmdir(junction);}
    bool run(const QStringList &args){QProcess p;p.setWorkingDirectory(root);auto env=gitdetail::safeEnvironment();env.insert("GIT_CONFIG_NOSYSTEM","1");env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global"));p.setProcessEnvironment(env);p.start("git",args);if(!p.waitForFinished(15000)||p.exitCode()!=0){error=QString::fromUtf8(p.readAllStandardError());return false;}return true;}
    QString linked(){const auto path=managed+"/fixture/linked";QDir().mkpath(QFileInfo(path).absolutePath());return run({"worktree","add","-b","linked",path,"main"})?path:QString();}
    bool junction(const QString &path,const QString &target){QProcess p;p.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(target)});if(!p.waitForFinished(5000)||p.exitCode()!=0)return false;junctions.append(path);return true;}
    GitServiceOptions options()const{GitServiceOptions o;o.managedWorktreeRoot=managed;return o;}
    GitWorkspaceContext context()const{GitWorkspaceContext c;c.trustedRoots={root};return c;}
};
GitResult wait(GitService &service,quint64 id){QSignalSpy spy(&service,&GitService::completed);QElapsedTimer time;time.start();while(time.elapsed()<20000){for(const auto &row:spy)if(row[0].toULongLong()==id)return qvariant_cast<GitResult>(row[1]);QCoreApplication::processEvents();QTest::qWait(5);}GitResult result;result.error.message="Fixture timeout";return result;}
GitSetupRepository listed(GitService &s){auto r=wait(s,s.setupRepositories());return r.ok&&!r.setupRepositories.isEmpty()?r.setupRepositories.first():GitSetupRepository();}
bool loaded(WorktreeSettingsController &model){QElapsedTimer t;t.start();while(model.isLoading()&&t.elapsed()<20000){QCoreApplication::processEvents();QTest::qWait(5);}return !model.isLoading()&&!model.repositoryIds().isEmpty();}
}
class WorktreeSettingsTest:public QObject {
    Q_OBJECT
private slots:
    void trustedLinkedSavePreservesBomAndMixedEol();
    void revisionsRejectExternalCreationEditAndDeletion();
    void invalidUtf8RemainsVisibleWithoutOverwrite();
    void setupJunctionCannotEscapeRepository();
    void draftsAreIsolatedAndExternalRefreshPreservesEdits();
    void viewRecreationRetainsDraftAndMutationCompletion();
    void settingsRemovalRequiresHostAndFreshState();
    void dirtyCloseShortcutAndClipboard();
    void controllerCancelsOnlyItsOwnRequests();
    void largeDelimiterInputIsBounded();
    void typingPreservesUnrelatedTreeAndUndo();
    void guardReentryCannotStartAnotherMutation();
    void controllerMayBeDeletedByLifecycleObservers();
    void columnsKeepStatusVisibleAtNarrowWidths();
};
void WorktreeSettingsTest::trustedLinkedSavePreservesBomAndMixedEol(){
    Fixture f;QVERIFY(f.error.isEmpty());const auto linked=f.linked();QVERIFY(!linked.isEmpty());const auto path=f.root+"/.stagewise/worktree-setup.ps1";
    const auto bytes=QByteArray::fromHex("efbbbf")+"one\r\ntwo\nthree\r";QVERIFY(put(path,bytes));GitService s(f.options());auto context=f.context();context.trustedRoots={linked};context.mountedPaths={linked};s.setWorkspaceContext(context);
    auto repo=listed(s);QCOMPARE(repo.mainWorktreePath,GitService::canonicalPath(f.root));QCOMPARE(repo.trustedWorkspaceAnchor,GitService::canonicalPath(linked));QCOMPARE(repo.powershell.content,"one\ntwo\nthree\n");
    auto result=wait(s,s.saveSetupScript(repo.trustedWorkspaceAnchor,GitSetupVariant::PowerShell,"ONE\ntwo\nthree\n",repo.powershell.revision));QVERIFY2(result.ok,qPrintable(result.error.message));
    QCOMPARE(read(path),QByteArray::fromHex("efbbbf")+"ONE\r\ntwo\nthree\r");QCOMPARE(result.setupScript.revision.sha256.size(),32);QVERIFY(result.setupScript.revision.exists);
}
void WorktreeSettingsTest::revisionsRejectExternalCreationEditAndDeletion(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());const auto path=f.root+"/.stagewise/worktree-setup.sh";auto repo=listed(s);QVERIFY(!repo.id.isEmpty());
    QVERIFY(put(path,"external\n"));auto result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::Posix,"draft\n",repo.posix.revision));QCOMPARE(result.error.code,GitErrorCode::Conflict);QCOMPARE(read(path),"external\n");
    repo=listed(s);QVERIFY(put(path,"changed\n"));result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::Posix,"draft\n",repo.posix.revision));QCOMPARE(result.error.code,GitErrorCode::Conflict);QCOMPARE(read(path),"changed\n");
    repo=listed(s);QVERIFY(QFile::remove(path));result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::Posix,"draft\n",repo.posix.revision));QCOMPARE(result.error.code,GitErrorCode::Conflict);QVERIFY(!QFileInfo::exists(path));
    result=wait(s,s.saveSetupScript(f.root,static_cast<GitSetupVariant>(9),"draft",{}));QCOMPARE(result.error.code,GitErrorCode::InvalidPath);
}
void WorktreeSettingsTest::invalidUtf8RemainsVisibleWithoutOverwrite(){
    Fixture f;const auto path=f.root+"/.stagewise/worktree-setup.ps1";const auto bytes=QByteArray::fromHex("fffe00d8");QVERIFY(put(path,bytes));GitService s(f.options());s.setWorkspaceContext(f.context());auto repo=listed(s);
    QVERIFY(!repo.id.isEmpty());QVERIFY(!repo.powershell.error.isEmpty());QVERIFY(repo.posix.error.isEmpty());auto result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::PowerShell,"no",repo.powershell.revision));QVERIFY(!result.ok);QCOMPARE(read(path),bytes);
    result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::Posix,"echo okay\n",repo.posix.revision));QVERIFY2(result.ok,qPrintable(result.error.message));
}
void WorktreeSettingsTest::setupJunctionCannotEscapeRepository(){
    Fixture f;const auto outside=f.base.filePath("outside");QVERIFY(QDir().mkpath(outside));QVERIFY(put(outside+"/worktree-setup.ps1","sentinel"));QVERIFY(f.junction(f.root+"/.stagewise",outside));
    GitService s(f.options());s.setWorkspaceContext(f.context());auto result=wait(s,s.saveSetupScript(f.root,GitSetupVariant::PowerShell,"overwrite",{}));QVERIFY(!result.ok);QCOMPARE(read(outside+"/worktree-setup.ps1"),"sentinel");
}
void WorktreeSettingsTest::draftsAreIsolatedAndExternalRefreshPreservesEdits(){
    Fixture a,b;QVERIFY(put(a.root+"/.stagewise/worktree-setup.sh","initial A\n"));QVERIFY(put(b.root+"/.stagewise/worktree-setup.ps1","initial B\n"));GitService s(a.options());auto c=a.context();c.trustedRoots.append(b.root);s.setWorkspaceContext(c);WorktreeSettingsController model(&s);model.setMutationGuard([](const QString &,GitOperation){return true;});model.refresh();QVERIFY(loaded(model));QCOMPARE(model.repositoryIds().size(),2);
    QString first,second;for(const auto &id:model.repositoryIds())if(model.repository(id).mainWorktreePath==GitService::canonicalPath(a.root))first=id;else second=id;QVERIFY(!first.isEmpty()&&!second.isEmpty());
    model.setDraft(first,GitSetupVariant::Posix,"draft A\n");model.selectRepository(second);model.selectVariant(GitSetupVariant::PowerShell);model.setDraft(second,GitSetupVariant::PowerShell,"draft B\n");
    QVERIFY(put(a.root+"/.stagewise/worktree-setup.sh","external A\n"));model.refresh();QVERIFY(loaded(model));QCOMPARE(model.draft(first,GitSetupVariant::Posix).text,"draft A\n");QVERIFY(model.draft(first,GitSetupVariant::Posix).conflict);QVERIFY(!model.canSave(first,GitSetupVariant::Posix));
    model.resetDraft(first,GitSetupVariant::Posix);QCOMPARE(model.draft(first,GitSetupVariant::Posix).text,"external A\n");QCOMPARE(model.draft(second,GitSetupVariant::PowerShell).text,"draft B\n");QVERIFY(model.hasUnsavedDrafts());
}
void WorktreeSettingsTest::viewRecreationRetainsDraftAndMutationCompletion(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);model.setMutationGuard([](const QString &,GitOperation){return true;});auto *pane=new WorktreeSettingsPane(&model);QVERIFY(loaded(model));const auto id=model.selectedRepository();model.setDraft(id,GitSetupVariant::PowerShell,"submitted\n");QSignalSpy started(&model,&WorktreeSettingsController::mutationStarted),finished(&model,&WorktreeSettingsController::mutationFinished);
    QVERIFY(model.save(id,GitSetupVariant::PowerShell));model.setDraft(id,GitSetupVariant::PowerShell,"newer unsaved draft\n");delete pane;QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QCOMPARE(started.size(),1);QVERIFY(finished[0][2].toBool());QVERIFY(loaded(model));
    QCOMPARE(read(f.root+"/.stagewise/worktree-setup.ps1"),"submitted\n");QCOMPARE(model.draft(id,GitSetupVariant::PowerShell).text,"newer unsaved draft\n");QVERIFY(model.hasUnsavedDrafts());WorktreeSettingsPane rebuilt(&model);QCOMPARE(rebuilt.findChild<CodeEditor *>("worktreeSetupEditor")->sourceText(),"newer unsaved draft\n");
}
void WorktreeSettingsTest::settingsRemovalRequiresHostAndFreshState(){
    Fixture f;const auto linked=f.linked();QVERIFY(!linked.isEmpty());GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);model.refresh();QVERIFY(loaded(model));const auto id=model.selectedRepository();QVERIFY(!model.canRemove(id,linked));model.setMutationGuard([](const QString &,GitOperation){return false;});QVERIFY(!model.remove(id,linked));QVERIFY(QFileInfo::exists(linked));
    model.setMutationGuard([](const QString &,GitOperation){return true;});QVERIFY(model.canRemove(id,linked));auto c=f.context();c.mountedPaths={linked};s.setWorkspaceContext(c);QSignalSpy finished(&model,&WorktreeSettingsController::worktreeRemovalFinished);QVERIFY(model.remove(id,linked));QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY(!finished[0][1].toBool());QVERIFY(QFileInfo::exists(linked));
    s.setWorkspaceContext(f.context());model.refresh();QVERIFY(loaded(model));QVERIFY(model.remove(id,linked));QTRY_COMPARE_WITH_TIMEOUT(finished.size(),2,20000);QVERIFY(finished[1][1].toBool());QVERIFY(!QFileInfo::exists(linked));QVERIFY(f.run({"show-ref","--verify","refs/heads/linked"}));
}
void WorktreeSettingsTest::dirtyCloseShortcutAndClipboard(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);model.setMutationGuard([](const QString &,GitOperation){return true;});WorktreeSettingsPane pane(&model);QVERIFY(loaded(model));auto *editor=pane.findChild<CodeEditor *>("worktreeSetupEditor");QVERIFY(editor);editor->setPlainText("saved with Ctrl S\n");QVERIFY(pane.hasUnsavedDrafts());QVERIFY(!pane.requestClose(WorktreeSettingsPane::CloseChoice::Cancel));
    QSignalSpy finished(&model,&WorktreeSettingsController::mutationFinished);QTest::keyClick(editor,Qt::Key_S,Qt::ControlModifier);QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);QVERIFY(finished[0][2].toBool());QVERIFY(loaded(model));QVERIFY(!pane.hasUnsavedDrafts());
    pane.findChild<QToolButton *>("copySTAGEWISE_TARGET_WORKTREE_PATH")->click();QCOMPARE(QApplication::clipboard()->text(),"STAGEWISE_TARGET_WORKTREE_PATH");editor->setPlainText("discard me");QVERIFY(pane.requestClose(WorktreeSettingsPane::CloseChoice::Discard));QCOMPARE(read(f.root+"/.stagewise/worktree-setup.ps1"),"saved with Ctrl S\n");
}
void WorktreeSettingsTest::controllerCancelsOnlyItsOwnRequests(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());auto *model=new WorktreeSettingsController(&s);model->refresh();const auto request=s.inspect(f.root);delete model;auto result=wait(s,request);QVERIFY2(result.ok,qPrintable(result.error.message));
}
void WorktreeSettingsTest::largeDelimiterInputIsBounded(){
    const QByteArray original(512*1024,'\n');QString decoded,error;QVERIFY(gitdetail::decodeSetupText(original,&decoded,&error));QByteArray encoded;QVERIFY(gitdetail::encodeSetupText(original,"x"+decoded,&encoded,&error));QCOMPARE(encoded,QByteArray("x")+original);
    QVERIFY(!gitdetail::encodeSetupText({},QString(1024*1024+1,'x'),&encoded,&error));QVERIFY(!gitdetail::decodeSetupText(QByteArray("a\0b",3),&decoded,&error));
}
void WorktreeSettingsTest::typingPreservesUnrelatedTreeAndUndo(){
    Fixture f;QVERIFY(!f.linked().isEmpty());GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);WorktreeSettingsPane pane(&model);QVERIFY(loaded(model));
    auto *tree=pane.findChild<QTreeWidget *>("settingsManagedWorktrees");QVERIFY(tree->topLevelItemCount()>0);const QPersistentModelIndex stable(tree->model()->index(0,0));
    auto *editor=pane.findChild<CodeEditor *>("worktreeSetupEditor");const auto before=editor->sourceText();editor->insertPlainText("# edit\n");QVERIFY(stable.isValid());QVERIFY(editor->document()->isUndoAvailable());editor->undo();QCOMPARE(editor->sourceText(),before);QVERIFY(stable.isValid());
}
void WorktreeSettingsTest::guardReentryCannotStartAnotherMutation(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);model.refresh();QVERIFY(loaded(model));const auto id=model.selectedRepository();model.setDraft(id,GitSetupVariant::PowerShell,"outer save\n");quint64 nested=42;int guardCalls=0;
    model.setMutationGuard([&](const QString &,GitOperation){++guardCalls;nested=model.save(id,GitSetupVariant::PowerShell);return true;});QSignalSpy finished(&model,&WorktreeSettingsController::mutationFinished);QVERIFY(model.save(id,GitSetupVariant::PowerShell));QCOMPARE(nested,quint64(0));QCOMPARE(guardCalls,1);QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000);
}
void WorktreeSettingsTest::controllerMayBeDeletedByLifecycleObservers(){
    Fixture f;GitService s(f.options());s.setWorkspaceContext(f.context());QPointer<WorktreeSettingsController> model=new WorktreeSettingsController(&s);model->refresh();QVERIFY(loaded(*model));const auto id=model->selectedRepository();model->setDraft(id,GitSetupVariant::PowerShell,"draft\n");model->setMutationGuard([](const QString &,GitOperation){return true;});
    connect(model,&WorktreeSettingsController::changed,&s,[&]{if(model&&model->isMutating())delete model;});const auto request=model->save(id,GitSetupVariant::PowerShell);QVERIFY(request>0);QVERIFY(model.isNull());
    auto *secondService=new GitService(f.options());secondService->setWorkspaceContext(f.context());QPointer<WorktreeSettingsController> second=new WorktreeSettingsController(secondService);second->refresh();QVERIFY(loaded(*second));const auto secondId=second->selectedRepository();second->setDraft(secondId,GitSetupVariant::PowerShell,"another\n");second->setMutationGuard([](const QString &,GitOperation){return true;});
    connect(second,&WorktreeSettingsController::mutationFinished,&s,[&]{delete second;});QVERIFY(second->save(secondId,GitSetupVariant::PowerShell));delete secondService;QVERIFY(second.isNull());
}
void WorktreeSettingsTest::columnsKeepStatusVisibleAtNarrowWidths(){
    Fixture f;QVERIFY(!f.linked().isEmpty());GitService s(f.options());s.setWorkspaceContext(f.context());WorktreeSettingsController model(&s);WorktreeSettingsPane pane(&model);QVERIFY(loaded(model));
    auto *tree=pane.findChild<QTreeWidget *>("settingsManagedWorktrees");QFont body("Arial");body.setPixelSize(13);pane.setFont(body);pane.resize(768,1050);pane.show();QCoreApplication::processEvents();
    QVERIFY2(tree->columnWidth(2)>=tree->fontMetrics().horizontalAdvance("Usage unknown")+16,qPrintable(QString("Column %1, text %2, viewport %3").arg(tree->columnWidth(2)).arg(tree->fontMetrics().horizontalAdvance("Usage unknown")).arg(tree->viewport()->width())));
    for(const int width:{768,420}){pane.resize(width,1050);QCoreApplication::processEvents();const int available=tree->viewport()->width();
        QVERIFY(tree->columnWidth(3)>=available*0.4-2);int sum=0;for(int column=0;column<4;++column)sum+=tree->columnWidth(column);QVERIFY(sum<=available+1);
    }
    for(int column=0;column<4;++column)QVERIFY(!tree->topLevelItem(0)->toolTip(column).isEmpty());
}
int main(int argc,char **argv){qputenv("QT_QPA_PLATFORM","offscreen");qputenv("QT_QPA_FONTDIR",QDir(qEnvironmentVariable("WINDIR","C:/Windows")).filePath("Fonts").toUtf8());QApplication app(argc,argv);WorktreeSettingsTest test;return QTest::qExec(&test,argc,argv);}
#include "test_worktree_settings.moc"
