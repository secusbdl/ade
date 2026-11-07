#include "ui/MainWindow.h"
#include "ui/BrowserNavigationInput.h"
#include "ui/SessionStore.h"
#include "ui/AppStyle.h"
#include "browser/browserwidget.h"
#include <QTemporaryDir>
#include <QTabWidget>
#include <QToolButton>
#include <QLineEdit>
#include <QtTest>
using namespace ade;
namespace {
QToolButton* action(QWidget* root,const QString& name){for(auto* b:root->findChildren<QToolButton*>())if(b->accessibleName()==name)return b;return nullptr;}
QWidget* open(MainWindow& w){auto* b=w.findChild<QToolButton*>("newBrowserTabButton");if(!b)return nullptr;b->click();return w.findChild<QTabWidget*>("contentTabs")->currentWidget();}
bool seed(const QString& path){Conversation chat;chat.id="fixture";chat.title="Browser functional checks";return SessionStore(path).save({chat});}
}
class BrowserFindings:public QObject{
 Q_OBJECT
private slots:
 void initTestCase(){initializeAppStyle(*qApp);}
 void cleanup(){QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);}
 void historyButtonsStartDisabled(){QTemporaryDir dir;QVERIFY(seed(dir.path()));MainWindow w(dir.path());auto* page=open(w);QVERIFY(page);auto* nav=page->findChild<QWidget*>("browserNavigation");QVERIFY(action(nav,"Back"));QVERIFY(action(nav,"Forward"));QVERIFY(!action(nav,"Back")->isEnabled());QVERIFY(!action(nav,"Forward")->isEnabled());QVERIFY(w.close());}
 void historyButtonsTrackAvailability(){QTemporaryDir dir;QVERIFY(seed(dir.path()));MainWindow w(dir.path());auto* page=open(w);auto* browser=page->findChild<BrowserWidget*>();auto* nav=page->findChild<QWidget*>("browserNavigation");emit browser->canGoBackChanged(true);emit browser->canGoForwardChanged(true);QVERIFY(action(nav,"Back")->isEnabled());QVERIFY(action(nav,"Forward")->isEnabled());emit browser->canGoBackChanged(false);emit browser->canGoForwardChanged(false);QVERIFY(!action(nav,"Back")->isEnabled());QVERIFY(!action(nav,"Forward")->isEnabled());QVERIFY(w.close());}
 void loadingActionChangesToStop(){QTemporaryDir dir;QVERIFY(seed(dir.path()));MainWindow w(dir.path());auto* page=open(w);auto* browser=page->findChild<BrowserWidget*>();auto* nav=page->findChild<QWidget*>("browserNavigation");auto* reload=action(nav,"Reload");QVERIFY(reload);emit browser->loadingChanged(true);QCOMPARE(reload->accessibleName(),QString("Stop loading"));emit browser->loadingChanged(false);QCOMPARE(reload->accessibleName(),QString("Reload"));QVERIFY(w.close());}
 void inputContracts_data(){QTest::addColumn<QString>("input");QTest::addColumn<QString>("search");QTest::addColumn<QUrl>("expected");const QString search="https://search.example/?q=%s";QTest::newRow("file-with-spaces")<<"file:///C:/sample file.html"<<search<<QUrl("file:///C:/sample%20file.html");QTest::newRow("file-url")<<"file:///C:/sample%20file.html"<<search<<QUrl("file:///C:/sample%20file.html");QTest::newRow("native-blank")<<"about:blank"<<search<<QUrl("about:blank");QTest::newRow("uppercase-localhost")<<"LOCALHOST:3000/path"<<search<<QUrl("http://localhost:3000/path");QTest::newRow("bare-loopback")<<"::1:8080/path"<<search<<QUrl("http://[::1]:8080/path");QTest::newRow("unicode-search")<<QString::fromUtf8("بحث محلي")<<search<<QUrl("https://search.example/?q=%D8%A8%D8%AD%D8%AB%20%D9%85%D8%AD%D9%84%D9%8A");QTest::newRow("explicit-http")<<"http://example.test/path?q=x"<<search<<QUrl("http://example.test/path?q=x");QTest::newRow("trimmed-domain")<<"  example.test/docs  "<<search<<QUrl("https://example.test/docs");QTest::newRow("single-word")<<"query"<<search<<QUrl("https://search.example/?q=query");}
 void inputContracts(){QFETCH(QString,input);QFETCH(QString,search);QFETCH(QUrl,expected);QCOMPARE(browserNavigationInput(input,search),expected);}
 void addressClassification_data(){QTest::addColumn<QString>("input");QTest::addColumn<QUrl>("url");QTest::newRow("localhost-path")<<"localhost/dashboard"<<QUrl("http://localhost/dashboard");QTest::newRow("loopback-ipv6")<<"[::1]:8080/app"<<QUrl("http://[::1]:8080/app");QTest::newRow("domain")<<"example.test/docs"<<QUrl("https://example.test/docs");QTest::newRow("plain-search")<<"foo bar"<<QUrl("https://www.google.com/search?q=foo%20bar");}
 void addressClassification(){QFETCH(QString,input);QFETCH(QUrl,url);QTemporaryDir dir;QVERIFY(seed(dir.path()));MainWindow w(dir.path());auto* page=open(w);auto* address=page->findChild<QLineEdit*>("browserAddress");address->setText(input);QTest::keyClick(address,Qt::Key_Return);QCOMPARE(page->property("browserSessionUrl").toUrl(),url);QVERIFY(w.close());}
};
QTEST_MAIN(BrowserFindings)
#include "test_browser_navigation_followups.moc"
