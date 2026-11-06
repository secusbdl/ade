#include "ui/MainWindow.h"
#include "ui/AppStyle.h"
#include "ui/SettingsPage.h"
#include "ui/Composer.h"
#include "ui/SessionStore.h"
#include "core/ProviderStore.h"
#include "git/WorktreeSettingsController.h"
#include "editor/CodeEditor.h"
#include <QApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileSystemModel>
#include <QPixmap>
#include <QProcess>
#include <QElapsedTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QTreeView>
#include <QToolButton>

// Explicit hidden visual fixture; no credentials or provider requests.
int main(int argc,char** argv){
    QApplication app(argc,argv);ade::initializeAppStyle(app);if(app.arguments().size()!=2)return 2;
    const auto output=QDir(app.arguments()[1]).absolutePath();if(!QDir().mkpath(output))return 3;
    QTemporaryDir profile,workspace,files;const auto code=workspace.filePath("dashboard.cpp");
    QFile source(code);if(!source.open(QIODevice::WriteOnly))return 4;
    source.write("#include <QApplication>\n#include <QWidget>\n\nint main(int argc, char **argv) {\n    QApplication app(argc, argv);\n    QWidget dashboard;\n    dashboard.setWindowTitle(\"Dashboard\");\n    dashboard.resize(1200, 800);\n    dashboard.show();\n    return app.exec();\n}\n");source.close();
    QProcess git;git.setWorkingDirectory(workspace.path());git.start("git",{"init","-b","main"});if(!git.waitForFinished(10000)||git.exitCode()!=0)return 10;
    git.start("git",{"-c","user.name=Visual Fixture","-c","user.email=fixture@example.invalid","add","dashboard.cpp"});if(!git.waitForFinished(10000)||git.exitCode()!=0)return 10;
    git.start("git",{"-c","user.name=Visual Fixture","-c","user.email=fixture@example.invalid","commit","-m","Visual fixture"});if(!git.waitForFinished(10000)||git.exitCode()!=0)return 10;
    QFile attachment(files.filePath("Design notes.txt"));if(!attachment.open(QIODevice::WriteOnly))return 5;attachment.write("Refine spacing and preserve keyboard navigation.");attachment.close();
    ade::ProviderConfig provider;provider.id="deepseek";provider.name="DeepSeek API";provider.model="deepseek-v4-pro";provider.baseUrl="https://api.deepseek.com/v1";
    if(!ade::ProviderStore::save(profile.filePath("providers.json"),{provider}))return 6;
    ade::Conversation session;session.id="visual-fixture";session.title="Build the dashboard";session.providerId=provider.id;session.model=provider.model;session.workspace=workspace.path();
    session.messages={QJsonObject{{"role","user"},{"content","Review the dashboard layout and its keyboard navigation."}},QJsonObject{{"role","assistant"},{"content","The dashboard uses a native Qt window. I can review the layout alongside the workspace files.\n\n- Preserve readable spacing at smaller window sizes.\n- Keep keyboard focus visible.\n- Check every action against the current workspace."}}};
    QList<ade::Conversation> sessions{session};
    for(int index=0;index<6;++index){
        ade::Conversation archived;archived.id=QString("archived-%1").arg(index);archived.archived=true;
        archived.title=QStringList{"Review the dashboard layout","Add keyboard navigation to the settings page","Investigate the failed build","Update the workspace documentation","Compare provider responses","A longer chat title that should remain readable without displacing the row actions"}[index];
        archived.workspace=workspace.path();archived.created=archived.updated=QDateTime::currentMSecsSinceEpoch()-index*86400000;
        sessions.append(archived);
    }
    if(!ade::SessionStore(profile.path()).save(sessions))return 7;
    ade::MainWindow window(profile.path());window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1540,940);
    auto* composer=window.findChild<ade::Composer*>();composer->setPlainText("Use the attached notes to refine the dashboard.");emit composer->filesAttached({attachment.fileName()});
    if(auto* button=window.findChild<QToolButton*>("toggleFileTreeButton"))button->click();
    auto* tree=window.findChild<QTreeView*>("workspaceFiles");if(!tree)return 8;
    auto* model=qobject_cast<QFileSystemModel*>(tree->model());if(!model)return 8;emit tree->doubleClicked(model->index(code));
    window.show();auto* settings=window.findChild<ade::SettingsPage*>();
    for(const auto& theme:{"light","dark"}){settings->setPreference("personalization.appearance",theme);app.processEvents();if(!window.grab().save(QDir(output).filePath(QString("workbench-")+theme+".png")))return 9;}
    auto* settingsButton=window.findChild<QToolButton*>("settingsButton");if(!settingsButton)return 11;settingsButton->click();settings->selectSection("worktree-setup");
    auto* controller=window.findChild<ade::WorktreeSettingsController*>();if(!controller)return 12;
    QElapsedTimer wait;wait.start();while(controller->repositoryIds().isEmpty()&&wait.elapsed()<15000){app.processEvents();QThread::msleep(10);}if(controller->repositoryIds().isEmpty())return 12;
    auto* editor=window.findChild<ade::CodeEditor*>("worktreeSetupEditor");if(!editor)return 13;
    editor->setPlainText("$ErrorActionPreference = 'Stop'\n\n# Prepare this repository's linked workspace.\nWrite-Output $env:STAGEWISE_TARGET_WORKTREE_PATH\npnpm install\n");
    for(const auto& theme:{"light","dark"}){settings->setPreference("personalization.appearance",theme);app.processEvents();if(!editor->font().family().contains("Roboto Mono")||editor->font().pixelSize()!=12)return 14;if(!window.grab().save(QDir(output).filePath(QString("worktree-settings-integrated-")+theme+".png")))return 15;}
    settings->selectSection("archived-agents");
    for(const auto& theme:{"light","dark"}){settings->setPreference("personalization.appearance",theme);app.processEvents();if(!window.grab().save(QDir(output).filePath(QString("archived-chats-")+theme+".png")))return 16;}
    window.resize(960,720);app.processEvents();if(!window.grab().save(QDir(output).filePath("archived-chats-narrow.png")))return 17;
    return 0;
}
