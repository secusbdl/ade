#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "ui/AddProviderDialog.h"
#include "ui/ModelSelection.h"
#include "ui/ProviderCatalog.h"
#include "ui/ProviderDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ClaudeProviderUiTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() const {
    auto p = ProviderCatalog::instantiate("claude-code");
    p.id = "claude-fixture";
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("claude_fixture.exe");
    return p;
  }
  QPushButton *connectButton(ProviderDialog &d) const {
    return d.findChild<QPushButton *>("connectClaude");
  }
private slots:
  void initTestCase() {
    const auto font = QFontDatabase::addApplicationFont(
        QString::fromUtf8(ADE_TEST_FONT_PATH));
    QVERIFY(font >= 0);
    const auto families = QFontDatabase::applicationFontFamilies(font);
    QVERIFY(!families.isEmpty());
    QFont body(families.first());
    body.setPixelSize(13);
    qApp->setFont(body);
    qApp->setStyle("Fusion");
  }
  void cleanup() {
    qunsetenv("ADE_CLAUDE_FIXTURE");
    qunsetenv("ADE_CLAUDE_LOG");
    qunsetenv("ADE_CLAUDE_ARGS_LOG");
  }
  void pickerCreatesRealExternalKindWithNoKeyOrUrl() {
    ProviderDialog d({});
    d.show();
    d.startAddProvider();
    auto *picker = d.findChild<AddProviderDialog *>();
    QVERIFY(picker);
    auto *card = picker->findChild<QPushButton *>("providerChoice_claude-code");
    QVERIFY(card);
    card->click();
    QTRY_COMPARE(d.providers().size(), 1);
    const auto p = d.providers().first();
    QVERIFY(p.isClaudeHarness());
    QCOMPARE(p.protocol, QString("claude-control"));
    QCOMPARE(p.model, QString("default"));
    QVERIFY(p.baseUrl.isEmpty());
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(connectButton(d));
    QVERIFY(!d.findChild<QLineEdit *>("providerApiKey")->isVisible());
    QVERIFY(!d.findChild<QLineEdit *>("providerBaseUrl")->isVisible());
  }
  void discoveryAndSavedConfigUseInstalledNativeProcess() {
    auto p = provider();
    ProviderDialog d({p});
    d.show();
    auto *b = connectButton(d);
    QVERIFY(b);
    b->click();
    QTRY_VERIFY(d.modelCatalogs().contains(p.id));
    QTRY_VERIFY(b->isEnabled());
    const auto catalog = d.modelCatalogs().value(p.id).toObject();
    QCOMPARE(catalog.value("kind").toString(), QString("claude-code"));
    QCOMPARE(catalog.value("models").toArray().size(), 1);
    QCOMPARE(
        catalog.value("models").toArray()[0].toObject().value("id").toString(),
        QString("fixture-model"));
    bool honest = false;
    for (auto *label : d.findChildren<QLabel *>())
      honest |= label->text().contains("did not verify sign-in");
    QVERIFY(honest);
    d.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    p = d.providers().first();
    QVERIFY(p.isClaudeHarness());
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(p.baseUrl.isEmpty());
    QTemporaryDir dir;
    QString error;
    QVERIFY(ProviderStore::save(dir.filePath("providers.json"), {p}, &error));
    const auto loaded =
        ProviderStore::load(dir.filePath("providers.json"), &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(loaded.size(), 1);
    QVERIFY(loaded[0].isClaudeHarness());
    AgentEngine engine;
    engine.setProvider(loaded[0]);
    engine.setHarnessContext("saved-owner", dir.path());
    QSignalSpy done(&engine, &AgentEngine::finished);
    QVERIFY(engine.submitWithAttachments("Native saved configuration", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(engine.lastTurnSucceeded());
  }
  void thinkingSettingsSurviveSaveAndReachOfficialCliFlags() {
    auto p = provider();
    p.thinkingMode = "disabled";
    p.reasoningEffort = "low";
    ProviderDialog d({p});
    d.show();
    auto *thinking = d.findChild<QComboBox *>("providerThinking");
    QVERIFY(thinking);
    QVERIFY(thinking->isVisible());
    QVERIFY(thinking->isEnabled());
    QCOMPARE(thinking->currentData().toString(), QString("disabled"));
    d.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    p = d.providers().first();
    QCOMPARE(p.thinkingMode, QString("disabled"));
    QTemporaryDir dir;
    const auto log = dir.filePath("args");
    qputenv("ADE_CLAUDE_ARGS_LOG", log.toUtf8());
    AgentEngine e;
    e.setProvider(p);
    e.setHarnessContext("owner", dir.path());
    QSignalSpy done(&e, &AgentEngine::finished);
    QVERIFY(e.submitWithAttachments("Thinking", {}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(e.lastTurnSucceeded());
    QFile f(log);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto args = QJsonDocument::fromJson(f.readAll().trimmed()).array();
    QVERIFY(args.contains("--thinking"));
    QVERIFY(args.contains("disabled"));
    QVERIFY(args.contains("--effort"));
    QVERIFY(args.contains("low"));
  }
  void discoveryLocksSaveRemoveAndCancelRestoresControls() {
    qputenv("ADE_CLAUDE_FIXTURE", "hold");
    auto p = provider();
    ProviderDialog d({p});
    d.show();
    connectButton(d)->click();
    QVERIFY(!d.findChild<QPushButton *>("removeProvider")->isEnabled());
    QVERIFY(!d.findChild<QDialogButtonBox *>()
                 ->button(QDialogButtonBox::Save)
                 ->isEnabled());
    QVERIFY(!d.findChild<QLineEdit *>("claudeExecutable")->isEnabled());
    d.findChild<QPushButton *>("cancelClaudeDiscovery")->click();
    QVERIFY(connectButton(d)->isEnabled());
    QVERIFY(d.findChild<QPushButton *>("removeProvider")->isEnabled());
    QVERIFY(d.modelCatalogs().isEmpty());
    qunsetenv("ADE_CLAUDE_FIXTURE");
    connectButton(d)->click();
    QTRY_VERIFY(d.modelCatalogs().contains(p.id));
  }
  void completionReentryMayDeleteDialog() {
    QPointer<ProviderDialog> d = new ProviderDialog({provider()});
    d->show();
    auto *model = d->findChild<QLineEdit *>("providerModel");
    connect(model, &QLineEdit::textChanged, d, [&] { delete d.data(); });
    connectButton(*d)->click();
    QTRY_VERIFY(!d);
  }
  void removalDescribesCliOwnershipAndRetainsSignIn() {
    ProviderDialog d({provider()});
    d.show();
    d.findChild<QPushButton *>("removeProvider")->click();
    auto *prompt = d.findChild<QMessageBox *>();
    QVERIFY(prompt);
    QVERIFY(prompt->informativeText().contains("Claude Code"));
    QVERIFY(prompt->informativeText().contains(
        "sign-in and subscription remain unchanged"));
    QVERIFY(!prompt->informativeText().contains("API key"));
    prompt->button(QMessageBox::Yes)->click();
    QTRY_VERIFY(d.providers().isEmpty());
  }
  void hiddenDiscoveredModelIsNotChosenAsNewDefault() {
    auto p = provider();
    p.disabledModelIds = {"fixture-model"};
    ProviderDialog d({p});
    d.show();
    connectButton(d)->click();
    QTRY_VERIFY(d.modelCatalogs().contains(p.id));
    QCOMPARE(d.findChild<QLineEdit *>("providerModel")->text(),
             QString("default"));
    QCOMPARE(d.modelCatalogs()
                 .value(p.id)
                 .toObject()
                 .value("models")
                 .toArray()
                 .size(),
             1);
  }
  void nativeFormProducesBoundedRepresentativeCapture() {
    ProviderDialog d({provider()});
    d.resize(880, 680);
    d.show();
    QTest::qWait(30);
    QVERIFY(d.findChild<QPushButton *>("connectClaude")
                ->visibleRegion()
                .boundingRect()
                .height() > 0);
    const auto directory = qEnvironmentVariable("ADE_CLAUDE_CAPTURE_DIR");
    if (!directory.isEmpty()) {
      QVERIFY(QDir().mkpath(directory));
      QVERIFY(
          d.grab().save(QDir(directory).filePath("claude-provider-light.png")));
      const auto original = qApp->palette();
      auto dark = original;
      dark.setColor(QPalette::Window, QColor("#202020"));
      dark.setColor(QPalette::WindowText, QColor("#eeeeee"));
      dark.setColor(QPalette::Base, QColor("#181818"));
      dark.setColor(QPalette::Text, QColor("#eeeeee"));
      dark.setColor(QPalette::Button, QColor("#303030"));
      dark.setColor(QPalette::ButtonText, QColor("#eeeeee"));
      dark.setColor(QPalette::Disabled, QPalette::Text, QColor("#999999"));
      dark.setColor(QPalette::Disabled, QPalette::ButtonText,
                    QColor("#999999"));
      qApp->setPalette(dark);
      QTest::qWait(20);
      QVERIFY(
          d.grab().save(QDir(directory).filePath("claude-provider-dark.png")));
      qApp->setPalette(original);
    }
  }
};
QTEST_MAIN(ClaudeProviderUiTest)
#include "test_claude_provider_ui.moc"
