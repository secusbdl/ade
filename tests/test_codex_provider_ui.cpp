#include "ui/ModelSelection.h"
#include "ui/ProviderDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CodexProviderUiTest : public QObject {
  Q_OBJECT
  QString fixture() const {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath("harness_fixture.exe");
  }
  ProviderConfig provider() const {
    ProviderConfig p;
    p.id = "codex";
    p.name = "Codex";
    p.kind = "codex";
    p.protocol = "codex-app-server";
    p.harnessExecutable = fixture();
    return p;
  }
private slots:
  void cleanup() { qunsetenv("ADE_HARNESS_FIXTURE_SCENARIO"); }
  void connectDiscoversWithoutApiKey() {
    ProviderDialog dialog({provider()});
    dialog.show();
    auto *connect = dialog.findChild<QPushButton *>("connectCodexSubscription");
    QVERIFY(connect);
    auto *key = dialog.findChild<QLineEdit *>("providerApiKey");
    QVERIFY(key);
    QVERIFY(!key->isVisible());
    QTest::mouseClick(connect, Qt::LeftButton);
    auto *model = dialog.findChild<QLineEdit *>("providerModel");
    QVERIFY(model);
    QTRY_COMPARE(model->text(), QString("model-a"));
    QTRY_VERIFY(connect->isEnabled());
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto p = dialog.providers().first();
    QVERIFY(p.isCodexHarness());
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(p.baseUrl.isEmpty());
    const auto selections = availableModelSelections(
        dialog.providers(), dialog.modelCatalogs(), {});
    QCOMPARE(selections.size(), 2);
  }
  void savedSubscriptionWithoutEndpointStillSelectable() {
    auto p = provider();
    p.model = "model-a";
    ProviderDialog dialog({p});
    dialog.show();
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    ProviderConfig resolved;
    ModelSelection selection{p.id, p.model, {}, {}, {}};
    QVERIFY(selection.resolve(dialog.providers(), &resolved));
    QVERIFY(resolved.isCodexHarness());
    QVERIFY(resolved.baseUrl.isEmpty());
  }
  void switchingRowsCancelsOwnedDiscovery() {
    auto p = provider();
    p.model = "model-a";
    ProviderConfig api;
    api.id = "api";
    api.name = "API";
    api.baseUrl = "https://example.test/v1";
    api.model = "x";
    ProviderDialog dialog({p, api});
    dialog.show();
    auto *connect = dialog.findChild<QPushButton *>("connectCodexSubscription");
    QVERIFY(connect);
    QTest::mouseClick(connect, Qt::LeftButton);
    auto *list = dialog.findChild<QListWidget *>();
    QVERIFY(list);
    list->setCurrentRow(1);
    QCOMPARE(dialog.findChild<QLineEdit *>("providerName")->text(),
             QString("API"));
    QTest::qWait(100);
    QCOMPARE(dialog.findChild<QLineEdit *>("providerModel")->text(),
             QString("x"));
  }
  void catalogBelongsToExecutable() {
    auto p = provider();
    p.model = "a";
    const auto catalog = cleanModelCatalog(
        p, {QJsonObject{{"id", "b"},
                        {"name", "B"},
                        {"reasoningEfforts", QJsonArray{"low", "high"}}}});
    QCOMPARE(catalog["models"]
                 .toArray()
                 .first()
                 .toObject()["reasoningEfforts"]
                 .toArray()
                 .size(),
             2);
    QCOMPARE(availableModelSelections({p}, {{p.id, catalog}}, {}).size(), 2);
    p.harnessExecutable += "different";
    QCOMPARE(availableModelSelections({p}, {{p.id, catalog}}, {}).size(), 1);
  }
};
QTEST_MAIN(CodexProviderUiTest)
#include "test_codex_provider_ui.moc"
