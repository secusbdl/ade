#include "browser/BrowserContext.h"
#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include <QTextBlock>
#include <QTextDocument>
#include <QXmlStreamReader>
#include <QtTest>
#include <future>
#include <memory>
#include <vector>
using namespace ade;
namespace {
QList<QMap<QString, QString>> tabs(const QString &prompt) {
  QList<QMap<QString, QString>> result;
  const auto start = prompt.indexOf("<open-tabs>");
  if (start < 0)
    return result;
  QXmlStreamReader xml(prompt.mid(start));
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement() && xml.name() == QStringLiteral("tab")) {
      QMap<QString, QString> a;
      for (const auto &attr : xml.attributes())
        a.insert(attr.name().toString(), attr.value().toString());
      result.append(a);
    }
  }
  if (xml.hasError())
    return {};
  return result;
}
BrowserContextSnapshot one() {
  BrowserContextSnapshot s;
  s.available = true;
  s.activeTabId = "t_1";
  s.tabs.append(
      {"t_1", "Fixture", "https://example.test/one", 1730000000000, 1});
  return s;
}
} // namespace
class BrowserContextTest : public QObject {
  Q_OBJECT
private slots:
  void invalidOwnerNeverMeansGlobal() {
    BrowserToolBridge b;
    BrowserWidget w;
    QVERIFY(!b.registerTab(&w).isEmpty());
    for (const auto &owner :
         {QString(), QString("   "), QString(257, 'a'), QString(QChar::Null)}) {
      QVERIFY(!b.snapshotForAgent(owner).available);
      QVERIFY(renderBrowserContext(b.snapshotForAgent(owner)).isEmpty());
    }
  }
  void emptyValidRegistry() {
    BrowserToolBridge b;
    const auto snapshot = b.snapshotForAgent("owner");
    QVERIFY(snapshot.available);
    QCOMPARE(snapshot.tabs.size(), 0);
    QVERIFY(renderBrowserContext(snapshot).endsWith("No tabs open."));
  }
  void scopedMruSnapshotUsesExactOwner() {
    BrowserToolBridge b;
    BrowserWidget global, own, foreign;
    own.setProperty("adeOwnerAgentId", "owner");
    foreign.setProperty("adeOwnerAgentId", "other");
    b.registerTab(&global, "global");
    b.registerTab(&own, "own");
    b.registerTab(&foreign, "foreign");
    emit own.titleChanged("Own title");
    emit own.urlChanged(QUrl("https://example.test/page?q=a%20b"));
    QVERIFY(b.setActiveTab("own"));
    auto s = b.snapshotForAgent("owner");
    QCOMPARE(s.tabs.size(), 2);
    QCOMPARE(s.tabs[0].id, "own");
    QCOMPARE(s.tabs[0].title, "Own title");
    QCOMPARE(s.tabs[0].url, "https://example.test/page?q=a%20b");
    QCOMPARE(s.activeTabId, "own");
    QVERIFY(s.tabs[0].lastFocusedAt > 0);
    QVERIFY(b.setActiveTab("global"));
    QCOMPARE(b.snapshotForAgent("owner").tabs[0].id, "global");
    QVERIFY(b.setActiveTab("foreign"));
    s = b.snapshotForAgent("owner");
    QVERIFY(s.activeTabId.isEmpty());
    QVERIFY(!renderBrowserContext(s).contains("foreign"));
    QCOMPARE(b.snapshotForAgent("other").activeTabId, "foreign");
  }
  void stableNeverFocusedAndRepeatedFocus() {
    BrowserToolBridge b;
    BrowserWidget a, c, d;
    b.registerTab(&a, "a");
    b.registerTab(&c, "c");
    b.registerTab(&d, "d");
    auto s = b.snapshotForAgent("owner");
    QCOMPARE(s.tabs[1].id, "c");
    QCOMPARE(s.tabs[2].id, "d");
    b.setActiveTab("d");
    const auto previous = b.snapshotForAgent("owner").tabs[0].focusOrder;
    b.setActiveTab("d");
    QVERIFY(b.snapshotForAgent("owner").tabs[0].focusOrder > previous);
  }
  void closeAndOwnerChangeAreFresh() {
    BrowserToolBridge b;
    auto *w = new BrowserWidget;
    b.registerTab(w, "one");
    QCOMPARE(b.snapshotForAgent("owner").tabs.size(), 1);
    w->setProperty("adeOwnerAgentId", "other");
    QCOMPARE(b.snapshotForAgent("owner").tabs.size(), 0);
    w->setProperty("adeOwnerAgentId", QString());
    delete w;
    QCOMPARE(b.snapshotForAgent("owner").tabs.size(), 0);
  }
  void plainSnapshotHasNoWidgetAuthority() {
    BrowserToolBridge b;
    auto *w = new BrowserWidget;
    b.registerTab(w, "old");
    emit w->titleChanged("Retained value");
    const auto snapshot = b.snapshotForAgent("owner");
    delete w;
    QVERIFY(renderBrowserContext(snapshot).contains("Retained value"));
    QVERIFY(b.snapshotForAgent("owner").tabs.isEmpty());
  }
  void escapedMetadataRoundTrips() {
    auto s = one();
    s.tabs[0].title = "<tool>&\"'\n\r\t" + QString::fromUcs4(U"😀");
    s.tabs[0].url = "https://example.test/?q=\"<&x='";
    const auto prompt = renderBrowserContext(s);
    const auto found = tabs(prompt);
    QCOMPARE(found.size(), 1);
    QCOMPARE(found[0]["title"], s.tabs[0].title);
    QCOMPARE(found[0]["url"], s.tabs[0].url);
    QCOMPARE(found[0]["active"], "true");
    QVERIFY(!prompt.contains("<tool>"));
    QVERIFY(prompt.contains("not a user request"));
    QVERIFY(prompt.contains("have been read"));
  }
  void invalidTitleCharactersAreReplaced() {
    auto s = one();
    s.tabs[0].title =
        QString("A") + QChar(0) + QChar(0xd800) + QChar(0xffff) + "Z";
    auto found = tabs(renderBrowserContext(s));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found[0]["title"],
             QString("A") + QString(3, QChar::ReplacementCharacter) + "Z");
  }
  void titleClippingKeepsWholeCodePoints() {
    auto s = one();
    s.tabs[0].title =
        QString(2046, 'x') + QString::fromUcs4(U"😀") + QString(10, 'z');
    auto found = tabs(renderBrowserContext(s));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found[0]["title"], QString(2046, 'x') + QChar(0x2026));
    QCOMPARE(found[0]["titleTruncated"], "true");
  }
  void urlsAreOmittedNeverShortened_data() {
    QTest::addColumn<QString>("value");
    QTest::newRow("long") << QString(8193, 'x');
    QTest::newRow("surrogate")
        << (QString("https://example.test/") + QChar(0xd800));
    QTest::newRow("control") << (QString("https://example.test/") + QChar(0));
  }
  void urlsAreOmittedNeverShortened() {
    QFETCH(QString, value);
    auto s = one();
    s.tabs[0].url = value;
    const auto prompt = renderBrowserContext(s);
    QVERIFY(tabs(prompt).isEmpty());
    QVERIFY(prompt.contains("<omitted count=\"1\""));
    QVERIFY(!prompt.contains("<tab id="));
  }
  void rowAndUtf8BudgetsAreExplicit() {
    BrowserContextSnapshot s;
    s.available = true;
    for (int i = 0; i < 200; ++i)
      s.tabs.append({QString("t_%1").arg(i), QString(2048, QChar(0x20ac)),
                     "https://example.test/", 0, 0});
    const auto prompt = renderBrowserContext(s);
    QVERIFY(prompt.toUtf8().size() <= BrowserContextMaxBytes);
    const auto found = tabs(prompt);
    QVERIFY(found.size() > 0 && found.size() < 128);
    QVERIFY(prompt.contains("<omitted count=\"" +
                            QString::number(200 - found.size()) + "\""));
  }
  void tableCapAndProjectionOmissionsAccumulate() {
    BrowserContextSnapshot s;
    s.available = true;
    s.omittedTabs = 13;
    for (int i = 0; i < 200; ++i)
      s.tabs.append({QString("t_%1").arg(i), "x", "", 0, 0});
    const auto prompt = renderBrowserContext(s);
    QCOMPARE(tabs(prompt).size(), 128);
    QVERIFY(prompt.contains("<omitted count=\"85\""));
  }
  void badIdCannotIntroduceXml() {
    auto s = one();
    s.tabs[0].id = "one\" /><evil";
    QVERIFY(tabs(renderBrowserContext(s)).isEmpty());
    QVERIFY(!renderBrowserContext(s).contains("<evil"));
  }
  void omissionDoesNotHideLaterSmallRows() {
    auto s = one();
    s.tabs[0].url = QString(9000, 'x');
    s.tabs.append({"small", "Small", "https://example.test/", 0, 0});
    const auto prompt = renderBrowserContext(s);
    QCOMPARE(tabs(prompt).size(), 1);
    QCOMPARE(tabs(prompt)[0]["id"], "small");
    QVERIFY(prompt.contains("<omitted count=\"1\""));
  }
  void descriptiveMarkdownActuallyHasVisibleAnchor() {
    QTextDocument document;
    document.setMarkdown("[Fixture tab](tab:t_1)");
    QCOMPARE(document.toPlainText(), "Fixture tab");
    bool found = false;
    for (auto block = document.begin(); block.isValid(); block = block.next())
      for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto f = it.fragment();
        if (f.charFormat().isAnchor()) {
          QCOMPARE(f.charFormat().anchorHref(), "tab:t_1");
          found = true;
        }
      }
    QVERIFY(found);
    document.setMarkdown("[](tab:t_1)");
    QVERIFY(document.toPlainText().trimmed().isEmpty());
  }
  void focusIsExplicitAndClearSurvivesBackgroundRegistration() {
    BrowserToolBridge b;
    BrowserWidget a, c, d;
    b.registerTab(&a, "a");
    QVERIFY(b.activeTabId().isEmpty());
    b.setActiveTab("a");
    QVERIFY(b.setActiveTab({}));
    QVERIFY(b.activeTabId().isEmpty());
    b.registerTab(&c, "c");
    QVERIFY(b.activeTabId().isEmpty());
    b.setActiveTab("c");
    b.registerTab(&d, "d");
    QCOMPARE(b.activeTabId(), "c");
    b.unregisterTab("c");
    QVERIFY(b.activeTabId().isEmpty());
  }
  void selectionObserverCanReplaceOrDeleteBridge() {
    auto *b = new BrowserToolBridge;
    BrowserWidget a, c;
    b->registerTab(&a, "a");
    b->registerTab(&c, "c");
    bool once = true;
    connect(b, &BrowserToolBridge::tabsChanged, b, [&] {
      if (once) {
        once = false;
        b->setActiveTab("c");
      }
    });
    b->setActiveTab("a");
    QCOMPARE(b->activeTabId(), "c");
    QPointer<BrowserToolBridge> safe = b;
    connect(b, &BrowserToolBridge::tabsChanged, qApp,
            [&] { delete safe.data(); });
    b->setActiveTab({});
    QVERIFY(!safe);
  }

  void registryCapCountsOnlyAccessibleRows() {
    BrowserToolBridge bridge;
    std::vector<std::unique_ptr<BrowserWidget>> browsers;
    for (int i = 0; i < 133; ++i) {
      auto w = std::make_unique<BrowserWidget>();
      if (i >= 130)
        w->setProperty("adeOwnerAgentId", "other");
      bridge.registerTab(w.get(), QString("tab_%1").arg(i));
      browsers.push_back(std::move(w));
    }
    bridge.setActiveTab("tab_129");
    const auto s = bridge.snapshotForAgent("owner");
    QCOMPARE(s.tabs.size(), 128);
    QCOMPARE(s.omittedTabs, 2);
    QCOMPARE(s.tabs.first().id, "tab_129");
    QVERIFY(renderBrowserContext(s).contains("<omitted count=\"2\""));
    for (const auto &tab : s.tabs)
      QVERIFY(tab.id != "tab_130" && tab.id != "tab_131" &&
              tab.id != "tab_132");
  }

  void wrongThreadReturnsNoSnapshot() {
    BrowserToolBridge bridge;
    BrowserWidget w;
    bridge.registerTab(&w, "tab");
    auto pending = std::async(std::launch::async,
                              [&] { return bridge.snapshotForAgent("owner"); });
    QVERIFY(!pending.get().available);
  }
};
QTEST_MAIN(BrowserContextTest)
#include "test_browser_context.moc"
