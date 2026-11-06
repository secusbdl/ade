#include "ui/SettingsPage.h"
#include "ui/SessionStore.h"
#include "core/ProviderStore.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>

using namespace ade;

class SettingsTest : public QObject {
    Q_OBJECT
private slots:
    void persistsIndependentSectionsAndSignals();
    void invalidStorageIsPreserved();
    void validatesSearchEngineAndUrls();
    void navigationAndArchiveRestoreDoNotMutateSessions();
    void pluginCredentialsAreEncryptedAndSeparate();
    void externalEditsAndInvalidFieldsArePreserved();
    void keyboardZoomPersistsAndRetiredControlsCannotSave();
    void corruptPluginStoreIsNotOverwritten();
    void sitePermissionsValidateAndPersist();
    void historyOpensThroughNativeHost();
};

void SettingsTest::persistsIndependentSectionsAndSignals() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SettingsPage page(directory.path());
    QSignalSpy changed(&page,&SettingsPage::preferencesChanged);
    QVERIFY(page.setPreference("agent.keepAwake",false));
    QVERIFY(page.setPreference("personalization.uiZoom",120));
    QVERIFY(page.setPreference("worktrees.setupScript",QString::fromUtf8("echo 日本語")));
    QCOMPARE(changed.count(),3);
    QVERIFY(!page.setPreference("personalization.uiZoom",200));
    QCOMPARE(changed.count(),3);
    SettingsPage reopened(directory.path());
    QCOMPARE(reopened.preference("agent.keepAwake").toBool(),false);
    QCOMPARE(reopened.preference("personalization.uiZoom").toInt(),120);
    QCOMPARE(reopened.preference("worktrees.setupScript").toString(),QString::fromUtf8("echo 日本語"));
    QCOMPARE(reopened.preference("browsing.defaultEngineId").toString(),QString("google"));
    reopened.selectSection("agent-general");
    auto *awake=reopened.findChild<QCheckBox *>("agentKeepAwake");
    QVERIFY(awake);QVERIFY(!awake->isChecked());awake->click();
    SettingsPage again(directory.path());
    QVERIFY(again.preference("agent.keepAwake").toBool());
}

void SettingsTest::invalidStorageIsPreserved() {
    QTemporaryDir directory;
    const QString path=directory.path()+"/preferences.json";
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("{broken preference data");file.close();
    SettingsPage page(directory.path());
    QVERIFY(!page.storageError().isEmpty());
    QVERIFY(!page.setPreference("agent.keepAwake",false));
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),QByteArray("{broken preference data"));
}

void SettingsTest::validatesSearchEngineAndUrls() {
    QTemporaryDir directory;
    SettingsPage page(directory.path());
    QVERIFY(!page.addSearchEngine("","example","https://example.test/?q=%s"));
    QVERIFY(!page.addSearchEngine("Example","example","https://example.test/search"));
    QVERIFY(!page.addSearchEngine("Example","example","file:///C:/tmp/%s"));
    QVERIFY(!page.addSearchEngine("Example","example","https://user:secret@example.test/?q=%s"));
    QVERIFY(page.addSearchEngine("Example","example.test","https://example.test/?q=%s"));
    QVERIFY(!page.addSearchEngine("Duplicate","EXAMPLE.TEST","https://other.test/?q=%s"));
    QCOMPARE(page.preference("browsing.searchEngines").toArray().size(),4);
    QVERIFY(!page.setPreference("browsing.newTabUrl","javascript:alert(1)"));
    QVERIFY(!page.setPreference("browsing.startupUrl","https://key:secret@example.test"));
    QVERIFY(page.setPreference("browsing.newTabUrl","https://example.test/app"));
    SettingsPage reopened(directory.path());
    QCOMPARE(reopened.preference("browsing.searchEngines").toArray().size(),4);
    QCOMPARE(reopened.preference("browsing.newTabUrl").toString(),QString("https://example.test/app"));
}

