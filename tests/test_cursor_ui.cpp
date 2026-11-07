#include "core/ProviderStore.h"
#include "ui/AddProviderDialog.h"
#include "ui/ProviderCatalog.h"
#include "ui/ProviderDialog.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class CursorUiTest : public QObject {
  Q_OBJECT
  ProviderConfig provider() {
    auto p = ProviderCatalog::instantiate("cursor");
    p.harnessExecutable = QDir(QCoreApplication::applicationDirPath())
                              .filePath("cursor_fixture.exe");
    return p;
  }
private slots:
  void cleanup() { qunsetenv("ADE_CURSOR_LOG"); }
  void pickerCreatesNativeOptionalCursor() {
    ProviderDialog d({});
    d.show();
    d.startAddProvider();
    auto *picker = d.findChild<AddProviderDialog *>();
    QVERIFY(picker);
    auto *card = picker->findChild<QPushButton *>("providerChoice_cursor");
    QVERIFY(card);
    card->click();
    QTRY_COMPARE(d.providers().size(), 1);
    const auto p = d.providers().first();
    QVERIFY(p.isCursorHarness());
    QCOMPARE(p.model, QString("default"));
    QCOMPARE(p.protocol, QString("cursor-acp"));
    QVERIFY(p.apiKey.isEmpty());
    QVERIFY(p.baseUrl.isEmpty());
    QVERIFY(d.findChild<QLineEdit *>("cursorExecutable"));
    QVERIFY(!d.findChild<QLineEdit *>("providerApiKey")->isVisible());
    QVERIFY(!d.findChild<QPushButton *>("fetchProviderModels")->isEnabled());
  }
  void checkOnlyInspectsProgramFiles() {
    QTemporaryDir t;
    const auto log = t.filePath("calls");
    qputenv("ADE_CURSOR_LOG", log.toUtf8());
    ProviderDialog d({provider()});
    d.show();
    auto *button = d.findChild<QPushButton *>("checkCursorInstallation");
    QVERIFY(button);
    button->click();
    QVERIFY(!QFileInfo::exists(log));
    bool found = false;
    for (auto *label : d.findChildren<QLabel *>())
      found |= label->text().contains("No process, login");
    QVERIFY(found);
  }
  void savePreservesExactCursorInstance() {
    QTemporaryDir t;
    const auto original = provider();
    ProviderDialog d({original});
    d.show();
    auto *buttons = d.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    buttons->button(QDialogButtonBox::Save)->click();
    QCOMPARE(d.result(), int(QDialog::Accepted));
    const auto p = d.providers().first();
    QCOMPARE(p.id, original.id);
    QCOMPARE(p.harnessExecutable, original.harnessExecutable);
    QCOMPARE(p.protocol, QString("cursor-acp"));
    QString error;
    QVERIFY2(ProviderStore::save(t.filePath("providers"), {p}, &error),
             qPrintable(error));
    QCOMPARE(ProviderStore::load(t.filePath("providers"), &error).first().kind,
             QString("cursor"));
  }
  void invalidExecutableDoesNotSave() {
    auto p = provider();
    p.harnessExecutable = "D:/missing-cursor-agent.exe";
    ProviderDialog d({p});
    d.show();
    d.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    QVERIFY(d.isVisible());
    QCOMPARE(d.result(), int(QDialog::Rejected));
  }
  void switchBackToHttpRestoresFetchAndRows() {
    auto http = ProviderCatalog::instantiate("openai-api");
    http.id = "http-instance";
    ProviderDialog d({provider(), http});
    d.show();
    QVERIFY(!d.findChild<QPushButton *>("fetchProviderModels")->isEnabled());
    QVERIFY(d.selectProviderId(http.id));
    QVERIFY(d.findChild<QPushButton *>("fetchProviderModels")->isEnabled());
    QVERIFY(d.findChild<QLineEdit *>("providerApiKey")->isVisible());
  }
};
QTEST_MAIN(CursorUiTest)
#include "test_cursor_ui.moc"
