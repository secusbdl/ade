#include <QtTest>
#include <QLabel>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileSystemModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QTabWidget>
#include "ui/MainWindow.h"
#include "ui/AppFonts.h"
#include "ui/Composer.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "editor/EditorWidget.h"
#include "editor/CodeEditor.h"
#include "terminal/TerminalWidget.h"
#include "git/GitPanel.h"
#include "git/ProcessRunner.h"
#include "git/WorktreeSetupRunner.h"
#include "git/WorktreeSettingsController.h"
#include "git/WorktreeSettingsPane.h"
using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path); return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(); }
QByteArray read(const QString &path) { QFile file(path); return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }
struct Fixture {
    QTemporaryDir base; QString root,data,error; QStringList junctions;
    Fixture() {
        root=base.filePath("repository"); data=base.filePath("app-data"); QDir().mkpath(root); QDir().mkpath(data);
        run({"init","-b","main"}); run({"config","user.name","ADE host fixture"}); run({"config","user.email","fixture@example.invalid"}); run({"config","commit.gpgsign","false"}); run({"config","core.autocrlf","false"}); run({"config","core.hooksPath",base.filePath("hooks")});
        put(root+"/one.txt","base\n"); run({"add","."}); run({"commit","-m","base"});
    }
    ~Fixture() { for(const auto &path:junctions) QDir().rmdir(path); }
    bool run(const QStringList &args) {
        auto environment=gitdetail::safeEnvironment(); environment.insert("GIT_CONFIG_NOSYSTEM","1"); environment.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global-config"));
        QProcess process; process.setWorkingDirectory(root); process.setProcessEnvironment(environment); process.start("git",args);
        if(!process.waitForFinished(15000)||process.exitCode()!=0) { error=QString::fromUtf8(process.readAllStandardError()); return false; } return true;
    }
    QString linked(const QString &branch="linked") {
        const auto path=data+"/worktrees/fixture/"+branch; QDir().mkpath(QFileInfo(path).absolutePath()); return run({"worktree","add","-b",branch,path,"main"})?path:QString();
    }
    QString junction(const QString &target,const QString &name="alias") {
        const auto path=base.filePath(name); QProcess process; process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(target)});
        if(!process.waitForFinished(5000)||process.exitCode()!=0) { error=QString::fromUtf8(process.readAllStandardError()); return {}; }
        junctions.append(path); return path;
    }
    Conversation chat(const QString &id,const QString &workspace) const {
        Conversation conversation; conversation.id=id; conversation.title=id; conversation.workspace=workspace; conversation.titleLocked=true;
        conversation.created=conversation.updated=QDateTime::currentMSecsSinceEpoch(); conversation.providerId="fixture"; conversation.model="fixture-model"; return conversation;
    }
    bool seed(const QList<Conversation> &chats) const { return SessionStore(data).save(chats); }
    bool provider(quint16 port) const {
        ProviderConfig p; p.id="fixture"; p.name="Fixture"; p.baseUrl=QString("http://127.0.0.1:%1/v1").arg(port); p.apiKey="dummy-not-a-real-credential"; p.model="fixture-model"; p.protocol="openai";
        return ProviderStore::save(data+"/providers.json",{p});
    }
};
Conversation stored(const Fixture &fixture,const QString &id) { for(const auto &chat:SessionStore(fixture.data).load()) if(chat.id==id) return chat; return {}; }
void selectChat(MainWindow &window,const QString &id) { auto *list=window.findChild<QListWidget *>("agentList"); if(!list) return; for(int i=0;i<list->count();++i) if(list->item(i)->data(Qt::UserRole).toString()==id) { list->setCurrentRow(i); return; } }
QTreeView *openFiles(MainWindow &window) {
    if(auto* button=window.findChild<QToolButton*>("toggleFileTreeButton"))button->click();
    return window.findChild<QTreeView *>("workspaceFiles");
}
GitResult waitFor(GitService *service,quint64 id,int timeout=20000) {
    QSignalSpy spy(service,&GitService::completed); QElapsedTimer timer; timer.start();
    while(timer.elapsed()<timeout) { for(const auto &row:spy) if(row[0].toULongLong()==id) return qvariant_cast<GitResult>(row[1]); QCoreApplication::processEvents(); QTest::qWait(5); }
    GitResult result; result.error.message="Host test wait timed out."; return result;
}
class Dialogs:public QObject {
public:
    QTimer timer; QString createName; bool acceptDeletion=false, cancelEditor=false, sawDelete=false, sawEditor=false;
    explicit Dialogs(QObject *parent=nullptr):QObject(parent) {
        timer.setInterval(10); connect(&timer,&QTimer::timeout,this,[this] {
            auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget()); if(!dialog) return;
            if(dialog->objectName()=="gitCreateDialog"&&!createName.isEmpty()) {
                dialog->findChild<QLineEdit *>("gitNewName")->setText(createName);
                auto *source=dialog->findChild<QComboBox *>("gitSourceBranch"); source->setCurrentIndex(source->findData("main")); createName.clear();
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            } else if(auto *box=qobject_cast<QMessageBox *>(dialog)) {
                if(box->objectName()=="gitDeleteConfirmation") { sawDelete=true; if(acceptDeletion) box->findChild<QPushButton *>("gitConfirmDelete")->click(); else box->reject(); }
                else if(box->windowTitle()=="Unsaved changes"&&cancelEditor) { sawEditor=true; box->button(QMessageBox::Cancel)->click(); }
            }
        }); timer.start();
    }
};
QTreeWidgetItem *treeItem(QTreeWidget *tree,const QString &name) { for(int i=0;i<tree->topLevelItemCount();++i) if(tree->topLevelItem(i)->text(0)==name) return tree->topLevelItem(i); return nullptr; }
}
class GitHostTest:public QObject {
    Q_OBJECT
private slots:
    void connectedJunctionIsStoredCanonically();
    void removedLegacyJunctionDetachesAllAffectedChats();
    void cleanupRequiresIndependentUsageHistory();
    void noWorkspaceChatClearsFileTree();
    void unopenedFilesViewIsDestroyedWithWindow();
    void inFlightMutationBlocksSendAndReconnect();
    void setupStartsOnlyAfterSuccessfulMountAndBlocksWork();
    void failedMountDoesNotExecuteSetup();
    void dirtyAliasEditorCanVetoBranchMutation();
    void otherChatBusyAgentCanVetoBranchMutation();
    void activeTerminalCanVetoWorktreeRemoval();
    void repositorySettingsPreserveDraftsAndUnrelatedEditors();
    void settingsRemovalUsesTheTerminalGuard();
    void externalTopologyRefreshesSettingsAndPreservesDraft();
};
void GitHostTest::connectedJunctionIsStoredCanonically() {
    Fixture f; const auto alias=f.junction(f.root); QVERIFY2(!alias.isEmpty(),qPrintable(f.error)); QVERIFY(f.seed({f.chat("chat",{})}));
    MainWindow window(f.data); window.connectWorkspace(alias);
    QCOMPARE(stored(f,"chat").workspace,GitService::canonicalPath(f.root));
}
void GitHostTest::removedLegacyJunctionDetachesAllAffectedChats() {
    Fixture f; const auto linked=f.linked(); QVERIFY(!linked.isEmpty()); const auto alias=f.junction(linked); QVERIFY(!alias.isEmpty());
    auto first=f.chat("alias-chat",alias),second=f.chat("canonical-chat",linked),archived=f.chat("archived-alias",alias),other=f.chat("main-chat",f.root); archived.archived=true;
    first.messages=QJsonArray{QJsonObject{{"role","user"},{"content","Keep this conversation"}}};
    QVERIFY(f.seed({first,second,archived,other})); MainWindow window(f.data); auto *panel=window.findChild<GitPanel *>(); QVERIFY(panel); auto *files=openFiles(window); QVERIFY(files);
    window.findChild<QToolButton *>("sourceControlButton")->click(); auto *trees=panel->findChild<QTreeWidget *>("gitWorktrees");
    QTRY_VERIFY_WITH_TIMEOUT(treeItem(trees,"linked"),15000); trees->setCurrentItem(treeItem(trees,"linked"));
    Dialogs dialogs; dialogs.acceptDeletion=true; QSignalSpy done(panel,&GitPanel::worktreeRemovalFinished);
    panel->findChild<QPushButton *>("gitDeleteWorktree")->click(); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,20000); QVERIFY(done[0][1].toBool()); QVERIFY(dialogs.sawDelete);
    QVERIFY(stored(f,"alias-chat").workspace.isEmpty()); QVERIFY(stored(f,"canonical-chat").workspace.isEmpty()); QVERIFY(stored(f,"archived-alias").workspace.isEmpty());
    QCOMPARE(stored(f,"main-chat").workspace,f.root); QCOMPARE(stored(f,"alias-chat").messages,first.messages); QCOMPARE(SessionStore(f.data).load().size(),4);
    QVERIFY(!files->model());
    QCOMPARE(window.findChild<QLabel*>("fileTreeTitle")->text(),QString("Files"));
    QVERIFY(files->isHidden());
    QVERIFY(!window.findChild<QWidget*>("fileTreeEmpty")->isHidden());
}
void GitHostTest::cleanupRequiresIndependentUsageHistory() {
    Fixture f; const auto linked=f.linked(); QVERIFY(!linked.isEmpty()); QVERIFY(f.seed({f.chat("chat",linked)})); MainWindow window(f.data);
    window.connectWorkspace(f.root); auto *panel=window.findChild<GitPanel *>(); auto result=waitFor(panel->service(),panel->service()->cleanupCandidates());
    QVERIFY2(result.ok,qPrintable(result.error.message)); QVERIFY2(result.cleanup.isEmpty(),"A just-used worktree cannot become an old cleanup candidate merely because its chat changed workspace.");
}
void GitHostTest::noWorkspaceChatClearsFileTree() {
    Fixture f; QVERIFY(f.seed({f.chat("connected",f.root),f.chat("empty",{})})); MainWindow window(f.data); auto *files=openFiles(window); QVERIFY(files); QVERIFY(files->model());
    selectChat(window,"empty"); QCOMPARE(window.currentSessionId(),"empty"); QVERIFY2(!files->model(),"No-workspace chat must not expose the previous repository or the Computer root.");
    selectChat(window,"connected"); QCOMPARE(window.currentSessionId(),"connected"); QVERIFY(files->model()); QCOMPARE(qobject_cast<QFileSystemModel *>(files->model())->filePath(files->rootIndex()),f.root);
}
void GitHostTest::unopenedFilesViewIsDestroyedWithWindow() {
    Fixture f; QVERIFY(f.seed({f.chat("chat",f.root)}));
    const auto before=QApplication::allWidgets(); QPointer<QTreeView> files;
    {
        MainWindow window(f.data);
        for(auto *widget:QApplication::allWidgets()) if(!before.contains(widget)&&widget->objectName()=="workspaceFiles") files=qobject_cast<QTreeView *>(widget);
        QVERIFY(files);
    }
    const bool destroyed=files.isNull();
    if(files) delete files;
    QVERIFY2(destroyed,"The unopened Files view must have a lifetime owned by its MainWindow.");
}
void GitHostTest::inFlightMutationBlocksSendAndReconnect() {
    Fixture f; QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QVERIFY(f.provider(server.serverPort())); QVERIFY(f.seed({f.chat("chat",f.root)}));
    MainWindow window(f.data); auto *panel=window.findChild<GitPanel *>(); QSignalSpy requests(&server,&QTcpServer::newConnection);
    panel->mutationStarted(f.root,GitOperation::SwitchBranch);
    window.submitPrompt("Keep this draft while Git runs"); const auto other=f.base.filePath("other"); QDir().mkpath(other); window.connectWorkspace(other); QTest::qWait(40);
    QCOMPARE(requests.size(),0); QCOMPARE(stored(f,"chat").workspace,f.root); QCOMPARE(window.findChild<Composer *>("composerInput")->toPlainText(),"Keep this draft while Git runs");
    panel->mutationFinished(f.root,GitOperation::SwitchBranch,false); window.connectWorkspace(other); QCOMPARE(stored(f,"chat").workspace,GitService::canonicalPath(other));
}
void GitHostTest::setupStartsOnlyAfterSuccessfulMountAndBlocksWork() {
    Fixture f; const auto linked=f.linked(); QVERIFY(!linked.isEmpty()); QVERIFY(put(f.root+"/.stagewise/worktree-setup.ps1","[IO.File]::WriteAllText((Join-Path $env:STAGEWISE_TARGET_WORKTREE_PATH 'setup-marker.txt'), $env:STAGEWISE_SOURCE_WORKTREE_PATH); Start-Sleep -Seconds 10"));
    QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QVERIFY(f.provider(server.serverPort())); QVERIFY(f.seed({f.chat("chat",f.root)})); MainWindow window(f.data);
    auto *panel=window.findChild<GitPanel *>(); auto *setup=window.findChild<WorktreeSetupRunner *>(); QVERIFY(setup); QSignalSpy updates(setup,&WorktreeSetupRunner::updated),requests(&server,&QTcpServer::newConnection);
    panel->mountWorktreeRequested(linked,{linked,f.root,f.root}); QCOMPARE(stored(f,"chat").workspace,GitService::canonicalPath(linked));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(linked+"/setup-marker.txt"),10000); QCOMPARE(read(linked+"/setup-marker.txt"),GitService::canonicalPath(f.root).toUtf8());
    window.submitPrompt("Wait for setup"); window.connectWorkspace(f.root); QTest::qWait(40); QCOMPARE(requests.size(),0); QCOMPARE(stored(f,"chat").workspace,GitService::canonicalPath(linked));
    setup->cancelAll(); QTRY_VERIFY_WITH_TIMEOUT(!updates.isEmpty()&&qvariant_cast<WorktreeSetupRun>(updates.last()[0]).status==WorktreeSetupRun::Status::Failed,10000);
    window.connectWorkspace(f.root); QCOMPARE(stored(f,"chat").workspace,GitService::canonicalPath(f.root));
}
void GitHostTest::failedMountDoesNotExecuteSetup() {
    Fixture f; const auto linked=f.linked(); QVERIFY(!linked.isEmpty()); QVERIFY(put(f.root+"/.stagewise/worktree-setup.ps1","[IO.File]::WriteAllText((Join-Path $env:STAGEWISE_TARGET_WORKTREE_PATH 'must-not-run.txt'),'no')"));
    QVERIFY(f.seed({f.chat("chat",f.root)})); MainWindow window(f.data); auto *setup=window.findChild<WorktreeSetupRunner *>(); QSignalSpy updates(setup,&WorktreeSetupRunner::updated);
    const QByteArray broken="{external-corruption"; QVERIFY(put(f.data+"/sessions.json",broken));
    window.findChild<GitPanel *>()->mountWorktreeRequested(linked,{linked,f.root,f.root}); QTest::qWait(350);
    QCOMPARE(updates.size(),0); QVERIFY(!QFileInfo::exists(linked+"/must-not-run.txt")); QCOMPARE(read(f.data+"/sessions.json"),broken); QCOMPARE(window.findChild<GitPanel *>()->workspace(),GitService::canonicalPath(f.root));
}
void GitHostTest::dirtyAliasEditorCanVetoBranchMutation() {
    Fixture f; const auto alias=f.junction(f.root); QVERIFY(!alias.isEmpty()); QVERIFY(f.seed({f.chat("chat",f.root)})); MainWindow window(f.data); auto *panel=window.findChild<GitPanel *>();
    auto *editor=new EditorWidget; QString error; QVERIFY2(editor->openFile(alias+"/one.txt",&error),qPrintable(error)); window.findChild<QTabWidget *>("contentTabs")->addTab(editor,"one.txt"); editor->editor()->setPlainText("Unsaved alias edit"); QVERIFY(editor->isModified());
    window.findChild<QToolButton *>("sourceControlButton")->click(); auto *create=panel->findChild<QPushButton *>("gitCreateBranch"); QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000);
    QSignalSpy started(panel,&GitPanel::mutationStarted); Dialogs dialogs; dialogs.createName="must-not-change"; dialogs.cancelEditor=true; create->click();
    QVERIFY(dialogs.sawEditor); QCOMPARE(started.size(),0); QVERIFY(editor->isModified()); QCOMPARE(editor->editor()->toPlainText(),"Unsaved alias edit"); QCOMPARE(read(f.root+"/one.txt"),"base\n");
}
void GitHostTest::otherChatBusyAgentCanVetoBranchMutation() {
    Fixture f; QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost)); QVERIFY(f.provider(server.serverPort())); QVERIFY(f.seed({f.chat("active-agent",f.root),f.chat("other-chat",f.root)})); MainWindow window(f.data);
    window.submitPrompt("Hold this request until cancelled"); QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(),10000);
    auto *socket=server.nextPendingConnection(); QVERIFY(socket); socket->setParent(&server);
    auto *engine=window.findChild<AgentEngine *>(); QVERIFY(engine&&engine->isBusy());
    selectChat(window,"other-chat"); QCOMPARE(window.currentSessionId(),"other-chat"); window.findChild<QToolButton *>("sourceControlButton")->click(); auto *panel=window.findChild<GitPanel *>(); auto *create=panel->findChild<QPushButton *>("gitCreateBranch"); QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000);
    QSignalSpy started(panel,&GitPanel::mutationStarted); Dialogs dialogs; dialogs.createName="must-not-run"; create->click(); QCOMPARE(started.size(),0); QVERIFY(engine->isBusy()); engine->cancel();
}
void GitHostTest::activeTerminalCanVetoWorktreeRemoval() {
    Fixture f; const auto path=f.linked(); QVERIFY(!path.isEmpty()); QVERIFY(f.seed({f.chat("chat",path)})); MainWindow window(f.data); auto *panel=window.findChild<GitPanel *>();
    auto *terminal=new TerminalWidget(&window); QString error; QVERIFY2(terminal->start(path,&error,{"powershell.exe","-NoProfile","-NonInteractive","-Command","Start-Sleep -Seconds 15"}),qPrintable(error));
    window.findChild<QToolButton *>("sourceControlButton")->click(); auto *trees=panel->findChild<QTreeWidget *>("gitWorktrees"); QTRY_VERIFY_WITH_TIMEOUT(treeItem(trees,"linked"),15000); trees->setCurrentItem(treeItem(trees,"linked"));
    QSignalSpy started(panel,&GitPanel::mutationStarted); Dialogs dialogs; dialogs.acceptDeletion=true; panel->findChild<QPushButton *>("gitDeleteWorktree")->click(); QTRY_VERIFY_WITH_TIMEOUT(dialogs.sawDelete,10000);
    QTRY_VERIFY_WITH_TIMEOUT(panel->findChild<QLabel *>("gitMessage")->text().contains("cancelled"),10000); QCOMPARE(started.size(),0); QVERIFY(QFileInfo::exists(path)); terminal->stop();
}
void GitHostTest::repositorySettingsPreserveDraftsAndUnrelatedEditors(){
    Fixture f;QVERIFY(f.seed({f.chat("chat",f.root)}));MainWindow window(f.data);
    auto* settings=window.findChild<SettingsPage*>();auto* controller=window.findChild<WorktreeSettingsController*>();QVERIFY(settings);QVERIFY(controller);
    QCOMPARE(window.findChildren<GitService*>().size(),1);settings->selectSection("worktree-setup");
    QTRY_COMPARE_WITH_TIMEOUT(controller->repositoryIds().size(),1,15000);
    const auto repo=controller->selectedRepository();QVERIFY(!repo.isEmpty());QCOMPARE(controller->selectedVariant(),GitSetupVariant::PowerShell);
    auto* pane=window.findChild<WorktreeSettingsPane*>();QVERIFY(pane);QPointer<CodeEditor> firstEditor=pane->findChild<CodeEditor*>("worktreeSetupEditor");QVERIFY(firstEditor);
    const QString script="# NATIVE_REPOSITORY_SETUP_52471\nWrite-Output 'fixture'\n";firstEditor->setPlainText(script);QVERIFY(controller->hasUnsavedDrafts());
    settings->selectSection("plugins");QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!firstEditor);QVERIFY(controller->hasUnsavedDrafts());
    settings->selectSection("worktree-setup");pane=window.findChild<WorktreeSettingsPane*>();QVERIFY(pane);QCOMPARE(pane->findChild<CodeEditor*>("worktreeSetupEditor")->sourceText(),script);
    auto* unrelated=new EditorWidget;QString error;QVERIFY2(unrelated->openFile(f.root+"/one.txt",&error),qPrintable(error));
    window.findChild<QTabWidget*>("contentTabs")->addTab(unrelated,"one.txt");unrelated->editor()->setPlainText("Retain this unsaved source edit");
    QPointer<EditorWidget> retained=unrelated;Dialogs dialogs;dialogs.cancelEditor=true;
    QSignalSpy saved(controller,&WorktreeSettingsController::mutationFinished);auto* save=pane->findChild<QPushButton*>("saveWorktreeScript");QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),15000);save->click();
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(),1,15000);QVERIFY(saved.first()[2].toBool());QVERIFY(!controller->hasUnsavedDrafts());
    QCOMPARE(read(f.root+"/.stagewise/worktree-setup.ps1"),script.toUtf8());QVERIFY(retained);QVERIFY(unrelated->isModified());QVERIFY(!dialogs.sawEditor);
    QCOMPARE(unrelated->editor()->toPlainText(),"Retain this unsaved source edit");QCOMPARE(read(f.root+"/one.txt"),"base\n");
    const auto copied=SessionStore(f.data).load();QCOMPARE(copied.first().workspace,GitService::canonicalPath(f.root));
}
void GitHostTest::externalTopologyRefreshesSettingsAndPreservesDraft(){
    Fixture f;QVERIFY(f.seed({f.chat("chat",f.root)}));MainWindow window(f.data);
    auto* service=window.findChild<GitService*>();auto* settings=window.findChild<SettingsPage*>();auto* controller=window.findChild<WorktreeSettingsController*>();
    QVERIFY(service);QVERIFY(settings);QVERIFY(controller);QSignalSpy topology(service,&GitService::repositoryTopologyChanged),coverage(service,&GitService::repositoryWatchCoverageChanged);
    settings->selectSection("worktree-setup");QTRY_COMPARE_WITH_TIMEOUT(controller->repositoryIds().size(),1,15000);
    QTRY_VERIFY_WITH_TIMEOUT(!coverage.isEmpty(),15000);const auto repository=controller->selectedRepository();
    const QString draft="Write-Output 'PRESERVE_DRAFT_AFTER_EXTERNAL_WORKTREE'\n";controller->setDraft(repository,GitSetupVariant::PowerShell,draft);
    QCOMPARE(controller->repository(repository).worktrees.size(),0);const auto linked=f.linked("external-created");QVERIFY2(!linked.isEmpty(),qPrintable(f.error));
    QTRY_VERIFY_WITH_TIMEOUT(!topology.isEmpty(),15000);QTRY_COMPARE_WITH_TIMEOUT(controller->repository(repository).worktrees.size(),1,15000);
    QCOMPARE(GitService::pathKey(controller->repository(repository).worktrees.first().worktree.path),GitService::pathKey(linked));
    QCOMPARE(controller->draft(repository,GitSetupVariant::PowerShell).text,draft);QVERIFY(controller->hasUnsavedDrafts());
}
void GitHostTest::settingsRemovalUsesTheTerminalGuard(){
    Fixture f;const auto path=f.linked();QVERIFY(!path.isEmpty());QVERIFY(f.seed({f.chat("chat",f.root)}));MainWindow window(f.data);
    auto* terminal=new TerminalWidget(&window);QString error;QVERIFY2(terminal->start(path,&error,{"powershell.exe","-NoProfile","-NonInteractive","-Command","Start-Sleep -Seconds 15"}),qPrintable(error));
    auto* settings=window.findChild<SettingsPage*>();auto* controller=window.findChild<WorktreeSettingsController*>();settings->selectSection("worktree-setup");
    QTRY_COMPARE_WITH_TIMEOUT(controller->repositoryIds().size(),1,15000);const auto repo=controller->selectedRepository();
    QTRY_VERIFY_WITH_TIMEOUT(controller->canRemove(repo,path),15000);
    QSignalSpy started(controller,&WorktreeSettingsController::mutationStarted);
    QCOMPARE(controller->remove(repo,path),quint64(0));QCOMPARE(started.size(),0);QVERIFY(QFileInfo::exists(path));terminal->stop();
}
int main(int argc,char **argv) {
    qputenv("QT_QPA_PLATFORM","offscreen"); qputenv("QT_QPA_FONTDIR",QDir(qEnvironmentVariable("WINDIR","C:/Windows")).filePath("Fonts").toUtf8()); QApplication app(argc,argv); const auto fonts=AppFonts::load(); QFont font(fonts.body); font.setPixelSize(13); app.setFont(font);
    GitHostTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_git_host.moc"