void SettingsTest::navigationAndArchiveRestoreDoNotMutateSessions() {
    QTemporaryDir directory;
    Conversation conversation;conversation.id="archived-one";conversation.title="Archived fixture";conversation.archived=true;
    QVERIFY(SessionStore(directory.path()).save({conversation}));
    SettingsPage page(directory.path());
    QSignalSpy back(&page,&SettingsPage::backRequested);
    QSignalSpy providers(&page,&SettingsPage::providersRequested);
    QSignalSpy restore(&page,&SettingsPage::restoreConversationRequested);
    auto *backButton=page.findChild<QPushButton *>("settingsBack");QVERIFY(backButton);backButton->click();QCOMPARE(back.count(),1);
    auto *providerButton=page.findChild<QPushButton *>("settingsAddProvider");QVERIFY(providerButton);providerButton->click();QCOMPARE(providers.count(),1);
    for(const auto &id:{"agent-general","models-providers","skills-context","worktree-setup","plugins","archived-agents","personalization","browsing","clear-data","history","account","about"}){
        page.selectSection(id);QCOMPARE(page.currentSection(),QString(id));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    page.selectSection("missing-route");QCOMPARE(page.currentSection(),QString("about"));
    page.selectSection("archived-agents");QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    auto *list=page.findChild<QListWidget *>("archivedChats");QVERIFY(list);QCOMPARE(list->count(),1);
    auto *restoreButton=page.findChild<QPushButton *>("restoreArchivedChat_archived-one");QVERIFY(restoreButton);QCOMPARE(restoreButton->property("conversationId").toString(),conversation.id);restoreButton->click();
    QCOMPARE(restore.count(),1);QCOMPARE(restore.first().first().toString(),QString("archived-one"));
    QVERIFY(SessionStore(directory.path()).load().first().archived);
}

void SettingsTest::pluginCredentialsAreEncryptedAndSeparate() {
    QTemporaryDir directory;
    SettingsPage page(directory.path());page.selectSection("plugins");
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QPushButton *details=nullptr;
    for(auto *button:page.findChildren<QPushButton *>())if(button->accessibleName()=="Configure GitHub")details=button;
    QVERIFY(details);details->click();QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    auto *key=page.findChild<QLineEdit *>("pluginCredential");QVERIFY(key);QCOMPARE(key->echoMode(),QLineEdit::Password);
    const QString sentinel="settings-fixture-credential-not-real";key->setText(sentinel);
    auto *save=page.findChild<QPushButton *>("savePluginCredential");QVERIFY(save);save->click();
    QCOMPARE(key->text(),QString());
    QFile stored(directory.path()+"/plugin-credentials.json");QVERIFY(stored.open(QIODevice::ReadOnly));
    const auto bytes=stored.readAll();QVERIFY(!bytes.contains(sentinel.toUtf8()));stored.close();
    QString error;const auto credentials=ProviderStore::load(stored.fileName(),&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(credentials.size(),1);QCOMPARE(credentials.first().apiKey,sentinel);
    QCOMPARE(credentials.first().baseUrl,QString("https://api.github.com"));
    QVERIFY(!QFile::exists(directory.path()+"/providers.json"));
    auto *remove=page.findChild<QPushButton *>("removePluginCredential");QVERIFY(remove);remove->click();
    QVERIFY(ProviderStore::load(stored.fileName(),&error).isEmpty());QVERIFY(error.isEmpty());
}

void SettingsTest::externalEditsAndInvalidFieldsArePreserved() {
    QTemporaryDir directory;
    SettingsPage page(directory.path());
    QVERIFY(page.setPreference("agent.keepAwake",false));
    const QString path=directory.path()+"/preferences.json";
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
    const QByteArray external="external update while settings are open";
    file.write(external);file.close();
    QVERIFY(!page.setPreference("personalization.uiZoom",110));
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),external);file.close();

    auto invalid=SettingsPage::defaultPreferences();invalid["agent"]=QString("invalid field type");
    const auto malformed=QJsonDocument(QJsonObject{{"schemaVersion",1},{"preferences",invalid}}).toJson();
    QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));file.write(malformed);file.close();
    page.reload();QVERIFY(!page.storageError().isEmpty());
    QVERIFY(!page.setPreference("agent.keepAwake",true));
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),malformed);file.close();

    QTemporaryDir clean;
    SettingsPage fresh(clean.path());
    QVERIFY(!fresh.setPreference("worktrees.setupScript",QString(2*1024*1024,'x')));
    QVERIFY(!fresh.setPreference("browsing.newTabUrl",123));
    QVERIFY(!fresh.setPreference("skills",QJsonArray{42}));
    QVERIFY(!QFile::exists(clean.path()+"/preferences.json"));
}

