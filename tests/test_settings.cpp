#include "ui/SettingsPage.h"
#include "ui/SessionStore.h"
#include "core/ProviderStore.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QToolButton>

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
    void objectSitePermissionRecords_data();
    void objectSitePermissionRecords();
    void historyOpensThroughNativeHost();
    // Notification sound controls (runtime-services slice 3). The old page had
    // an Off/Normal/Loud combo, a "still being connected" placeholder and no
    // pack, preview or import controls, and accepted any loudness/pack string.
    void generalSoundControlsWithoutHooks();
    void generalSoundControlsWithHooks();
    void agentPreferenceValidation();
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
    QSignalSpy addProvider(&page,&SettingsPage::addProviderRequested);
    QSignalSpy restore(&page,&SettingsPage::restoreConversationRequested);
    auto *backButton=page.findChild<QPushButton *>("settingsBack");QVERIFY(backButton);backButton->click();QCOMPARE(back.count(),1);
    auto *providerButton=page.findChild<QPushButton *>("settingsAddProvider");QVERIFY(providerButton);providerButton->click();QCOMPARE(addProvider.count(),1);QCOMPARE(providers.count(),0);
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

void SettingsTest::objectSitePermissionRecords_data(){
    QTest::addColumn<QJsonValue>("record");QTest::addColumn<QString>("label");QTest::addColumn<bool>("valid");
    for(const auto &setting:QStringList{"ask","allow","block"})
        QTest::newRow(qPrintable("object-"+setting))<<QJsonValue(QJsonObject{{"setting",setting}})<<setting<<true;
    for(int setting=0;setting<3;++setting)
        QTest::newRow(qPrintable("object-enum-"+QString::number(setting)))<<QJsonValue(QJsonObject{{"setting",setting}})<<QStringList{"ask","allow","block"}[setting]<<true;
    QTest::newRow("object-missing")<<QJsonValue(QJsonObject{})<<QString()<<false;
    for(const auto &entry:QList<QPair<QString,QJsonValue>>{{"null",QJsonValue(QJsonValue::Null)},{"boolean",true},{"negative",-1},{"large",3},{"fractional",1.5},{"unknown","unknown"},{"array",QJsonArray{}},{"nested",QJsonObject{}}})
        QTest::newRow(qPrintable("object-invalid-"+entry.first))<<QJsonValue(QJsonObject{{"setting",entry.second}})<<QString()<<false;
}

void SettingsTest::objectSitePermissionRecords(){
    QFETCH(QJsonValue,record);QFETCH(QString,label);QFETCH(bool,valid);
    QTemporaryDir directory;QVERIFY(directory.isValid());auto preferences=SettingsPage::defaultPreferences();
    auto browsing=preferences.value("browsing").toObject();browsing["permissionExceptions"]=QJsonObject{{"camera",QJsonObject{{"https://example.test",record}}}};preferences["browsing"]=browsing;
    const auto original=QJsonDocument(QJsonObject{{"schemaVersion",1},{"preferences",preferences}}).toJson();
    QFile file(directory.filePath("preferences.json"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),original.size());file.close();
    SettingsPage page(directory.path());
    if(valid){
        QVERIFY2(page.storageError().isEmpty(),qPrintable(page.storageError()));
        QCOMPARE(page.preference("browsing.permissionExceptions").toObject().value("camera").toObject().value("https://example.test"),record);
        page.selectSection("browsing");bool rendered=false;
        for(const auto* text:page.findChildren<QLabel*>())if(text->text()==QString("camera · ")+label)rendered=true;
        QVERIFY2(rendered,"Stored object permission must render its actual setting label.");
        QVERIFY(page.setPreference("agent.keepAwake",false));SettingsPage reopened(directory.path());QVERIFY(reopened.storageError().isEmpty());
        QCOMPARE(reopened.preference("browsing.permissionExceptions").toObject().value("camera").toObject().value("https://example.test"),record);
    }else{
        QVERIFY(!page.storageError().isEmpty());QVERIFY(!page.setPreference("agent.keepAwake",false));
        QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);
    }
}

