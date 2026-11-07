#include "core/ProviderStore.h"
#include "ui/AppStyle.h"
#include "ui/ProviderCatalog.h"
#include "ui/ProviderModelsDialog.h"
#include "ui/SettingsPage.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
using namespace ade;
namespace {
class ResizeObserver : public QObject {
public:
  std::function<void()> callback;
  bool eventFilter(QObject *, QEvent *e) override {
    if (e->type() == QEvent::Resize && callback) {
      auto fn = std::move(callback);
      fn();
      return true;
    }
    return false;
  }
};
bool seed(const QString &path) {
  auto p = ProviderCatalog::instantiate("deepseek-api");
  p.id = "fixture-deepseek";
  p.model = "raw-model-private-id";
  auto p2 = p;
  p2.id = "fixture-second";
  p2.name = "Parity DeepSeek 01";
  auto p3 = ProviderCatalog::instantiate("claude-code");
  p3.id = "fixture-claude";
  return ProviderStore::save(path + "/providers.json", {p, p2, p3});
}
void settle() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
  QTest::qWait(10);
}
void show(SettingsPage &page, QSize size = {1707, 995}) {
  page.setAttribute(Qt::WA_DontShowOnScreen);
  page.resize(size);
  page.show();
  settle();
}
QPushButton *button(SettingsPage &p, const QString &name) {
  return p.findChild<QPushButton *>(name);
}
void plugin(SettingsPage &p) {
  p.selectSection("plugins");
  settle();
  for (auto *b : p.findChildren<QPushButton *>())
    if (b->accessibleName() == "Configure GitHub") {
      b->click();
      break;
    }
  settle();
}
} // namespace
class SettingsPresentationTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    initializeAppStyle(
        *qobject_cast<QApplication *>(QCoreApplication::instance()));
  }
  void defaultNavigationAndContentAlignment() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p);
    QCOMPARE(p.findChild<QWidget *>("settingsNavigation")->width(), 250);
    auto *c = p.findChild<QWidget *>("settingsContent");
    QCOMPARE(c->width(), 768);
    auto *s = p.findChild<QScrollArea *>("settingsScroll");
    QVERIFY(qAbs(c->mapTo(s->viewport(), QPoint()).x() -
                 (s->viewport()->width() - c->width()) / 2) <= 1);
  }
  void providerDescriptionAndWholeCardNavigation() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p);
    auto *card = button(p, "providerSettingsCard_fixture-deepseek");
    QVERIFY(card);
    auto *desc = card->findChild<QLabel *>("providerDescription");
    QVERIFY(desc);
    QCOMPARE(desc->text(), ProviderCatalog::find("deepseek-api")->description);
    QSignalSpy edit(&p, &SettingsPage::providerEditRequested);
    QTest::mouseClick(card, Qt::LeftButton, Qt::NoModifier,
                      QPoint(card->width() / 2, card->height() / 2));
    QCOMPARE(edit.size(), 1);
    QCOMPARE(edit[0][0].toString(), QString("fixture-deepseek"));
    card->setFocus();
    QTest::keyClick(card, Qt::Key_Space);
    QCOMPARE(edit.size(), 2);
  }
  void providerActionsShareWideRow() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p);
    auto *add = button(p, "settingsAddProvider");
    auto *import = button(p, "settingsImportProviderKeys");
    QVERIFY(add && import);
    QCOMPARE(add->mapTo(&p, QPoint()).y(), import->mapTo(&p, QPoint()).y());
    QSignalSpy imports(&p, &SettingsPage::providerKeyImportRequested);
    import->click();
    QCOMPARE(imports.size(), 1);
  }
  void modelsActionHasIndependentBinding() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p);
    QSignalSpy edit(&p, &SettingsPage::providerEditRequested);
    button(p, "manageProviderModels_fixture-second")->click();
    settle();
    auto *dialog = p.findChild<ProviderModelsDialog *>();
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QCOMPARE(edit.size(), 0);
    dialog->reject();
    settle();
  }
  void shortWindowNavigationScrolls() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p, {800, 280});
    auto *nav = p.findChild<QScrollArea *>("settingsNavigationScroll");
    QVERIFY(nav);
    QVERIFY(nav->verticalScrollBar()->maximum() > 0);
    auto *about = button(p, "settingsNav_about");
    nav->ensureWidgetVisible(about);
    settle();
    QVERIFY(nav->viewport()->rect().intersects(
        QRect(about->mapTo(nav->viewport(), QPoint()), about->size())));
    about->click();
    QCOMPARE(p.currentSection(), QString("about"));
  }
  void narrowNavigationLeavesUsableContent() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p, {350, 600});
    QCOMPARE(p.width(), 350);
    QVERIFY(p.findChild<QWidget *>("settingsNavigation")->width() <= 64);
    auto *c = p.findChild<QWidget *>("settingsContent");
    QVERIFY(c->width() >= 230);
    auto *nav = button(p, "settingsNav_plugins");
    QCOMPARE(nav->accessibleName(), QString("Plugins"));
    QVERIFY(!nav->toolTip().isEmpty());
    nav->click();
    settle();
    QCOMPARE(p.currentSection(), QString("plugins"));
    QCOMPARE(p.findChild<QScrollArea *>("settingsScroll")
                 ->horizontalScrollBar()
                 ->maximum(),
             0);
  }
  void scaledContentAndNavigation() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p, {1900, 1000});
    QVERIFY(p.setPreference("personalization.uiZoom", 130));
    settle();
    QCOMPARE(p.findChild<QWidget *>("settingsNavigation")->width(), 325);
    QCOMPARE(p.findChild<QWidget *>("settingsContent")->maximumWidth(), 998);
    p.resize(450, 600);
    settle();
    QVERIFY(p.findChild<QWidget *>("settingsContent")->width() >= 260);
  }
  void compactPluginDetailKeepsActions() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p);
    plugin(p);
    auto *title = p.findChild<QLabel *>("settingsTitle");
    QVERIFY(title);
    QCOMPARE(title->text(), QString("GitHub"));
    auto *enabled = p.findChild<QCheckBox *>("pluginEnabled_github");
    QVERIFY(enabled);
    auto *credential = p.findChild<QLineEdit *>("pluginCredential");
    QVERIFY(credential);
    QVERIFY(credential->mapTo(&p, QPoint()).y() < 400);
    QCOMPARE(button(p, "savePluginCredential")->mapTo(&p, QPoint()).y(),
             button(p, "checkPluginConnection")->mapTo(&p, QPoint()).y());
    QVERIFY(button(p, "allPlugins"));
    QCOMPARE(credential->echoMode(), QLineEdit::Password);
    button(p, "allPlugins")->click();
    settle();
    QCOMPARE(p.findChildren<QPushButton *>("pluginCard").size(), 5);
  }
  void allProviderCardChildrenFit() {
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    for (const auto size : {QSize(1707, 995), QSize(350, 650)}) {
      show(p, size);
      for (auto *card : p.findChildren<QPushButton *>()) {
        if (!card->objectName().startsWith("providerSettingsCard_"))
          continue;
        QVERIFY(card->height() >= 60);
        for (auto *label : card->findChildren<QLabel *>()) {
          QVERIFY2(card->rect().contains(
                       QRect(label->mapTo(card, QPoint()), label->size())),
                   qPrintable(label->objectName()));
          if (!label->text().isEmpty())
            QVERIFY(label->height() >= label->fontMetrics().height());
        }
      }
    }
  }
  void darkStatusRetainsContrast() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    QVERIFY(p.setPreference("personalization.appearance", "dark"));
    show(p);
    p.selectSection("plugins");
    settle();
    auto *status = p.findChild<QLabel *>("pluginCardStatus_github");
    QVERIFY(status);
    QVERIFY(status->palette().color(QPalette::WindowText).lightness() > 150);
    plugin(p);
    auto *connection = p.findChild<QLabel *>("pluginConnectionStatus");
    QVERIFY(connection);
    QVERIFY(connection->palette().color(QPalette::WindowText).lightness() >
            150);
  }
  void navigationResizeCanDeferOwnerDeletion() {
    QTemporaryDir d;
    auto *p = new SettingsPage(d.path());
    show(*p);
    ResizeObserver observer;
    QPointer<SettingsPage> alive = p;
    p->findChild<QWidget *>("settingsNavigation")
        ->installEventFilter(&observer);
    observer.callback = [p] { p->deleteLater(); };
    p->setPreference("personalization.uiZoom", 130);
    settle();
    QVERIFY(!alive);
  }
  void navigationResizeCanReplaceSection() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p);
    ResizeObserver observer;
    p.findChild<QWidget *>("settingsNavigation")->installEventFilter(&observer);
    observer.callback = [&] { p.selectSection("plugins"); };
    QVERIFY(p.setPreference("personalization.uiZoom", 130));
    settle();
    QCOMPARE(p.currentSection(), QString("plugins"));
    QCOMPARE(p.findChild<QLabel *>("settingsTitle")->text(),
             QString("Plugins"));
    QCOMPARE(p.findChild<QWidget *>("settingsContent")->maximumWidth(), 998);
  }
  void navigationResizeRetainsNewerZoom() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p);
    ResizeObserver observer;
    p.findChild<QWidget *>("settingsNavigation")->installEventFilter(&observer);
    observer.callback = [&] {
      QVERIFY(p.setPreference("personalization.uiZoom", 70));
    };
    QVERIFY(p.setPreference("personalization.uiZoom", 130));
    settle();
    QCOMPARE(p.preference("personalization.uiZoom").toInt(), 70);
    QCOMPARE(p.findChild<QWidget *>("settingsContent")->maximumWidth(), 538);
    QCOMPARE(button(p, "settingsNav_plugins")->height(), 22);
  }
  void resizeKeepsUnsavedPluginDraft() {
    QTemporaryDir d;
    SettingsPage p(d.path());
    show(p);
    plugin(p);
    auto *draft = p.findChild<QLineEdit *>("pluginCredential");
    QVERIFY(draft);
    draft->setText("synthetic_unsaved_fixture");
    p.resize(350, 500);
    settle();
    p.resize(1707, 995);
    settle();
    QCOMPARE(p.findChild<QLineEdit *>("pluginCredential"), draft);
    QCOMPARE(draft->text(), QString("synthetic_unsaved_fixture"));
    QVERIFY(!QFileInfo::exists(d.path() + "/plugin-credentials.json"));
  }
  void captureEvidence() {
    const QString out = qEnvironmentVariable("ADE_SETTINGS_RENDER_DIR");
    if (out.isEmpty())
      QSKIP("Optional private render output unset");
    QVERIFY(QDir().mkpath(out));
    QTemporaryDir d;
    QVERIFY(seed(d.path()));
    SettingsPage p(d.path());
    show(p);
    for (const QString theme : {"light", "dark"}) {
      QVERIFY(p.setPreference("personalization.appearance", theme));
      for (const QSize size : {QSize(1707, 995), QSize(350, 650)}) {
        p.resize(size);
        for (const QString section :
             {"models-providers", "plugins", "github"}) {
          if (section == "github")
            plugin(p);
          else
            p.selectSection(section);
          settle();
          QVERIFY(p.grab().save(out + "/" + section + "-" + theme + "-" +
                                QString::number(size.width()) + ".png"));
        }
      }
    }
  }
};
QTEST_MAIN(SettingsPresentationTest)
#include "test_settings_presentation.moc"
