#include "ui/SettingsPage.h"
#include "ui/ContextPreferences.h"
#include "core/ContextLoader.h"
#include "core/ProviderStore.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size();}
QByteArray get(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
bool makeSkill(const QString &directory){return put(directory+"/shared/SKILL.md","---\nname: settings-audit-shared\ndescription: Isolated settings audit fixture\n---\nFixture instruction.\n");}
QCheckBox *check(SettingsPage &page,const QString &label){for(auto *box:page.findChildren<QCheckBox*>())if(box->text()==label)return box;return nullptr;}
QListWidgetItem *skill(SettingsPage &page){auto *list=page.findChild<QListWidget*>("discoveredSkills");if(!list)return nullptr;for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()=="settings-audit-shared")return list->item(i);return nullptr;}
QJsonObject model(){return {{"providerInstanceId","provider-a"},{"modelId","model-a"}};}
QJsonObject preset(){auto value=model();value["id"]="preset-a";value["name"]="Fixture";value["models"]=QJsonArray{model()};return value;}
}
class SettingsAudit final:public QObject{
    Q_OBJECT
private slots:
    void workspaceInstructionsAndDisabledSkillsStayScoped();
    void disablingGlobalSkillPreservesWorkspaceOverride();
    void failedInstructionSaveRollsBackControl();
    void failedDirectorySaveRollsBackControl();
    void failedGlobalOptInSaveRollsBackControl();
    void invalidNestedModelEntriesAreRejected_data();
    void invalidNestedModelEntriesAreRejected();
    void unavailablePresetNeverSilentlySelectsAnotherProvider();
};
void SettingsAudit::workspaceInstructionsAndDisabledSkillsStayScoped(){
    QTemporaryDir data,root;const auto a=root.path()+"/workspace.a",b=root.path()+"/workspace.b";
    QVERIFY(makeSkill(a+"/.stagewise/skills"));QVERIFY(makeSkill(b+"/.stagewise/skills"));
    SettingsPage page(data.path());page.setContextWorkspace(a);page.selectSection("skills-context");
    auto *respect=check(page,"Respect workspace AGENTS.md");QVERIFY(respect);QVERIFY(respect->isChecked());respect->click();
    QVERIFY(!contextOptionsFor(page.preferences(),a,{}).respectAgentsMd);
    QVERIFY2(contextOptionsFor(page.preferences(),b,{}).respectAgentsMd,"A workspace instruction preference leaked into another workspace.");
    auto *entry=skill(page);QVERIFY(entry);entry->setCheckState(Qt::Unchecked);
    QVERIFY(contextOptionsFor(page.preferences(),a,{}).disabledSkills.contains("settings-audit-shared"));
    QVERIFY(!contextOptionsFor(page.preferences(),b,{}).disabledSkills.contains("settings-audit-shared"));
    SettingsPage reopened(data.path());QVERIFY(!contextOptionsFor(reopened.preferences(),a,{}).respectAgentsMd);
    QVERIFY(contextOptionsFor(reopened.preferences(),b,{}).respectAgentsMd);
    const auto alias=a+"/../workspace.a";QVERIFY(!contextOptionsFor(reopened.preferences(),alias,{}).respectAgentsMd);
}
void SettingsAudit::disablingGlobalSkillPreservesWorkspaceOverride(){
    QTemporaryDir data,global,workspace;QVERIFY(makeSkill(global.path()));QVERIFY(makeSkill(workspace.path()+"/.stagewise/skills"));
    SettingsPage page(data.path());QVERIFY(page.setPreference("skills",QJsonArray{QJsonObject{{"path",global.path()},{"enabled",true}}}));
    page.selectSection("skills-context");auto *entry=skill(page);QVERIFY(entry);QCOMPARE(entry->data(Qt::UserRole+1).toString(),QString("global"));entry->setCheckState(Qt::Unchecked);
    const auto options=contextOptionsFor(page.preferences(),workspace.path(),{});
    QVERIFY(options.disabledGlobalSkills.contains("settings-audit-shared"));
    QVERIFY(!options.disabledSkills.contains("settings-audit-shared"));
    ContextLoader loader;const auto snapshot=loader.load(options);QVERIFY(std::any_of(snapshot.skills.begin(),snapshot.skills.end(),[](const ContextSkill &s){return s.name=="settings-audit-shared"&&s.source=="workspace";}));
}
void SettingsAudit::failedInstructionSaveRollsBackControl(){
    QTemporaryDir data,workspace;SettingsPage page(data.path());page.setContextWorkspace(workspace.path());page.selectSection("skills-context");auto *respect=check(page,"Respect workspace AGENTS.md");QVERIFY(respect);
    const QByteArray corrupt="{external invalid change";QVERIFY(put(data.path()+"/preferences.json",corrupt));respect->click();
    QVERIFY(contextOptionsFor(page.preferences(),workspace.path(),{}).respectAgentsMd);QVERIFY(respect->isChecked());QCOMPARE(get(data.path()+"/preferences.json"),corrupt);
}
void SettingsAudit::failedDirectorySaveRollsBackControl(){
    QTemporaryDir data,directory;SettingsPage page(data.path());QVERIFY(page.setPreference("skills",QJsonArray{QJsonObject{{"path",directory.path()},{"enabled",true}}}));page.selectSection("skills-context");auto *enabled=check(page,"Enabled");QVERIFY(enabled);
    const QByteArray corrupt="{external invalid change";QVERIFY(put(data.path()+"/preferences.json",corrupt));enabled->click();
    QVERIFY(page.preference("skills").toArray().first().toObject()["enabled"].toBool());QVERIFY(enabled->isChecked());QCOMPARE(get(data.path()+"/preferences.json"),corrupt);
}
void SettingsAudit::failedGlobalOptInSaveRollsBackControl(){
    QTemporaryDir data;SettingsPage page(data.path());page.selectSection("skills-context");auto *enabled=check(page,"~/.codex/skills");QVERIFY(enabled);QVERIFY(!enabled->isChecked());
    const QByteArray corrupt="{external invalid change";QVERIFY(put(data.path()+"/preferences.json",corrupt));enabled->click();
    QVERIFY(!contextOptionsFor(page.preferences(),{},{}).enabledGlobalSkillDirs.contains("globalskills-codex"));QVERIFY(!enabled->isChecked());QCOMPARE(get(data.path()+"/preferences.json"),corrupt);
}
void SettingsAudit::invalidNestedModelEntriesAreRejected_data(){
    QTest::addColumn<QJsonObject>("invalid");
    auto p=preset();p["models"]="invalid";QTest::newRow("non-array-main-models")<<p;
    p=preset();p["models"]=QJsonArray{"invalid"};QTest::newRow("non-object-main-model")<<p;
    p=preset();auto m=model();m["overrides"]=QJsonObject{{"maxOutputTokens",-1}};p["models"]=QJsonArray{m};QTest::newRow("negative-nested-output-limit")<<p;
    p=preset();m=model();m["overrides"]=QJsonObject{{"thinkingBudget",1.5}};p["models"]=QJsonArray{m};QTest::newRow("fractional-thinking-budget")<<p;
    p=preset();m=model();m["providerInstanceId"]="";p["models"]=QJsonArray{m};QTest::newRow("empty-nested-provider")<<p;
    p=preset();p["titleGeneration"]=QJsonArray{false};QTest::newRow("invalid-title-model")<<p;
    p=preset();p["contextCompression"]="invalid";QTest::newRow("non-array-compression-models")<<p;
}
void SettingsAudit::invalidNestedModelEntriesAreRejected(){QFETCH(QJsonObject,invalid);QTemporaryDir data;SettingsPage page(data.path());QVERIFY(page.setPreference("modelPresets",QJsonArray{preset()}));const auto before=get(data.path()+"/preferences.json");QVERIFY(!page.setPreference("modelPresets",QJsonArray{invalid}));QCOMPARE(get(data.path()+"/preferences.json"),before);}
void SettingsAudit::unavailablePresetNeverSilentlySelectsAnotherProvider(){
    QTemporaryDir data;ProviderConfig available;available.id="provider-a";available.name="Available provider";available.baseUrl="https://example.test/v1";available.model="model-a";available.apiKey="isolated-fixture-key";QVERIFY(ProviderStore::save(data.path()+"/providers.json",{available}));
    SettingsPage page(data.path());auto saved=preset();saved["providerInstanceId"]="removed-provider";saved["modelId"]="removed-model";saved["models"]=QJsonArray{QJsonObject{{"providerInstanceId","removed-provider"},{"modelId","removed-model"}}};QVERIFY(page.setPreference("modelPresets",QJsonArray{saved}));page.selectSection("models-providers");
    QPushButton *edit=nullptr;for(auto *button:page.findChildren<QPushButton*>())if(button->text()=="Edit"){edit=button;break;}QVERIFY(edit);bool observed=false,substituted=false;
    QTimer::singleShot(0,&page,[&]{auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;auto *picker=dialog->findChild<QComboBox*>("presetModel");if(picker){observed=true;substituted=picker->currentData().toJsonObject()["providerInstanceId"].toString()=="provider-a";}dialog->reject();});edit->click();QVERIFY(observed);QVERIFY2(!substituted,"Editing an unavailable preset implicitly selected a different provider.");
}
QTEST_MAIN(SettingsAudit)
#include "test_settings_audit.moc"
