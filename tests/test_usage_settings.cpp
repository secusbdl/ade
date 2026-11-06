#include "ui/ProviderDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QtTest>
using namespace ade;
class UsageSettingsTest : public QObject {
  Q_OBJECT
private slots:
  void explicitContextLimitCanBeSavedAndClearedForAnotherModel() {
    ProviderConfig p{"fixture",  "Fixture", "https://fixture.invalid/v1",
                     "fake-key", "first",   "openai"};
    p.contextWindowTokens = 16000;
    ProviderDialog dialog({p});
    auto *limit = dialog.findChild<QSpinBox *>("providerContextWindow");
    auto *model = dialog.findChild<QLineEdit *>("providerModel");
    QVERIFY(limit);
    QVERIFY(model);
    QCOMPARE(limit->value(), 16000);
    model->setText("second");
    QCOMPARE(limit->value(), 0);
    limit->setValue(32000);
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.providers().first().model, "second");
    QCOMPARE(dialog.providers().first().contextWindowTokens, 32000);
  }
  void endpointAndProtocolEditsInvalidateExplicitLimit() {
    ProviderConfig p{"fixture", "Fixture", "https://fixture.invalid/v1",
                     {},        "first",   "openai"};
    p.contextWindowTokens = 16000;
    ProviderDialog dialog({p});
    auto *limit = dialog.findChild<QSpinBox *>("providerContextWindow");
    QVERIFY(limit);
    dialog.findChild<QLineEdit *>("providerBaseUrl")
        ->setText("https://different.invalid/v1");
    QCOMPARE(limit->value(), 0);
    limit->setValue(16000);
    auto combos = dialog.findChildren<QComboBox *>();
    QComboBox *protocol = nullptr;
    for (auto *c : combos)
      if (c->findData("anthropic") >= 0)
        protocol = c;
    QVERIFY(protocol);
    protocol->setCurrentIndex(protocol->findData("anthropic"));
    QCOMPARE(limit->value(), 0);
  }
};
QTEST_MAIN(UsageSettingsTest)
#include "test_usage_settings.moc"
