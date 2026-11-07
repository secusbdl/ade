#include "ui/AppStyle.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include "ui/WorkspaceFileTreeModel.h"
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>
using namespace ade;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    initializeAppStyle(app);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::currentPath();
    QDir().mkpath(out);
    for (const QString theme : {"light", "dark"})
        for (const QString state :
             {"empty", "history", "files", "content", "compact", "content-compact", "files-compact"}) {
            QTemporaryDir data, workspace;
            Conversation chat;
            chat.id = "fixture";
            chat.title = "Workbench layout";
            chat.workspace = workspace.filePath("layout-fixture");
            QDir().mkpath(chat.workspace);
            QFile file(chat.workspace + "/probe.txt");
            if (!file.open(QIODevice::WriteOnly))
                return 2;
            file.write("Native workbench fixture.\n");
            file.close();
            if (state != "empty")
                chat.messages = {
                    QJsonObject{{"role", "user"}, {"content", "Read probe.txt and summarize the result."}},
                    QJsonObject{
                        {"role", "assistant"},
                        {"content", "The fixture is ready.\n\n```cpp\nint main() { return 0; }\n```"}}};
            SessionStore(data.path()).save({chat});
            MainWindow window(data.path());
            window.findChild<SettingsPage*>()->setPreference("personalization.appearance", theme);
            window.setAttribute(Qt::WA_DontShowOnScreen);
            window.resize(state.contains("compact") ? 350 : 1540, state.contains("compact") ? 500 : 940);
            window.show();
            QTest::qWait(50);
            if (state.startsWith("files"))
                window.findChild<QToolButton*>("toggleFileTreeButton")->click();
            if (state.startsWith("content")) {
                window.findChild<QToolButton*>("toggleFileTreeButton")->click();
                auto* tree = window.findChild<QTreeView*>("workspaceFiles");
                auto* model = qobject_cast<WorkspaceFileTreeModel*>(tree->model());
                emit tree->doubleClicked(model->index(file.fileName()));
                window.findChild<QToolButton*>("closeFileTreeButton")->click();
            }
            QTest::qWait(100);
            window.grab().save(out + "/" + state + "-" + theme + ".png");
        }
    return 0;
}
