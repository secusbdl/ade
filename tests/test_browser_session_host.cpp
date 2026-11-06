#include "ui/AppStyle.h"
#include "ui/BrowserSessionStore.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/WorkbenchSplitter.h"
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest>
using namespace ade;
namespace {
bool seed(const QString& directory) {
    Conversation chat; chat.id="fixture";chat.title="Browser restore fixture";
    return SessionStore(directory).save({chat});
}
QTabWidget* tabs(MainWindow& window) { return window.findChild<QTabWidget*>("contentTabs"); }
QByteArray read(const QString& path) { QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray(); }
bool write(const QString& path,const QByteArray& data) { QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(data)==data.size(); }
QString pageId(QWidget* page) { return page->property("browserSessionId").toString(); }
QUrl pageUrl(QWidget* page) { return page->property("browserSessionUrl").toUrl(); }
QWidget* open(MainWindow& window,const QString& address={}) {
    auto* button=window.findChild<QToolButton*>("newBrowserTabButton");if(!button)return nullptr;
    button->click();auto* page=tabs(window)->currentWidget();if(!page)return nullptr;
    if(!address.isEmpty()) {
        auto* input=page->findChild<QLineEdit*>("browserAddress");if(!input)return nullptr;
        input->setText(address);QTest::keyClick(input,Qt::Key_Return);
    }
    return page;
}
}
class BrowserSessionHostTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { initializeAppStyle(*qApp); }
    void cleanup() { QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); }
    void realWindowRestartRetainsOrderUrlsSelectionAndTitles() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));QString first,second;
        {
            MainWindow window(temp.path());QVERIFY(tabs(window)->count()==0);
            auto* a=open(window,"https://example.test/first?q=one#anchor");QVERIFY(a);first=pageId(a);
            auto* b=open(window,"http://localhost:3000/second");QVERIFY(b);second=pageId(b);
            tabs(window)->tabBar()->moveTab(1,0);tabs(window)->setCurrentIndex(1);
            QCOMPARE(pageId(tabs(window)->currentWidget()),first);
            QVERIFY(window.close());
        }
        QString error;const auto saved=BrowserSessionStore(temp.path()).load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));
        QCOMPARE(saved.tabs.size(),2);QCOMPARE(saved.tabs[0].id,second);QCOMPARE(saved.activeTabId,first);
        {
            MainWindow restored(temp.path());QCOMPARE(tabs(restored)->count(),2);
            QCOMPARE(pageId(tabs(restored)->widget(0)),second);QCOMPARE(pageId(tabs(restored)->widget(1)),first);
            QCOMPARE(pageUrl(tabs(restored)->widget(0)),QUrl("http://localhost:3000/second"));
            QCOMPARE(pageUrl(tabs(restored)->currentWidget()),QUrl("https://example.test/first?q=one#anchor"));
            QCOMPARE(tabs(restored)->currentWidget()->findChild<QLineEdit*>("browserAddress")->text(),QString("https://example.test/first?q=one#anchor"));
            QVERIFY(restored.close());
        }
    }
    void legacyProfileStartupMigratesOnceAndExplicitEmptyStaysEmpty() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));
        {SettingsPage settings(temp.path());QVERIFY(settings.setPreference("browsing.startupUrl","https://example.test/start"));}
        {
            MainWindow window(temp.path());QCOMPARE(tabs(window)->count(),1);QCOMPARE(pageUrl(tabs(window)->widget(0)),QUrl("https://example.test/start"));
            open(window,"https://example.test/second");QCOMPARE(tabs(window)->count(),2);QVERIFY(window.close());
        }
        {
            MainWindow restored(temp.path());QCOMPARE(tabs(restored)->count(),2);
            while(tabs(restored)->count())emit tabs(restored)->tabCloseRequested(0);
            QVERIFY(restored.close());
        }
        MainWindow empty(temp.path());QCOMPARE(tabs(empty)->count(),0);QVERIFY(empty.close());
    }
    void restoreBlankDoesNotUseCustomNewTabAndPreservesHiddenContent() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));
        {SettingsPage settings(temp.path());QVERIFY(settings.setPreference("browsing.newTabUrl","https://example.test/new"));}
        BrowserSessionState state{{{"blank",QUrl("about:blank"),"Saved title"}},"blank"};
        QVERIFY(BrowserSessionStore(temp.path()).save(state));
        {
            MainWindow window(temp.path());QCOMPARE(tabs(window)->count(),1);QCOMPARE(pageUrl(tabs(window)->widget(0)),QUrl("about:blank"));
            QCOMPARE(tabs(window)->tabText(0),QString("Saved title"));
            auto* split=window.findChild<WorkbenchSplitter*>();QVERIFY(split);split->setContentCollapsed(true);QVERIFY(window.close());
        }
        MainWindow restored(temp.path());auto* split=restored.findChild<WorkbenchSplitter*>();QVERIFY(split);QVERIFY(split->contentCollapsed());
        auto* page=open(restored);QVERIFY(page);QCOMPARE(pageUrl(page),QUrl("https://example.test/new"));QVERIFY(!split->contentCollapsed());
        QVERIFY(restored.close());
    }
    void inactiveCloseAndNonBrowserPageDoNotResurrectTabs() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));QString survivor;
        {
            MainWindow window(temp.path());QVERIFY(open(window,"https://example.test/closed"));
            auto* active=open(window,"https://example.test/retained");QVERIFY(active);survivor=pageId(active);
            emit tabs(window)->tabCloseRequested(0);
            // An ordinary non-browser content page must never enter the browser inventory.
            tabs(window)->setCurrentIndex(tabs(window)->addTab(new QWidget,"Transient content"));
            QVERIFY(window.close());
        }
        MainWindow restored(temp.path());QCOMPARE(tabs(restored)->count(),1);QCOMPARE(pageId(tabs(restored)->currentWidget()),survivor);QVERIFY(restored.close());
    }
    void corruptStateReportedAndPreservedEvenAfterNewTabAndClose() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));const auto path=temp.path()+"/browser-tabs.json";const QByteArray original="{\"schemaVersion\":99,\"private\":\"preserve\"}";
        QVERIFY(write(path,original));
        {MainWindow window(temp.path());QCOMPARE(tabs(window)->count(),0);QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("preserved"));
         QVERIFY(open(window,"https://example.test/new"));QVERIFY(window.close());}
        QCOMPARE(read(path),original);
    }
    void externalWindowRevisionPreservedOnClose() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));MainWindow window(temp.path());
        BrowserSessionState external{{{"external",QUrl("https://example.test/external"),"Other window"}},"external"};
        QVERIFY(BrowserSessionStore(temp.path()).save(external));const auto path=temp.path()+"/browser-tabs.json";const auto original=read(path);
        QVERIFY(open(window,"https://example.test/local"));QVERIFY(window.close());QCOMPARE(read(path),original);
        QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("another window"));
    }
    void coalescedSavePersistsWithoutClose() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));MainWindow window(temp.path());QVERIFY(open(window,"https://example.test/live"));
        const auto path=temp.path()+"/browser-tabs.json";
        QTRY_VERIFY_WITH_TIMEOUT(read(path).contains("https://example.test/live"),1500);
        QVERIFY(window.close());
    }
    void tabSelectionCallbackCanDeleteHost() {
        QTemporaryDir temp;QVERIFY(seed(temp.path()));QPointer<MainWindow> window=new MainWindow(temp.path());
        auto* tabWidget=tabs(*window);auto* button=window->findChild<QToolButton*>("newBrowserTabButton");QVERIFY(button);
        connect(tabWidget,&QTabWidget::currentChanged,window,[window]{delete window.data();});
        button->click();QVERIFY(window.isNull());
    }
};
QTEST_MAIN(BrowserSessionHostTest)
#include "test_browser_session_host.moc"
