#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextLayout>
#include <QTreeWidget>
#include <QTimer>
#include "git/GitPanel.h"
#include "git/ProcessRunner.h"
#include "editor/CodeEditor.h"
#include "ui/AppFonts.h"
using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size(); }
struct Fixture {
    QTemporaryDir base; QString root,managed,error;
    explicit Fixture(const QString &name="repo") {
        root=base.filePath(name); managed=base.filePath("managed"); QDir().mkpath(root);
        run({"init","-b","main"}); run({"config","user.name","ADE UI fixture"}); run({"config","user.email","fixture@example.invalid"}); run({"config","commit.gpgsign","false"}); run({"config","core.autocrlf","false"}); run({"config","core.hooksPath",base.filePath("hooks")});
        put(root+"/one.txt","base\n"); run({"add","."}); run({"commit","-m","base"});
    }
    bool run(const QStringList &args) {
        QProcess p; auto env=gitdetail::safeEnvironment(); env.insert("GIT_CONFIG_NOSYSTEM","1"); env.insert("GIT_CONFIG_GLOBAL",base.filePath("no-global")); p.setProcessEnvironment(env); p.setWorkingDirectory(root); p.start("git",args);
        if(!p.waitForFinished(15000)||p.exitCode()!=0) { error=QString::fromUtf8(p.readAllStandardError()); return false; } return true;
    }
    GitServiceOptions options() const { GitServiceOptions o; o.managedWorktreeRoot=managed; return o; }
    GitWorkspaceContext context() const { GitWorkspaceContext c; c.trustedRoots={root}; c.usageKnown=true; return c; }
    QString linked(const QString &branch) { const auto path=managed+"/fixture/"+branch; QDir().mkpath(QFileInfo(path).absolutePath()); if(!run({"worktree","add","-b",branch,path,"main"})) return {}; return path; }
};
template<class T> T *child(QWidget &panel,const char *name) { return panel.findChild<T *>(name); }
QTreeWidgetItem *findItem(QTreeWidget *tree,const QString &value) { for(int i=0;i<tree->topLevelItemCount();++i) if(tree->topLevelItem(i)->text(0)==value) return tree->topLevelItem(i); return nullptr; }
void answerCreate(const QString &name) {
    QTimer::singleShot(0,[name] { if(auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget())) { if(auto *edit=dialog->findChild<QLineEdit *>("gitNewName")) edit->setText(name); if(auto *source=dialog->findChild<QComboBox *>("gitSourceBranch")) source->setCurrentIndex(source->findData("main")); if(auto *buttons=dialog->findChild<QDialogButtonBox *>()) buttons->button(QDialogButtonBox::Ok)->click(); } });
}
class DialogAnswer:public QObject {
public:
    QTimer timer; bool saw=false,dirty=false;
    explicit DialogAnswer(bool accept,QObject *parent=nullptr):QObject(parent) {
        timer.setInterval(10); connect(&timer,&QTimer::timeout,this,[this,accept] {
            if(auto *box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                saw=true; dirty=box->text().contains("uncommitted changes"); timer.stop();
                if(accept) { if(auto *button=box->findChild<QPushButton *>("gitConfirmDelete")) button->click(); }
                else box->reject();
            }
        }); timer.start();
    }
};
}
class GitUiTest:public QObject {
    Q_OBJECT
private slots:
    void scopeAndDisabledActions();
    void stagedAndWorkingPreview();
    void lateWorkspaceResponsesIgnored();
    void createMountAndHostGuard();
    void sameWorkspaceChatSwitchDoesNotMountLateResult();
    void mainAndCheckedOutBranchesDisabled();
    void explicitDirtyDeletion();
    void refreshUpdatesSelectedPreview();
    void renderNativePanel();
    void explicitThemeReachesBothPreviews();
};
void GitUiTest::explicitThemeReachesBothPreviews() {
    GitPanel panel;
    for(const auto *name:{"gitOriginal","gitModified"}) {
        auto *editor=child<CodeEditor>(panel,name);QVERIFY(editor);QVERIFY(editor->isReadOnly());
        editor->setStyleSheet("background:transparent");auto colors=editor->palette();colors.setColor(QPalette::Base,Qt::transparent);editor->setPalette(colors);
        editor->setFileName("theme.cpp");editor->setPlainText("const int value = 12;");
    }
    for(const bool dark:{false,true,false}) {
        panel.setDarkTheme(dark);
        for(const auto *name:{"gitOriginal","gitModified"}) {
            auto *editor=child<CodeEditor>(panel,name);const auto formats=editor->document()->firstBlock().layout()->formats();QVERIFY(!formats.isEmpty());
            QCOMPARE(formats.first().format.foreground().color(),QColor(dark?"#76e2c6":"#e14238"));
            QCOMPARE(editor->sourceText(),QString("const int value = 12;"));
        }
    }
}
void GitUiTest::scopeAndDisabledActions() {
    Fixture f; QVERIFY2(f.error.isEmpty(),qPrintable(f.error)); GitPanel panel(f.options());
    QVERIFY(!child<QPushButton>(panel,"gitCreateWorktree")->isEnabled()); panel.setWorkspace(f.root);
    QTRY_VERIFY_WITH_TIMEOUT(!child<QLabel>(panel,"gitMessage")->text().isEmpty(),10000);
    QVERIFY(!child<QPushButton>(panel,"gitCreateWorktree")->isEnabled());
    panel.setWorkspaceContext(f.context()); panel.refresh(); QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(panel,"gitCreateWorktree")->isEnabled(),15000);
    panel.setWorkspace({}); QVERIFY(!child<QPushButton>(panel,"gitCreateWorktree")->isEnabled()); QVERIFY(panel.workspace().isEmpty());
}
void GitUiTest::stagedAndWorkingPreview() {
    Fixture f; QVERIFY(put(f.root+"/one.txt","staged\n")); QVERIFY(f.run({"add","one.txt"})); QVERIFY(put(f.root+"/one.txt","working\r\n")); QVERIFY(put(f.root+"/binary.bin",QByteArray("a\0b",3)));
    GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root); auto *tree=child<QTreeWidget>(panel,"gitChanges");
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),2,15000); tree->setCurrentItem(findItem(tree,"one.txt"));
    auto *original=child<CodeEditor>(panel,"gitOriginal"),*modified=child<CodeEditor>(panel,"gitModified");
    QTRY_COMPARE_WITH_TIMEOUT(modified->toPlainText(),QString("staged\n"),15000); QCOMPARE(original->toPlainText(),"base\n"); QVERIFY(original->isReadOnly()&&modified->isReadOnly()); QVERIFY(!child<QPushButton>(panel,"gitOpenWorkingFile")->isEnabled());
    child<QComboBox>(panel,"gitDiffVersion")->setCurrentIndex(0); QTRY_COMPARE_WITH_TIMEOUT(modified->toPlainText(),QString("working\n"),15000); QVERIFY(child<QPushButton>(panel,"gitOpenWorkingFile")->isEnabled());
    QSignalSpy open(&panel,&GitPanel::openDiffRequested); child<QPushButton>(panel,"gitOpenWorkingFile")->click(); QCOMPARE(open.size(),1); QCOMPARE(open[0][1].toString(),"one.txt"); QVERIFY(!open[0][2].toBool());
    tree->setCurrentItem(findItem(tree,"binary.bin")); QTRY_VERIFY_WITH_TIMEOUT(child<QLabel>(panel,"gitMessage")->text().contains("cannot be displayed"),15000); QVERIFY(!child<QPushButton>(panel,"gitOpenWorkingFile")->isEnabled());
}
void GitUiTest::lateWorkspaceResponsesIgnored() {
    Fixture first("first"),second("second"); QVERIFY(put(first.root+"/first.txt","first")); QVERIFY(put(second.root+"/second.txt","second"));
    GitPanel panel(first.options()); auto context=first.context(); context.trustedRoots<<second.root; panel.setWorkspaceContext(context);
    panel.setWorkspace(first.root); panel.setWorkspace(second.root);
    auto *tree=child<QTreeWidget>(panel,"gitChanges"); QTRY_VERIFY_WITH_TIMEOUT(findItem(tree,"second.txt"),15000); QVERIFY(!findItem(tree,"first.txt")); QVERIFY(child<QLabel>(panel,"gitHeading")->text().startsWith("second"));
    tree->setCurrentItem(findItem(tree,"second.txt")); panel.setWorkspace(first.root); QTRY_VERIFY_WITH_TIMEOUT(findItem(tree,"first.txt"),15000); QVERIFY(!findItem(tree,"second.txt"));
}
void GitUiTest::createMountAndHostGuard() {
    Fixture f; GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root); auto *create=child<QPushButton>(panel,"gitCreateWorktree");
    QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000); QSignalSpy mount(&panel,&GitPanel::mountWorktreeRequested);
    answerCreate("ui-created"); create->click(); QTRY_COMPARE_WITH_TIMEOUT(mount.size(),1,20000); QVERIFY(!panel.isMutating());
    const auto path=mount[0][0].toString(); QVERIFY(QFileInfo::exists(path+"/.git")); const auto metadata=qvariant_cast<WorktreeSetupMetadata>(mount[0][1]); QCOMPARE(metadata.workspacePath,path); QCOMPARE(metadata.sourceWorktreePath,GitService::canonicalPath(f.root));
    QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000); int guardCalls=0; panel.setMutationGuard([&](const QString &,GitOperation){ ++guardCalls; return false; });
    answerCreate("must-not-create"); create->click(); QCOMPARE(guardCalls,1); QCOMPARE(mount.size(),1); QVERIFY(!panel.isMutating()); QVERIFY(child<QLabel>(panel,"gitMessage")->text().contains("cancelled"));
}
void GitUiTest::sameWorkspaceChatSwitchDoesNotMountLateResult() {
    Fixture f; GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root,"first-chat");
    auto *create=child<QPushButton>(panel,"gitCreateWorktree"); QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000);
    QSignalSpy mount(&panel,&GitPanel::mountWorktreeRequested),finished(&panel,&GitPanel::mutationFinished);
    answerCreate("late-created"); create->click(); QVERIFY(panel.isMutating()); panel.setWorkspace(f.root,"second-chat");
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000); QVERIFY(finished[0][2].toBool()); QCOMPARE(mount.size(),0);
    QTRY_VERIFY_WITH_TIMEOUT(create->isEnabled(),15000); QTRY_VERIFY_WITH_TIMEOUT(findItem(child<QTreeWidget>(panel,"gitWorktrees"),"late-created"),15000);
}
void GitUiTest::mainAndCheckedOutBranchesDisabled() {
    Fixture f; const auto linked=f.linked("busy"); QVERIFY(!linked.isEmpty()); GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root);
    auto *trees=child<QTreeWidget>(panel,"gitWorktrees"); QTRY_COMPARE_WITH_TIMEOUT(trees->topLevelItemCount(),2,15000); trees->setCurrentItem(findItem(trees,"repo")); QVERIFY(!child<QPushButton>(panel,"gitDeleteWorktree")->isEnabled());
    trees->setCurrentItem(findItem(trees,"busy")); QVERIFY(child<QPushButton>(panel,"gitDeleteWorktree")->isEnabled());
    auto *switchButton=child<QPushButton>(panel,"gitSwitchBranch"); QTRY_VERIFY_WITH_TIMEOUT(switchButton->isEnabled(),15000); bool disabled=false;
    QTimer::singleShot(0,[&] { auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget()); if(!dialog) return; auto *choices=dialog->findChild<QComboBox *>("gitCheckoutBranch"); if(choices) { const int index=choices->findData("busy"); auto *model=qobject_cast<QStandardItemModel *>(choices->model()); disabled=index>=0&&model&&!model->item(index)->isEnabled(); } dialog->reject(); });
    switchButton->click(); QVERIFY(disabled);
}
void GitUiTest::explicitDirtyDeletion() {
    Fixture f; const auto path=f.linked("dirty"); QVERIFY(!path.isEmpty()); QVERIFY(put(path+"/precious.txt","unsaved")); GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root);
    auto *trees=child<QTreeWidget>(panel,"gitWorktrees"); QTRY_COMPARE_WITH_TIMEOUT(trees->topLevelItemCount(),2,15000); trees->setCurrentItem(findItem(trees,"dirty"));
    { DialogAnswer reject(false); child<QPushButton>(panel,"gitDeleteWorktree")->click(); QTRY_VERIFY_WITH_TIMEOUT(reject.saw,15000); QVERIFY(reject.dirty); QVERIFY(QFileInfo::exists(path+"/precious.txt")); }
    QSignalSpy started(&panel,&GitPanel::worktreeRemovalStarted),finished(&panel,&GitPanel::worktreeRemovalFinished),removed(&panel,&GitPanel::worktreeRemoved);
    { DialogAnswer accept(true); QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(panel,"gitDeleteWorktree")->isEnabled(),10000); child<QPushButton>(panel,"gitDeleteWorktree")->click(); QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,20000); QVERIFY(accept.dirty); }
    QCOMPARE(started.size(),1); QCOMPARE(removed.size(),1); QVERIFY(finished[0][1].toBool()); QVERIFY(!QFileInfo::exists(path));
}
void GitUiTest::refreshUpdatesSelectedPreview() {
    Fixture f; QVERIFY(put(f.root+"/one.txt","first local edit\n")); GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root);
    auto *tree=child<QTreeWidget>(panel,"gitChanges"); QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),1,15000); tree->setCurrentItem(tree->topLevelItem(0)); auto *modified=child<CodeEditor>(panel,"gitModified"); QTRY_COMPARE_WITH_TIMEOUT(modified->toPlainText(),QString("first local edit\n"),15000);
    QVERIFY(put(f.root+"/one.txt","second external edit\n")); panel.refresh(); QTRY_COMPARE_WITH_TIMEOUT(modified->toPlainText(),QString("second external edit\n"),15000);
    QVERIFY(put(f.root+"/one.txt","base\n")); panel.refresh(); QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),0,15000); QVERIFY(!child<QPushButton>(panel,"gitOpenWorkingFile")->isEnabled());
}
void GitUiTest::renderNativePanel() {
    const auto output=qEnvironmentVariable("ADE_GIT_RENDER_DIR"); if(output.isEmpty()) return;
    Fixture f("native-workspace"); QVERIFY(put(f.root+"/one.txt","Updated working content\nSecond line\n")); QVERIFY(put(f.root+"/new-component.cpp","#include <QWidget>\n// Native component\n"));
    GitPanel panel(f.options()); panel.setWorkspaceContext(f.context()); panel.setWorkspace(f.root); panel.resize(1100,740);
    auto *tree=child<QTreeWidget>(panel,"gitChanges"); QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(),2,15000); tree->setCurrentItem(findItem(tree,"one.txt")); QTRY_VERIFY_WITH_TIMEOUT(child<CodeEditor>(panel,"gitModified")->toPlainText().contains("Updated"),15000);
    QTRY_VERIFY_WITH_TIMEOUT(child<QPushButton>(panel,"gitCreateWorktree")->isEnabled(),15000); panel.ensurePolished(); panel.layout()->activate();
    QPixmap pixmap(panel.size()); panel.render(&pixmap); QVERIFY(pixmap.save(output+"/git-panel-light.png"));
    auto palette=panel.palette(); palette.setColor(QPalette::Window,QColor("#17191d")); palette.setColor(QPalette::Base,QColor("#17191d")); palette.setColor(QPalette::AlternateBase,QColor("#1e2126")); palette.setColor(QPalette::Text,QColor("#e4e6eb")); palette.setColor(QPalette::PlaceholderText,QColor("#989ca5")); palette.setColor(QPalette::WindowText,QColor("#e4e6eb")); palette.setColor(QPalette::Button,QColor("#24272e")); palette.setColor(QPalette::ButtonText,QColor("#e4e6eb")); panel.setPalette(palette); panel.setAutoFillBackground(true); panel.render(&pixmap); QVERIFY(pixmap.save(output+"/git-panel-dark.png"));
}
int main(int argc,char **argv) {
    qputenv("QT_QPA_PLATFORM","offscreen"); qputenv("QT_QPA_FONTDIR",QDir(qEnvironmentVariable("WINDIR","C:/Windows")).filePath("Fonts").toUtf8()); QApplication app(argc,argv); const auto fonts=AppFonts::load(); QFont font(fonts.body); font.setPixelSize(13); app.setFont(font); qRegisterMetaType<WorktreeSetupMetadata>();
    GitUiTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_git_ui.moc"
