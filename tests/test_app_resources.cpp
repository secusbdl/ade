#include "apps/AppResourceStore.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
using namespace ade;

class AppResourceTests final:public QObject {
    Q_OBJECT
    static bool write(const QString& path,const QByteArray& data){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(data)==data.size();}
private slots:
    void filesHeadersAndIdentity() {
        QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(write(temp.filePath("index.html"),"<!doctype html><title>fixture</title>"));QVERIFY(QDir().mkpath(temp.filePath("assets")));QVERIFY(write(temp.filePath("assets/style.css"),"body{color:red}"));
        AppResourceStore store;QString error;QVERIFY2(store.configure(QUrl("app://plugins/figma/figma-app"),temp.path(),&error),qPrintable(error));
        auto html=store.read(QUrl("app://plugins/figma/figma-app/index.html?_t=1"));QCOMPARE(html.status,200);QVERIFY(html.mimeType.startsWith("text/html"));QVERIFY(html.body.contains("fixture"));QCOMPARE(html.headers.value("Cache-Control"),"no-store");
        const auto head=store.read(QUrl("app://plugins/figma/figma-app/index.html"),"HEAD");QCOMPARE(head.status,200);QVERIFY(head.body.isEmpty());QCOMPARE(head.headers.value("Content-Length").toLongLong(),html.body.size());
        QCOMPARE(store.read(QUrl("app://plugins/figma/figma-app/assets/style.css")).body,"body{color:red}");
        QCOMPARE(store.read(QUrl("app://plugins/other/figma-app/index.html")).status,403);QCOMPARE(store.read(QUrl("app://agents/figma/figma-app/index.html")).status,403);QCOMPARE(store.read(QUrl("https://plugins/figma/figma-app/index.html")).status,403);
        QCOMPARE(store.read(QUrl("app://plugins/figma/figma-app/index.html"),"POST").status,405);QCOMPARE(store.read(QUrl("app://plugins/figma/figma-app/missing.txt")).status,404);
    }
    void encodedTraversalAndWindowsPathsAreRejected() {
        QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(write(temp.filePath("secret.txt"),"private"));QVERIFY(QDir().mkpath(temp.filePath("root")));
        AppResourceStore store;QVERIFY(store.configure(QUrl("app://agents/a1/test"),temp.filePath("root")));
        for(const auto& suffix:{"../secret.txt","%2e%2e/secret.txt","%2E%2E%2fsecret.txt","%2e%2e%5csecret.txt","x%00.txt","x%3asecret","C%3a/secret.txt","%ff.txt","NUL","file. ","assets//name"}){
            const auto result=store.read(QUrl(QString("app://agents/a1/test/")+suffix));QVERIFY2(result.status!=200,suffix);QVERIFY(!result.body.contains("private"));
        }
        QVERIFY(!AppResourceStore::safeSegment("CON.txt"));QVERIFY(!AppResourceStore::safeSegment("LPT1"));QVERIFY(AppResourceStore::safeSegment(QString::fromUtf8("مرحبا.css")));
        QVERIFY(!store.configure(QUrl("app://plugins/../app"),temp.path()));
        QVERIFY(!store.configured());QCOMPARE(store.read(QUrl("app://agents/a1/test/index.html")).status,503);
        QVERIFY(store.configure(QUrl("app://agents/a1/test"),temp.filePath("root")));
        QCOMPARE(store.read(QUrl("app://agent@agents/a1/test/index.html")).status,403);QCOMPARE(store.read(QUrl("app://agents:123/a1/test/index.html")).status,403);
    }
    void oversizeAndDirectoryDoNotBecomeFiles() {
        QTemporaryDir temp;QVERIFY(temp.isValid());QFile large(temp.filePath("large.bin"));QVERIFY(large.open(QIODevice::WriteOnly));QVERIFY(large.resize(AppResourceStore::maximumAssetBytes+1));large.close();QVERIFY(QDir().mkdir(temp.filePath("folder")));
        AppResourceStore store;QVERIFY(store.configure(QUrl("app://plugins/test/app"),temp.path()));
        QCOMPARE(store.read(QUrl("app://plugins/test/app/large.bin")).status,413);QCOMPARE(store.read(QUrl("app://plugins/test/app/folder")).status,404);
    }
    void junctionCannotEscapeRoot() {
#ifdef Q_OS_WIN
        QTemporaryDir temp;QVERIFY(temp.isValid());const auto root=temp.filePath("root"),outside=temp.filePath("outside"),link=QDir(root).filePath("escape");QVERIFY(QDir().mkdir(root));QVERIFY(QDir().mkdir(outside));QVERIFY(write(QDir(outside).filePath("secret.txt"),"outside-secret"));
        QProcess process;process.start("cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside)});QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
        AppResourceStore store;const bool configured=store.configure(QUrl("app://plugins/test/app"),root);const auto result=store.read(QUrl("app://plugins/test/app/escape/secret.txt"));
        // Remove only the freshly created junction before QTemporaryDir cleanup.
        const bool removed=RemoveDirectoryW(QDir::toNativeSeparators(link).toStdWString().c_str());
        QVERIFY(removed);QVERIFY(configured);QCOMPARE(result.status,403);QVERIFY(!result.body.contains("outside-secret"));QVERIFY(QFile::exists(QDir(outside).filePath("secret.txt")));
#else
        QSKIP("Windows junction fixture");
#endif
    }
};
QTEST_GUILESS_MAIN(AppResourceTests)
#include "test_app_resources.moc"
