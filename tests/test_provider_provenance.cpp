#include "core/ProviderStore.h"
#include "ui/AppStyle.h"
#include "ui/ProviderCatalog.h"
#include "ui/SettingsPage.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray bytes(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}
ProviderConfig legacy(QString url, QString protocol = "openai") {
  ProviderConfig p;
  p.id = "fixture";
  p.name = "Custom DeepSeek";
  p.baseUrl = url;
  p.protocol = protocol;
  p.model = "synthetic-model";
  return p;
}
QLabel *mark(QWidget *card) {
  for (auto *label : card->findChildren<QLabel *>("settingsCategoryMark"))
    if (label->property("settingsMarkSize").toInt() == 36)
      return label;
  return nullptr;
}
void settle() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}
} // namespace
class ProviderProvenanceTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    initializeAppStyle(
        *qobject_cast<QApplication *>(QCoreApplication::instance()));
  }
  void cardsRequireRealTemplateOrUniqueFixedEndpoint_data() {
    QTest::addColumn<QString>("endpoint");
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<QString>("templateId");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("expectedTemplate");
    QTest::addColumn<QString>("glyph");
    const auto add = [](const char *name, const QString &endpoint,
                        const QString &protocol, const QString &provenance,
                        const QString &kind, const QString &expected,
                        const QString &glyph) {
      QTest::newRow(name) << endpoint << protocol << provenance << kind
                          << expected << glyph;
    };
    add("legacy-deepseek", "https://api.deepseek.com/v1", "openai", "", "api",
        "deepseek-api", "deepseek");
    add("legacy-unknown", "https://fixture.invalid/v1", "openai", "", "api", "",
        "brain");
    add("legacy-qwen-configurable-default",
        "https://token-plan.ap-southeast-1.maas.aliyuncs.com/compatible-mode/"
        "v1",
        "openai", "", "api", "", "brain");
    add("legacy-qwen-configurable-regional",
        "https://regional.invalid/compatible-mode/v1", "openai", "", "api", "",
        "brain");
    add("explicit-qwen-regional", "https://regional.invalid/compatible-mode/v1",
        "openai", "qwen-token-plan", "api", "qwen-token-plan", "alibaba");
    add("explicit-custom-official-url", "https://api.deepseek.com/v1", "openai",
        "custom", "api", "custom", "brain");
    add("legacy-ambiguous-minimax", "https://api.minimax.io/v1", "openai", "",
        "api", "", "brain");
    add("legacy-ambiguous-moonshot", "https://api.moonshot.ai/v1", "openai", "",
        "api", "", "brain");
    add("explicit-minimax-api", "https://api.minimax.io/v1", "openai",
        "minimax-api", "api", "minimax-api", "minimax");
    add("explicit-minimax-plan", "https://api.minimax.io/v1", "openai",
        "minimax-plan", "api", "minimax-plan", "minimax");
    add("legacy-protocol-mismatch", "https://api.deepseek.com/v1", "anthropic",
        "", "api", "", "brain");
    add("legacy-near-url", "https://api.deepseek.com/v1-extra", "openai", "",
        "api", "", "brain");
    add("explicit-stale-template", "https://fixture.invalid/v1", "openai",
        "deepseek-api", "api", "", "brain");
    add("legacy-codex", "", "codex-app-server", "", "codex", "codex", "openai");
    add("legacy-claude", "", "claude-control", "", "claude-code", "claude-code",
        "anthropic");
  }
  void cardsRequireRealTemplateOrUniqueFixedEndpoint() {
    QFETCH(QString, endpoint);
    QFETCH(QString, protocol);
    QFETCH(QString, templateId);
    QFETCH(QString, kind);
    QFETCH(QString, expectedTemplate);
    QFETCH(QString, glyph);
    QTemporaryDir temp;
    auto provider = legacy(endpoint, protocol);
    provider.templateId = templateId;
    provider.kind = kind;
    const auto path = temp.filePath("providers.json");
    QString error;
    QVERIFY2(ProviderStore::save(path, {provider}, &error), qPrintable(error));
    const auto original = bytes(path);
    SettingsPage page(temp.path());
    page.setAttribute(Qt::WA_DontShowOnScreen);
    page.resize(1280, 800);
    page.show();
    settle();
    auto *card = page.findChild<QPushButton *>("providerSettingsCard_fixture");
    QVERIFY(card);
    auto *description = card->findChild<QLabel *>("providerDescription");
    QVERIFY(description);
    QVERIFY(mark(card));
    const auto expected =
        expectedTemplate.isEmpty()
            ? (provider.isExternalHarness()
                   ? QString("External coding agent using its own configured "
                             "account.")
                   : QString("Custom model endpoint using %1.").arg(protocol))
            : ProviderCatalog::find(expectedTemplate)->description;
    QCOMPARE(description->text(), expected);
    QCOMPARE(mark(card)->property("settingsGlyph").toString(), glyph);
    QSignalSpy edit(&page, &SettingsPage::providerEditRequested);
    card->click();
    QCOMPARE(edit.size(), 1);
    QCOMPARE(edit[0][0].toString(), provider.id);
    QCOMPARE(bytes(path), original);
    page.reload();
    settle();
    QCOMPARE(bytes(path), original);
  }
  void syntheticBeforeAfterCaptures() {
    QTemporaryDir fallback;
    const auto directory = qEnvironmentVariable("ADE_PROVENANCE_CAPTURES", fallback.path());
    QVERIFY(!directory.isEmpty());
    QVERIFY(QDir().mkpath(directory));
    QTemporaryDir temp;
    auto a = legacy("https://api.deepseek.com/v1");
    a.name = "Legacy DeepSeek endpoint";
    auto b = legacy("https://fixture.invalid/v1");
    b.id = "custom";
    b.name = "Unknown custom endpoint";
    auto c = ProviderCatalog::instantiate("qwen-token-plan");
    c.id = "qwen";
    c.name = "Explicit Qwen regional template";
    c.baseUrl = "https://regional.invalid/v1";
    QVERIFY(ProviderStore::save(temp.filePath("providers.json"), {a, b, c}));
    SettingsPage page(temp.path());
    page.setAttribute(Qt::WA_DontShowOnScreen);
    page.resize(1280, 800);
    page.show();
    for (const auto *theme : {"light", "dark"}) {
      QVERIFY(page.setPreference("personalization.appearance", theme));
      settle();
      QVERIFY(page.grab().save(directory + "/providers-" + theme + ".png"));
    }
  }
};
QTEST_MAIN(ProviderProvenanceTest)
#include "test_provider_provenance.moc"
