#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "harness/OpenCodeModelDiscovery.h"
#include "ui/AddProviderDialog.h"
#include "ui/ModelSelection.h"
#include "ui/ProviderCatalog.h"
#include "ui/ProviderDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class OpenCodeProviderUiTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() const {
    auto p = ProviderCatalog::instantiate("opencode");
    p.id = "opencode-fixture";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("acp_fixture.exe");
    return p;
  }
  QPushButton *connectButton(ProviderDialog &dialog) {
    return dialog.findChild<QPushButton *>("connectOpenCode");
  }
private slots:
  void cleanup() {
    qunsetenv("ADE_ACP_SCENARIO");
    qunsetenv("ADE_ACP_LOG");
  }
  void pickerCreatesExternalKindWithDefaultModel() {
    ProviderDialog dialog({});
    dialog.show();
    dialog.startAddProvider();
    auto *picker = dialog.findChild<AddProviderDialog *>();
    QVERIFY(picker);
    auto *card = picker->findChild<QPushButton *>("providerChoice_opencode");
    QVERIFY(card);
    card->click();
    QTRY_COMPARE(dialog.providers().size(), 1);
    const auto p = dialog.providers().first();
    QVERIFY(p.isOpenCodeHarness());
    QCOMPARE(p.protocol, QString("opencode-acp"));
    QCOMPARE(p.model, QString("default"));
    QVERIFY(p.baseUrl.isEmpty());
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(connectButton(dialog));
    QVERIFY(!dialog.findChild<QLineEdit *>("providerApiKey")->isVisible());
  }
  void discoveryAndSavedConfigAreNativeAndTokenFree() {
    const auto original = provider();
    ProviderDialog dialog({original});
    dialog.show();
    auto *button = connectButton(dialog);
    QVERIFY(button);
    button->click();
    QTRY_VERIFY(dialog.modelCatalogs().contains(original.id));
    QTRY_VERIFY(button->isEnabled());
    const auto catalog = dialog.modelCatalogs()[original.id].toObject();
    QCOMPARE(catalog["kind"].toString(), QString("opencode"));
    QCOMPARE(catalog["models"].toArray().size(), 2);
    QVERIFY(
        !QJsonDocument(catalog).toJson().contains("SYNTHETIC_SECRET_CATALOG"));
    const auto choices = availableModelSelections(dialog.providers(),
                                                  dialog.modelCatalogs(), {});
    QCOMPARE(choices.size(), 2);
    bool honest = false;
    for (auto *label : dialog.findChildren<QLabel *>())
      if (label->text().contains("did not verify sign-in"))
        honest = true;
    QVERIFY(honest);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto p = dialog.providers().first();
    QVERIFY(p.isOpenCodeHarness());
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(p.baseUrl.isEmpty());
    QTemporaryDir dir;
    QString error;
    QVERIFY2(ProviderStore::save(dir.filePath("providers.json"), {p}, &error),
             qPrintable(error));
    const auto restored =
        ProviderStore::load(dir.filePath("providers.json"), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(restored.size(), 1);
    QCOMPARE(restored.first().harnessExecutable, p.harnessExecutable);
    QVERIFY(restored.first().isOpenCodeHarness());
  }
  void thinkingSettingSurvivesSaveAndSelectsAdvertisedOffValue() {
    auto p = provider();
    p.model = "model-b";
    p.thinkingMode = "disabled";
    ProviderDialog dialog({p});
    dialog.show();
    auto *thinking = dialog.findChild<QComboBox *>("providerThinking");
    QVERIFY(thinking);
    QVERIFY(thinking->isVisible());
    QVERIFY(thinking->isEnabled());
    QCOMPARE(thinking->currentData().toString(), QString("disabled"));
    dialog.findChild<QDialogButtonBox *>()
        ->button(QDialogButtonBox::Save)
        ->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto saved = dialog.providers().first();
    QCOMPARE(saved.thinkingMode, QString("disabled"));
    QTemporaryDir dir;
    const auto log = dir.filePath("calls");
    qputenv("ADE_ACP_LOG", log.toUtf8());
    qputenv("ADE_ACP_SCENARIO", "thinking-off");
    AgentEngine engine;
    engine.setProvider(saved);
    engine.setHarnessContext("thinking-owner", dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("check thinking", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
    QFile file(log);
    QVERIFY(file.open(QIODevice::ReadOnly));
    bool off = false;
    for (const auto &line : file.readAll().split('\n')) {
      const auto row = QJsonDocument::fromJson(line).object();
      if (row["method"] == "session/set_config_option" &&
          row["params"].toObject()["configId"] == "thought_level")
        off = row["params"].toObject()["value"] == "none";
    }
    QVERIFY(off);
  }
  void removingOpenCodePreservesCliAccountAndDoesNotLaunch() {
    ProviderDialog dialog({provider()});
    dialog.show();
    dialog.findChild<QPushButton *>("removeProvider")->click();
    auto *prompt =
        dialog.findChild<QMessageBox *>("removeProviderConfirmation");
    QVERIFY(prompt);
    QVERIFY(prompt->informativeText().contains(
        "OpenCode sign-in and subscription remain unchanged"));
    QVERIFY(!prompt->informativeText().contains("API key"));
    QVERIFY(!dialog.findChild<OpenCodeModelDiscovery *>());
    prompt->button(QMessageBox::Yes)->click();
    QVERIFY(dialog.providers().isEmpty());
  }
  void discoveryLocksSaveAndRemoveThenCancels() {
    qputenv("ADE_ACP_SCENARIO", "models-hold");
    ProviderDialog dialog({provider()});
    dialog.show();
    connectButton(dialog)->click();
    auto *cancel = dialog.findChild<QPushButton *>("cancelOpenCodeDiscovery");
    QVERIFY(cancel);
    QTRY_VERIFY(cancel->isEnabled());
    auto *remove = dialog.findChild<QPushButton *>("removeProvider");
    QVERIFY(remove);
    QVERIFY(!remove->isEnabled());
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(!buttons->button(QDialogButtonBox::Save)->isEnabled());
    cancel->click();
    QTRY_VERIFY(connectButton(dialog)->isEnabled());
    QVERIFY(remove->isEnabled());
    QVERIFY(dialog.modelCatalogs().isEmpty());
  }
  void switchRowsRevokesDiscovery() {
    qputenv("ADE_ACP_SCENARIO", "models-hold");
    ProviderConfig api;
    api.id = "api";
    api.name = "API";
    api.model = "preserved";
    api.baseUrl = "https://example.invalid/v1";
    ProviderDialog dialog({provider(), api});
    dialog.show();
    connectButton(dialog)->click();
    auto *list = dialog.findChild<QListWidget *>();
    QVERIFY(list);
    list->setCurrentRow(1);
    QCOMPARE(dialog.findChild<QLineEdit *>("providerModel")->text(),
             QString("preserved"));
    QTRY_VERIFY(dialog.findChild<OpenCodeModelDiscovery *>() == nullptr);
    QVERIFY(dialog.modelCatalogs().isEmpty());
  }
  void observerCanDeleteDialogDuringDiscoveredModelChange() {
    auto p = provider();
    p.model = "unknown";
    QPointer<ProviderDialog> dialog = new ProviderDialog({p});
    dialog->show();
    auto *model = dialog->findChild<QLineEdit *>("providerModel");
    QVERIFY(model);
    connect(model, &QLineEdit::textChanged, dialog,
            [&] { delete dialog.data(); });
    connectButton(*dialog)->click();
    QTRY_VERIFY(!dialog);
  }
  void unavailableCliAndDiscoveryFailureRemainActionable() {
    auto p = provider();
    p.harnessExecutable = "D:/nonexistent-ade-opencode-fixture.exe";
    ProviderDialog dialog({p});
    dialog.show();
    connectButton(dialog)->click();
    QTRY_VERIFY(connectButton(dialog)->isEnabled());
    QVERIFY(dialog.modelCatalogs().isEmpty());
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QVERIFY(dialog.isVisible());
    qputenv("ADE_ACP_SCENARIO", "models-error");
    ProviderDialog errorDialog({provider()});
    errorDialog.show();
    connectButton(errorDialog)->click();
    QTRY_VERIFY(connectButton(errorDialog)->isEnabled());
    QVERIFY(errorDialog.modelCatalogs().isEmpty());
    for (auto *label : errorDialog.findChildren<QLabel *>())
      QVERIFY(!label->text().contains("SYNTHETIC_SECRET_ERROR"));
  }
};
QTEST_MAIN(OpenCodeProviderUiTest)
#include "test_opencode_provider_ui.moc"
