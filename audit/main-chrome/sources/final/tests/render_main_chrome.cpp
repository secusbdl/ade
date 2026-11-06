#include "ui/MainWindow.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/SettingsPage.h"
#include "ui/SessionStore.h"
#include "browser/browserwidget.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
int main(int argc,char** argv){
    qputenv("QT_QPA_PLATFORM","windows");QApplication app(argc,argv);initializeAppStyle(app);
    const QString output=argc>1?QString::fromLocal8Bit(argv[1]):QStringLiteral("renders");QDir().mkpath(output);
    const QStringList variants={"light","dark","narrow","zoom130","collapsed","long-input"};QJsonArray measurements;
    for(const auto& variant:variants){
        QTemporaryDir data;QList<Conversation> sessions;
        for(int i=0;i<5;++i){Conversation chat;chat.id="fixture-"+QString::number(i);chat.title=QStringList{"Polish the native workspace","Browser automation research","Review the app settings","Add an accessible sidebar","Prepare release notes"}[i];
            if(!i){chat.draft="Refine the layout while preserving the saved workspace.";chat.messages={QJsonObject{{"role","user"},{"content","Make the workspace compact and easy to navigate."}},QJsonObject{{"role","assistant"},{"content","The sidebar can resize or collapse. The composer keeps its model and permission controls available at narrow widths."}}};}
            sessions.append(chat);
        }
        if(!SessionStore(data.path()).save(sessions))return 2;
        MainWindow window(data.path());window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(variant=="narrow"||variant=="zoom130"?960:1540,variant=="narrow"||variant=="zoom130"?600:940);
        auto* settings=window.findChild<SettingsPage*>();settings->setPreference("personalization.appearance",variant=="dark"?"dark":"light");
        if(variant=="zoom130")settings->setPreference("personalization.uiZoom",130);
        auto* model=window.findChild<QComboBox*>("modelSelector");model->addItem("Provider / a descriptive model name");model->setCurrentIndex(model->count()-1);
        window.show();if(!QTest::qWaitFor([&]{return window.findChild<BrowserWidget*>()->isReady();},15000))return 5;
        if(variant=="collapsed")window.findChild<QToolButton*>("sidebarToggleButton")->click();
        if(variant=="long-input")window.findChild<Composer*>()->setPlainText(QString("A growing draft retains every line while the input stops growing at its maximum height.\n").repeated(16));
        QTest::qWait(50);if(!window.grab().save(QDir(output).filePath(variant+".png")))return 3;
        auto* sidebar=window.findChild<QSplitter*>("sidebarSplitter");auto* inner=window.findChild<QSplitter*>("workspaceSplitter");auto* composer=window.findChild<Composer*>();
        measurements.append(QJsonObject{{"variant",variant},{"windowWidth",window.width()},{"windowHeight",window.height()},{"sidebarWidth",sidebar->sizes()[0]},{"chatWidth",inner->sizes()[0]},{"contentWidth",inner->sizes()[1]},{"inputHeight",composer->height()},{"stopHidden",window.findChild<QToolButton*>("stopButton")->isHidden()}});
    }
    QFile file(QDir(output).filePath("measurements.json"));if(!file.open(QIODevice::WriteOnly))return 4;file.write(QJsonDocument(measurements).toJson());return 0;
}
