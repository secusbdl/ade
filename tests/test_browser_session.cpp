#include "ui/BrowserSessionStore.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray bytes(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool write(const QString& path, const QByteArray& data) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(data) == data.size(); }
BrowserSessionState fixture() {
    return {{{"first", QUrl("https://example.test/one?q=%E2%9C%93#part"), "First title"},
             {"blank", QUrl("about:blank"), ""},
             {"second", QUrl("http://localhost:3000/two"), QString::fromUtf8("Second \xE2\x9C\x93")}}, "second"};
}
}
class BrowserSessionTest final : public QObject {
    Q_OBJECT
private slots:
    void absentProfileRoundTripOrderAndSelection() {
        QTemporaryDir temp; const auto directory=temp.path()+"/profile";
        BrowserSessionStore store(directory); QString error;
        QVERIFY(store.load(&error).tabs.isEmpty()); QVERIFY2(error.isEmpty(),qPrintable(error));
        QVERIFY(store.isValid()); QVERIFY(!store.existedAtLoad());
        auto state=fixture(); QVERIFY2(store.save(state,&error),qPrintable(error));
        BrowserSessionStore reopened(directory); QCOMPARE(reopened.load(&error),state); QVERIFY(error.isEmpty());
        std::swap(state.tabs[0],state.tabs[2]); state.activeTabId="blank";
        QVERIFY(reopened.save(state,&error)); QCOMPARE(BrowserSessionStore(directory).load(&error),state);
        QVERIFY(reopened.save({},&error)); QVERIFY(BrowserSessionStore(directory).load(&error).tabs.isEmpty());
    }
    void credentialsRemovedButEncodedPathQueryAndFragmentRetained() {
        QTemporaryDir temp; BrowserSessionStore store(temp.path()); auto state=fixture();
        state.tabs[0].url=QUrl("https://user:password@example.test/a%20b?value=%26#section");
        QVERIFY(store.save(state)); const auto saved=bytes(temp.path()+"/browser-tabs.json");
        QVERIFY(!saved.contains("password")); QVERIFY(!saved.contains("user:"));
        const auto restored=BrowserSessionStore(temp.path()).load();
        QCOMPARE(restored.tabs[0].url,QUrl("https://example.test/a%20b?value=%26#section"));
    }
    void invalidStorage_data() {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("truncated") << QByteArray("{\"schemaVersion\":1,");
        QTest::newRow("future-schema") << QByteArray("{\"schemaVersion\":2,\"tabs\":[],\"activeTabId\":\"\"}");
        QTest::newRow("wrong-tabs-type") << QByteArray("{\"schemaVersion\":1,\"tabs\":{},\"activeTabId\":\"\"}");
        QTest::newRow("missing-active") << QByteArray("{\"schemaVersion\":1,\"tabs\":[]}");
        const auto document=[](QJsonArray tabs,const QString& active){return QJsonDocument(QJsonObject{{"schemaVersion",1},{"tabs",tabs},{"activeTabId",active}}).toJson();};
        const QJsonObject a{{"id","a"},{"url","https://example.test"},{"title","A"}};
        QTest::newRow("duplicate-id") << document({a,a},"a");
        QTest::newRow("absent-active") << document({a},"missing");
        auto bad=a;bad["title"]=4;QTest::newRow("title-type") << document({bad},"a");
        for(const auto& scheme:QStringList{"file:///C:/private.txt","javascript:alert(1)","data:text/html,private","about:config","ade-app://owner/a","relative/path","https://"}){
            bad=a;bad["url"]=scheme;QTest::newRow(qPrintable(scheme)) << document({bad},"a");
        }
        QTest::newRow("oversized") << QByteArray(BrowserSessionStore::MaximumBytes+1,' ');
    }
    void invalidStorage() {
        QFETCH(QByteArray,data); QTemporaryDir temp; const auto path=temp.path()+"/browser-tabs.json";
        QVERIFY(write(path,data)); BrowserSessionStore store(temp.path());QString error;
        QVERIFY(store.load(&error).tabs.isEmpty()); QVERIFY(!error.isEmpty()); QVERIFY(!store.isValid());
        QVERIFY(!store.save(fixture(),&error)); QCOMPARE(bytes(path),data);
    }
    void invalidCandidatePreservesOriginal() {
        QTemporaryDir temp;BrowserSessionStore store(temp.path()); auto state=fixture();QVERIFY(store.save(state));
        const auto path=temp.path()+"/browser-tabs.json"; const auto original=bytes(path);
        state.tabs[1].id=state.tabs[0].id; QVERIFY(!store.save(state)); QCOMPARE(bytes(path),original);
        state=fixture();state.tabs[0].url=QUrl("file:///C:/private");QVERIFY(!store.save(state));QCOMPARE(bytes(path),original);
        state=fixture();state.tabs[0].title=QString(1025,'x');QVERIFY(!store.save(state));QCOMPARE(bytes(path),original);
        state=fixture();for(int i=state.tabs.size();i<=BrowserSessionStore::MaximumTabs;++i)state.tabs.append({QString::number(i),QUrl("about:blank"),{}});
        QVERIFY(!store.save(state));QCOMPARE(bytes(path),original);
    }
    void concurrentWindowsAndExternalDeletionDoNotOverwrite() {
        QTemporaryDir temp;BrowserSessionStore first(temp.path()),second(temp.path());
        first.load();second.load();QVERIFY(first.save(fixture())); const auto path=temp.path()+"/browser-tabs.json"; const auto original=bytes(path);
        QString error;QVERIFY(!second.save({},&error));QVERIFY(error.contains("another window"));QCOMPARE(bytes(path),original);
        second.load();QVERIFY(QFile::remove(path));QVERIFY(!second.save(fixture(),&error));QVERIFY(!QFile::exists(path));
    }
    void lockConflictAndExternalEditPreserveData() {
        QTemporaryDir temp;BrowserSessionStore store(temp.path());QVERIFY(store.save(fixture()));
        const auto path=temp.path()+"/browser-tabs.json"; const auto original=bytes(path);
        QLockFile lock(path+".lock");QVERIFY(lock.tryLock());QString error;
        QVERIFY(!store.save({},&error));QVERIFY(error.contains("Another window"));QCOMPARE(bytes(path),original);lock.unlock();
        QVERIFY(write(path,"external edit"));QVERIFY(!store.save({},&error));QCOMPARE(bytes(path),QByteArray("external edit"));
    }
    void replacedProfileDirectoryCannotReceiveStaleSave() {
        QTemporaryDir temp;const auto profile=temp.path()+"/profile";QVERIFY(QDir().mkpath(profile));
        BrowserSessionStore store(profile);QVERIFY(store.save(fixture()));
        const auto original=bytes(profile+"/browser-tabs.json");QVERIFY(QDir().rename(profile,temp.path()+"/retired"));
        QVERIFY(QDir().mkpath(profile));QVERIFY(write(profile+"/browser-tabs.json",original));
        QString error;QVERIFY(!store.save({},&error));QVERIFY(error.contains("directory changed"));
        QCOMPARE(bytes(profile+"/browser-tabs.json"),original);QCOMPARE(bytes(temp.path()+"/retired/browser-tabs.json"),original);
    }
};
QTEST_GUILESS_MAIN(BrowserSessionTest)
#include "test_browser_session.moc"