void SettingsTest::keyboardZoomPersistsAndRetiredControlsCannotSave() {
    QTemporaryDir directory;
    SettingsPage page(directory.path());page.selectSection("personalization");
    auto *zoom=page.findChild<QSlider *>("settingsUiZoom");QVERIFY(zoom);
    QTest::keyClick(zoom,Qt::Key_Right);
    QCOMPARE(page.preference("personalization.uiZoom").toInt(),101);
    SettingsPage reopened(directory.path());
    QCOMPARE(reopened.preference("personalization.uiZoom").toInt(),101);
    // A page replacement must leave an emitting sender alive until its signal
    // returns, but detached controls cannot write through stale connections.
    page.selectSection("agent-general");
    zoom->setValue(120);
    QCOMPARE(page.preference("personalization.uiZoom").toInt(),101);
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
}

void SettingsTest::corruptPluginStoreIsNotOverwritten() {
    QTemporaryDir directory;
    const auto path=directory.path()+"/plugin-credentials.json";
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray broken="{broken credential data";file.write(broken);file.close();
    SettingsPage page(directory.path());page.selectSection("plugins");
    QPushButton *details=nullptr;
    for(auto *button:page.findChildren<QPushButton *>())if(button->accessibleName()=="Configure Figma")details=button;
    QVERIFY(details);details->click();
    auto *token=page.findChild<QLineEdit *>("pluginCredential");QVERIFY(token);
    token->setText("fixture-only-not-a-real-key");
    page.findChild<QPushButton *>("savePluginCredential")->click();
    page.findChild<QPushButton *>("removePluginCredential")->click();
    QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),broken);
}

void SettingsTest::sitePermissionsValidateAndPersist(){
    QTemporaryDir directory;SettingsPage page(directory.path());
    QVERIFY(page.setSitePermission("camera",QUrl("https://Example.com:443/"),"block"));
    QCOMPARE(page.preference("browsing.permissionExceptions").toObject()["camera"].toObject()["https://example.com"].toString(),QString("block"));
    QVERIFY(!page.setSitePermission("camera",QUrl("https://user:password@example.com"),"allow"));
    QVERIFY(!page.setSitePermission("camera",QUrl("https://example.com/path"),"allow"));
    QVERIFY(!page.setSitePermission("camera",QUrl("https://example.com?q=1"),"allow"));
    QVERIFY(!page.setSitePermission("unknown",QUrl("https://example.com"),"allow"));
    QVERIFY(!page.setSitePermission("camera",QUrl("https://example.com"),"invalid"));
    SettingsPage reopened(directory.path());QCOMPARE(reopened.preference("browsing.permissionExceptions"),page.preference("browsing.permissionExceptions"));
    QVERIFY(page.setSitePermission("camera",QUrl("https://example.com"),"default"));QVERIFY(page.preference("browsing.permissionExceptions").toObject().isEmpty());
    page.selectSection("browsing");QCOMPARE(page.findChild<QComboBox*>("permission_fullscreen")->currentData().toString(),QString("allow"));
}

void SettingsTest::historyOpensThroughNativeHost(){
    QTemporaryDir directory;QFile history(directory.filePath("browsing-history.json"));QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("[{\"url\":\"https://example.test/page\",\"title\":\"Fixture\"}]");history.close();
    SettingsPage page(directory.path());QSignalSpy opened(&page,&SettingsPage::openUrlRequested);page.selectSection("history");
    auto* list=page.findChild<QListWidget*>("browsingHistory");QVERIFY(list);QCOMPARE(list->count(),1);emit list->itemDoubleClicked(list->item(0));
    QCOMPARE(opened.size(),1);QCOMPARE(opened.first().first().toUrl(),QUrl("https://example.test/page"));
}

QTEST_MAIN(SettingsTest)
#include "test_settings.moc"