void SettingsTest::historyOpensThroughNativeHost(){
    QTemporaryDir directory;QFile history(directory.filePath("browsing-history.json"));QVERIFY(history.open(QIODevice::WriteOnly));
    history.write("[{\"url\":\"https://example.test/page\",\"title\":\"Fixture\"}]");history.close();
    SettingsPage page(directory.path());QSignalSpy opened(&page,&SettingsPage::openUrlRequested);page.selectSection("history");
    auto* list=page.findChild<QListWidget*>("browsingHistory");QVERIFY(list);QCOMPARE(list->count(),1);emit list->itemDoubleClicked(list->item(0));
    QCOMPARE(opened.size(),1);QCOMPARE(opened.first().first().toUrl(),QUrl("https://example.test/page"));
}

namespace {
QStringList comboTexts(QComboBox* combo){QStringList texts;for(int i=0;i<combo->count();++i)texts.append(combo->itemText(i));return texts;}
void flushDeletes(){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
}

void SettingsTest::generalSoundControlsWithoutHooks(){
    QTemporaryDir directory;SettingsPage page(directory.path());page.selectSection("agent-general");
    auto* loudness=page.findChild<QComboBox*>("notificationLoudness");QVERIFY(loudness);
    QCOMPARE(comboTexts(loudness),QStringList({"Off","Subtle","Loud"}));
    QCOMPARE(loudness->currentData().toString(),QString("normal"));
    auto* pack=page.findChild<QComboBox*>("notificationSoundPack");QVERIFY(pack);
    QCOMPARE(comboTexts(pack),QStringList({"ADE Chime"}));QCOMPARE(pack->currentData().toString(),QString("ade-chime"));
    auto* preview=page.findChild<QToolButton*>("notificationSoundPreview");QVERIFY(preview);QVERIFY(!preview->isEnabled());
    auto* import=page.findChild<QPushButton*>("notificationSoundImport");QVERIFY(import);QVERIFY(!import->isEnabled());
    for(auto* label:page.findChildren<QLabel*>())QVERIFY2(!label->text().contains("still being connected"),qPrintable(label->text()));
}

void SettingsTest::generalSoundControlsWithHooks(){
    QTemporaryDir directory;SettingsPage page(directory.path());page.selectSection("agent-general");
    const QString imported="00000000-0000-4000-8000-000000000001";
    QList<QPair<QString,QString>> previews;int imports=0;SettingsPage::SoundImportOutcome next;
    SettingsPage::SoundHooks hooks;
    hooks.packs=[imported]{return QList<QPair<QString,QString>>{{"ade-chime","ADE Chime"},{"ade-soft","Soft Tones"},{imported,"Mine"}};};
    hooks.preview=[&previews](const QString& pack,const QString& loudness){previews.append({pack,loudness});return true;};
    hooks.chooseAndImport=[&]{++imports;return next;};
    page.setSoundHooks(hooks);flushDeletes();
    auto* pack=page.findChild<QComboBox*>("notificationSoundPack");auto* loudness=page.findChild<QComboBox*>("notificationLoudness");
    auto* preview=page.findChild<QToolButton*>("notificationSoundPreview");auto* status=page.findChild<QLabel*>("settingsStatus");
    QVERIFY(pack&&loudness&&preview&&status);
    QCOMPARE(comboTexts(pack),QStringList({"ADE Chime","Soft Tones","Mine"}));QCOMPARE(pack->currentData().toString(),QString("ade-chime"));
    QVERIFY(preview->isEnabled());QCOMPARE(previews.size(),0);
    pack->setCurrentIndex(1);
    QCOMPARE(page.preference("agent.notificationSound").toString(),QString("ade-soft"));
    QCOMPARE(previews,(QList<QPair<QString,QString>>{{"ade-soft","normal"}}));
    loudness->setCurrentIndex(loudness->findData("loud"));
    QCOMPARE(previews.size(),2);QCOMPARE(previews.last(),(QPair<QString,QString>{"ade-soft","loud"}));
    loudness->setCurrentIndex(loudness->findData("off"));
    QCOMPARE(page.preference("agent.notificationLoudness").toString(),QString("off"));
    QCOMPARE(previews.size(),2);QVERIFY(!preview->isEnabled());
    loudness->setCurrentIndex(loudness->findData("normal"));
    QCOMPARE(previews.size(),3);QVERIFY(preview->isEnabled());
    preview->click();QCOMPARE(previews.size(),4);QCOMPARE(previews.last(),(QPair<QString,QString>{"ade-soft","normal"}));
    // Successful import selects the new pack and reports it.
    next={false,imported,"Mine",{}};
    page.findChild<QPushButton*>("notificationSoundImport")->click();flushDeletes();
    QCOMPARE(imports,1);QCOMPARE(page.preference("agent.notificationSound").toString(),imported);
    QCOMPARE(status->text(),QString("Mine is now selected for notifications."));
    pack=page.findChild<QComboBox*>("notificationSoundPack");QCOMPARE(pack->currentData().toString(),imported);
    // Errors are reported and leave the selection unchanged.
    next={false,{},{},"The file is not valid JSON."};
    page.findChild<QPushButton*>("notificationSoundImport")->click();
    QCOMPARE(imports,2);QCOMPARE(status->text(),QString("The file is not valid JSON."));
    QCOMPARE(page.preference("agent.notificationSound").toString(),imported);
    // Cancel shows nothing new.
    next={true,{},{},{}};const auto before=status->text();
    page.findChild<QPushButton*>("notificationSoundImport")->click();
    QCOMPARE(imports,3);QCOMPARE(status->text(),before);QCOMPARE(page.preference("agent.notificationSound").toString(),imported);
    QCOMPARE(SettingsPage(directory.path()).preference("agent.notificationSound").toString(),imported);
}

void SettingsTest::agentPreferenceValidation(){
    QTemporaryDir directory;SettingsPage page(directory.path());
    for(const auto& value:{"max","Normal","subtle",""})QVERIFY2(!page.setPreference("agent.notificationLoudness",QString(value)),value);
    for(const auto& value:QStringList{"..","../x",".","-lead","a/b","a.b","x\n",QString(65,'a')})QVERIFY2(!page.setPreference("agent.notificationSound",value),qPrintable(value));
    for(const auto& value:{"off","normal","loud"})QVERIFY(page.setPreference("agent.notificationLoudness",QString(value)));
    for(const auto& value:QStringList{"default","ade-soft","00000000-0000-4000-8000-000000000001",QString(64,'a')})QVERIFY2(page.setPreference("agent.notificationSound",value),qPrintable(value));
    // Files written by the previous page (default values) still load.
    QTemporaryDir legacy;QFile file(legacy.filePath("preferences.json"));QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"({"schemaVersion":1,"preferences":{"agent":{"keepAwake":false,"notificationLoudness":"loud","notificationSound":"default"}}})");file.close();
    SettingsPage reopened(legacy.path());QVERIFY(reopened.storageError().isEmpty());QCOMPARE(reopened.preference("agent.notificationLoudness").toString(),QString("loud"));
    QTemporaryDir hostile;QFile bad(hostile.filePath("preferences.json"));QVERIFY(bad.open(QIODevice::WriteOnly));
    // Valid JSON, but the stored pack id is a path: the store is marked invalid.
    bad.write(R"({"schemaVersion":1,"preferences":{"agent":{"notificationSound":"../evil"}}})");bad.close();
    QVERIFY(!SettingsPage(hostile.path()).storageError().isEmpty());
}

QTEST_MAIN(SettingsTest)
#include "test_settings.moc"
