#include "ui/SettingsPage.h"
#include "ui/AppStyle.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QDir>
#include <QPixmap>
#include <QTemporaryDir>

// Explicit visual-review utility. Uses an isolated profile and no network.
int main(int argc,char** argv){
    QApplication app(argc,argv);ade::initializeAppStyle(app);
    if(app.arguments().size()!=2)return 2;
    const QString output=QDir(app.arguments()[1]).absolutePath();if(!QDir().mkpath(output))return 3;
    QTemporaryDir profile;ade::ProviderConfig provider;provider.id="deepseek";provider.name="DeepSeek API";provider.model="deepseek-v4-pro";provider.baseUrl="https://api.deepseek.com/v1";
    if(!ade::ProviderStore::save(profile.filePath("providers.json"),{provider}))return 4;
    ade::SettingsPage page(profile.path());page.setAttribute(Qt::WA_DontShowOnScreen);page.setPreference("personalization.appearance","light");page.resize(1600,940);page.show();
    for(const auto& section:{"models-providers","plugins","browsing","clear-data","skills-context"}){
        page.selectSection(section);app.processEvents();
        if(!page.grab().save(QDir(output).filePath(QString(section)+".png")))return 5;
    }
    page.resize(960,720);page.selectSection("plugins");app.processEvents();if(!page.grab().save(QDir(output).filePath("plugins-narrow.png")))return 6;
    page.resize(1600,940);page.setPreference("personalization.appearance","dark");page.selectSection("plugins");app.processEvents();if(!page.grab().save(QDir(output).filePath("plugins-dark.png")))return 7;
    return 0;
}
