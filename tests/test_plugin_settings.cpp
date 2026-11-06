#include "ui/SettingsPage.h"
#include "ui/AppStyle.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
bool openPlugin(SettingsPage& page,const QString& name){
    page.selectSection("plugins");QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    for(auto* button:page.findChildren<QPushButton*>())if(button->accessibleName()=="Configure "+name){button->click();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);return page.findChild<QLineEdit*>("pluginCredential");}
    return false;
}
QPushButton* button(SettingsPage& page,const char* name){return page.findChild<QPushButton*>(name);}
QString status(SettingsPage& page){const auto* label=page.findChild<QLabel*>("pluginConnectionStatus");return label?label->text():QString();}
QByteArray bytes(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
void saveToken(SettingsPage& page,const QString& value){page.findChild<QLineEdit*>("pluginCredential")->setText(value);button(page,"savePluginCredential")->click();}
}
class PluginSettingsTest:public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){initializeAppStyle(*qobject_cast<QApplication*>(qApp));}
    void sourceMetadataAndExplicitCheck_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<QString>("id");QTest::addColumn<QString>("url");
        QTest::newRow("github")<<QString("GitHub")<<QString("github")<<QString("https://github.com/settings/tokens");
        QTest::newRow("supabase")<<QString("Supabase")<<QString("supabase")<<QString("https://supabase.com/dashboard/account/tokens");
        QTest::newRow("figma")<<QString("Figma")<<QString("figma")<<QString("https://help.figma.com/hc/en-us/articles/8085703771159-Manage-personal-access-tokens");
        QTest::newRow("posthog")<<QString("PostHog")<<QString("posthog")<<QString("https://us.posthog.com/settings/user-api-keys");
        QTest::newRow("vercel")<<QString("Vercel")<<QString("vercel")<<QString("https://vercel.com/account/tokens");
    }
    void sourceMetadataAndExplicitCheck(){
        QFETCH(QString,name);QFETCH(QString,id);QFETCH(QString,url);QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,name));
        auto* input=page.findChild<QLineEdit*>("pluginCredential");QVERIFY(input);QCOMPARE(input->echoMode(),QLineEdit::Password);QVERIFY(input->text().isEmpty());QVERIFY(!button(page,"checkPluginConnection")->isEnabled());
        QSignalSpy changed(&page,&SettingsPage::pluginCredentialsChanged),checks(&page,&SettingsPage::pluginConnectionCheckRequested),links(&page,&SettingsPage::openUrlRequested);
        button(page,"pluginTokenHelp")->click();QCOMPARE(links.size(),1);QCOMPARE(links[0][0].toUrl().toString(),url);
        const QString token="plugin-fixture-token-not-a-real-secret";saveToken(page,token);QCOMPARE(changed.size(),1);QCOMPARE(checks.size(),0);QVERIFY(input->text().isEmpty());QVERIFY(button(page,"checkPluginConnection")->isEnabled());QVERIFY(status(page).contains("not been checked"));
        const auto path=data.filePath("plugin-credentials.json");QVERIFY(!bytes(path).contains(token.toUtf8()));QString error;const auto records=ProviderStore::load(path,&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(records.size(),1);QCOMPARE(records[0].id,id);QCOMPARE(records[0].apiKey,token);QVERIFY(!QFile::exists(data.filePath("providers.json")));
        button(page,"checkPluginConnection")->click();QCOMPARE(checks.size(),1);QCOMPARE(checks[0][1].toString(),id);const auto request=checks[0][0].toULongLong();QVERIFY(request);QVERIFY(!button(page,"checkPluginConnection")->isEnabled());
        page.completePluginConnectionCheck(request+1,id,true,"Wrong request");page.completePluginConnectionCheck(request,"other",true,"Wrong plugin");QVERIFY(status(page).startsWith("Checking"));
        page.completePluginConnectionCheck(request,id,true,"Read-only API request succeeded.");QVERIFY(status(page).startsWith("Connection verified."));QVERIFY(input->text().isEmpty());
    }
    void replacementAndRemovalPreserveOtherPlugins(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"github-fixture-old");QVERIFY(openPlugin(page,"Supabase"));saveToken(page,"supabase-fixture");QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"github-fixture-new");
        auto records=ProviderStore::load(data.filePath("plugin-credentials.json"));QCOMPARE(records.size(),2);for(const auto& record:records)if(record.id=="github")QCOMPARE(record.apiKey,QString("github-fixture-new"));
        button(page,"removePluginCredential")->click();records=ProviderStore::load(data.filePath("plugin-credentials.json"));QCOMPARE(records.size(),1);QCOMPARE(records[0].id,QString("supabase"));QVERIFY(!button(page,"checkPluginConnection")->isEnabled());QVERIFY(!button(page,"removePluginCredential")->isEnabled());QVERIFY(page.preference("plugins.github.enabled",true).toBool());
    }
    void externalCredentialChangePreservesDraftAndDisk(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"first-fixture");const auto path=data.filePath("plugin-credentials.json");auto external=ProviderStore::load(path);external[0].apiKey="external-fixture";QVERIFY(ProviderStore::save(path,external));const auto before=bytes(path);QSignalSpy changed(&page,&SettingsPage::pluginCredentialsChanged);
        saveToken(page,"unsaved-fixture");QCOMPARE(changed.size(),0);QCOMPARE(bytes(path),before);QCOMPARE(page.findChild<QLineEdit*>("pluginCredential")->text(),QString("unsaved-fixture"));button(page,"removePluginCredential")->click();QCOMPARE(bytes(path),before);
    }
    void corruptStorageCannotBeOverwritten(){
        QTemporaryDir data;const auto path=data.filePath("plugin-credentials.json");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("{broken credential bytes");file.close();const auto before=bytes(path);SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"fixture");QVERIFY(!button(page,"savePluginCredential")->isEnabled());QVERIFY(!button(page,"removePluginCredential")->isEnabled());QCOMPARE(bytes(path),before);
    }
    void tokenShapeRejectedWithoutPersisting(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));for(const auto& value:QStringList{"bad token","bad\ntoken",QString("bad")+QChar(1)+"token",QString("bad")+QChar(31)+"token",QString(65537,'x')}){saveToken(page,value);QVERIFY(!QFile::exists(data.filePath("plugin-credentials.json")));}
    }
    void editingNavigationAndExternalChangeRejectStaleChecks(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"fixture-one");QSignalSpy checks(&page,&SettingsPage::pluginConnectionCheckRequested),cancelled(&page,&SettingsPage::pluginConnectionCheckCancelled);
        button(page,"checkPluginConnection")->click();const auto first=checks.last()[0].toULongLong();page.findChild<QLineEdit*>("pluginCredential")->setText("fixture-two");QCOMPARE(cancelled.size(),1);QCOMPARE(cancelled[0][0].toULongLong(),first);page.completePluginConnectionCheck(first,"github",true,"Stale success");QVERIFY(status(page).startsWith("Unsaved"));button(page,"savePluginCredential")->click();button(page,"checkPluginConnection")->click();const auto second=checks.last()[0].toULongLong();
        auto external=ProviderStore::load(data.filePath("plugin-credentials.json"));external[0].apiKey="external-fixture";QVERIFY(ProviderStore::save(data.filePath("plugin-credentials.json"),external));page.completePluginConnectionCheck(second,"github",true,"Stale success");QVERIFY(status(page).contains("changed"));QVERIFY(!button(page,"checkPluginConnection")->isEnabled());
        QVERIFY(openPlugin(page,"GitHub"));button(page,"checkPluginConnection")->click();const auto third=checks.last()[0].toULongLong();page.selectSection("about");QCOMPARE(cancelled.last()[0].toULongLong(),third);page.completePluginConnectionCheck(third,"github",true,"Stale success");QCOMPARE(page.currentSection(),QString("about"));
    }
    void posthogRegionValidationRollbackAndCancellation(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"PostHog"));saveToken(page,"phx_fixture");QSignalSpy checks(&page,&SettingsPage::pluginConnectionCheckRequested),cancelled(&page,&SettingsPage::pluginConnectionCheckCancelled);button(page,"checkPluginConnection")->click();auto* region=page.findChild<QComboBox*>("pluginPosthogRegion");QVERIFY(region);region->setCurrentIndex(region->findData("eu"));QCOMPARE(page.preference("plugins.posthog.region").toString(),QString("eu"));QCOMPARE(cancelled.size(),1);QVERIFY(!page.setPreference("plugins.posthog.region","arbitrary-host"));
        SettingsPage external(data.path());QVERIFY(external.setPreference("plugins.posthog.region","us"));region->setCurrentIndex(region->findData("us"));QCOMPARE(region->currentData().toString(),QString("eu"));QCOMPARE(SettingsPage(data.path()).preference("plugins.posthog.region").toString(),QString("us"));
    }
    void enableFailureRollsBackWithoutRemovingToken(){
        QTemporaryDir data;SettingsPage page(data.path());QVERIFY(openPlugin(page,"GitHub"));saveToken(page,"fixture");SettingsPage external(data.path());QVERIFY(external.setPreference("plugins.github.enabled",false));auto* enabled=page.findChild<QCheckBox*>("pluginEnabled_github");QVERIFY(enabled);enabled->click();QVERIFY(enabled->isChecked());QCOMPARE(ProviderStore::load(data.filePath("plugin-credentials.json")).size(),1);
    }
    void mutationAndCancellationCallbacksMayDeletePage(){
        QTemporaryDir data;auto* page=new SettingsPage(data.path());QPointer<SettingsPage> alive(page);QVERIFY(openPlugin(*page,"GitHub"));connect(page,&SettingsPage::pluginCredentialsChanged,page,[page]{delete page;});saveToken(*page,"fixture");QVERIFY(!alive);QCOMPARE(ProviderStore::load(data.filePath("plugin-credentials.json")).size(),1);
        page=new SettingsPage(data.path());alive=page;QVERIFY(openPlugin(*page,"GitHub"));button(*page,"checkPluginConnection")->click();connect(page,&SettingsPage::pluginConnectionCheckCancelled,page,[page]{delete page;});page->reload();QVERIFY(!alive);
    }
    void directProviderImportEntryEmitsRequest(){QTemporaryDir data;SettingsPage page(data.path());page.selectSection("models-providers");QSignalSpy requested(&page,&SettingsPage::providerKeyImportRequested);auto* entry=button(page,"settingsImportProviderKeys");QVERIFY(entry);QCOMPARE(entry->text(),QString("Import API key file…"));entry->click();QCOMPARE(requested.size(),1);}
};
QTEST_MAIN(PluginSettingsTest)
#include "test_plugin_settings.moc"
