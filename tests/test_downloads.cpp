#include "ui/DownloadsPage.h"
#include "browser/browserwidget.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>
#include <memory>

using namespace ade;

namespace {
bool writeFile(const QString& path,const QByteArray& value){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(value)==value.size();}
QByteArray readFile(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
QJsonObject storedRecord(const QString& path,const QString& state) {
    return {{"id","persisted"},{"url","http://127.0.0.1/file"},{"filename","persisted.bin"},{"targetPath",path},{"mimeType","application/octet-stream"},{"state",state},
        {"receivedBytes",10},{"totalBytes",100},{"startTime",1234},{"endTime",0},{"error",""}};
}
}

class DownloadsTests final : public QObject {
    Q_OBJECT
    QTcpServer server;
    std::unique_ptr<QTemporaryDir> temporary;
    QList<QUrl> opened;
    QUrl url(const QString& path="/page")const{return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));}
    bool load(BrowserWidget& browser,const QString& profile="browser") {
        browser.setAttribute(Qt::WA_DontShowOnScreen);browser.resize(800,600);browser.setProfileDirectory(temporary->filePath(profile));
        QSignalSpy navigation(&browser,&BrowserWidget::loadingChanged);browser.navigate(url());browser.show();
        QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<15000){for(const auto& row:navigation)if(!row.at(0).toBool())return true;QTest::qWait(15);}return false;
    }
    static QString state(const DownloadsPage& page,int index=0){return page.records().size()>index?page.records().at(index).toObject().value("state").toString():QString();}
private slots:
    void captureOpened(const QUrl& address){opened.append(address);}
    void initTestCase() {
        QVERIFY(server.listen(QHostAddress::LocalHost,0));
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto* socket=server.nextPendingConnection()) {
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[socket]{
                    if(socket->property("answered").toBool()){socket->readAll();return;}
                    const QByteArray request=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",request);
                    if(!request.contains("\r\n\r\n"))return;
                    socket->setProperty("answered",true);
                    const auto path=request.split(' ').value(1);
                    if(!path.startsWith("/file")){const QByteArray html="<!doctype html><title>Downloads fixture</title><p>Local download fixture</p>";socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: "+QByteArray::number(html.size())+"\r\n\r\n"+html);socket->disconnectFromHost();return;}
                    const bool slow=path.startsWith("/file-slow");const QByteArray payload(slow?512*1024:2048,slow?'s':'f');qsizetype offset=0;
                    for(const auto& line:request.split('\n'))if(line.toLower().startsWith("range: bytes="))offset=line.mid(13).split('-').first().trimmed().toLongLong();
                    offset=qBound<qsizetype>(0,offset,payload.size());
                    QByteArray header=offset?QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ")+QByteArray::number(offset)+'-'+QByteArray::number(payload.size()-1)+'/'+QByteArray::number(payload.size())+"\r\n":QByteArray("HTTP/1.1 200 OK\r\n");
                    socket->write(header+"Content-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=fixture.bin\r\nAccept-Ranges: bytes\r\nConnection: close\r\nContent-Length: "+QByteArray::number(payload.size()-offset)+"\r\n\r\n");
                    if(!slow){socket->write(payload.mid(offset));socket->disconnectFromHost();return;}
                    socket->setProperty("offset",offset);auto* chunks=new QTimer(socket);chunks->setInterval(35);
                    connect(chunks,&QTimer::timeout,socket,[socket,chunks,payload]{if(socket->state()!=QAbstractSocket::ConnectedState){chunks->stop();return;}const auto offset=socket->property("offset").toLongLong();const auto bytes=payload.mid(offset,16*1024);socket->write(bytes);socket->setProperty("offset",offset+bytes.size());if(offset+bytes.size()>=payload.size()){chunks->stop();socket->disconnectFromHost();}});chunks->start();
                });
            }
        });
    }
    void init(){temporary=std::make_unique<QTemporaryDir>();QVERIFY(temporary->isValid());opened.clear();QDesktopServices::setUrlHandler("file",this,"captureOpened");}
    void cleanup(){
        QDesktopServices::unsetUrlHandler("file");
        // WebView2 profile handles are released shortly after the QWidget dies.
        // Only remove this test's freshly-created directory under the temp root.
        const QString path=QDir(temporary->path()).canonicalPath();
        QVERIFY(path.startsWith(QDir(QDir::tempPath()).canonicalPath()+'/'));
        QElapsedTimer wait;wait.start();bool removed=false;
        while(!(removed=QDir(path).removeRecursively())&&wait.elapsed()<3000)QTest::qWait(50);
        if(removed)temporary->setAutoRemove(false);
        temporary.reset();
    }

    void downloadsPersistOpenAndRemoveOnlyHistory() {
        BrowserWidget browser;QVERIFY(load(browser));const QString saved=temporary->filePath("saved.bin");
        {
            DownloadsPage page(temporary->path());int choices=0;QUrl chosenSource;QString chosenSuggested;
            page.setDestinationChooser([&](QWidget*,const QUrl& source,const QString& suggested){++choices;chosenSource=source;chosenSuggested=suggested;return DownloadsPage::DestinationChoice{saved,false};});
            page.attachBrowser(&browser);page.attachBrowser(&browser);
            browser.navigate(url("/file"));QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("completed"),10000);QCOMPARE(choices,1);QCOMPARE(page.records().size(),1);QVERIFY2(page.storageError().isEmpty(),qPrintable(page.storageError()));
            QCOMPARE(chosenSource,url("/file"));QVERIFY(chosenSuggested.endsWith("fixture.bin"));
            QCOMPARE(readFile(saved),QByteArray(2048,'f'));
            auto* list=page.findChild<QTreeWidget*>("downloadsList");QCOMPARE(list->topLevelItemCount(),1);QCOMPARE(list->topLevelItem(0)->text(3),"Complete");
            auto* open=page.findChild<QPushButton*>("downloadOpen");QVERIFY(open->isEnabled());open->click();QCOMPARE(opened.size(),1);QCOMPARE(opened.first(),QUrl::fromLocalFile(saved));
            page.findChild<QPushButton*>("downloadShowFolder")->click();QCOMPARE(opened.size(),2);QCOMPARE(opened.last(),QUrl::fromLocalFile(temporary->path()));
            auto* search=page.findChild<QLineEdit*>("downloadsSearch");search->setText("unmatched");QVERIFY(list->topLevelItem(0)->isHidden());search->setText("saved.bin");QVERIFY(!list->topLevelItem(0)->isHidden());
        }
        DownloadsPage restored(temporary->path());QCOMPARE(restored.records().size(),1);QCOMPARE(state(restored),"completed");
        auto* list=restored.findChild<QTreeWidget*>("downloadsList");list->setCurrentItem(list->topLevelItem(0));restored.findChild<QPushButton*>("downloadRemoveHistory")->click();
        QCOMPARE(restored.records().size(),0);QCOMPARE(readFile(saved),QByteArray(2048,'f'));QCOMPARE(DownloadsPage(temporary->path()).records().size(),0);
    }

    void chooserCancelAndOverwriteRefusal() {
        BrowserWidget browser;QVERIFY(load(browser));DownloadsPage page(temporary->path());page.attachBrowser(&browser);
        page.setDestinationChooser([](QWidget*,const QUrl&,const QString&){return DownloadsPage::DestinationChoice{};});
        browser.navigate(url("/file"));QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("canceled"),10000);QVERIFY(page.records().first().toObject().value("targetPath").toString().isEmpty());
        const QString existing=temporary->filePath("existing.bin");QVERIFY(writeFile(existing,"keep"));
        page.setDestinationChooser([existing](QWidget*,const QUrl&,const QString&){return DownloadsPage::DestinationChoice{existing,false};});
        browser.navigate(url("/file?second"));QTRY_COMPARE_WITH_TIMEOUT(page.records().size(),2,10000);QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("awaiting-path"),1000);QTest::qWait(100);
        QCOMPARE(readFile(existing),QByteArray("keep"));QVERIFY(page.findChild<QPushButton*>("downloadChooseLocation")->isEnabled());
        page.setDestinationChooser([existing](QWidget*,const QUrl&,const QString&){return DownloadsPage::DestinationChoice{existing,true};});page.findChild<QPushButton*>("downloadChooseLocation")->click();
        QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("completed"),10000);QCOMPARE(readFile(existing),QByteArray(2048,'f'));
    }

    void pauseResumeCancelAndClearPreserveActiveRecords() {
        BrowserWidget browser;QVERIFY(load(browser));DownloadsPage page(temporary->path());page.attachBrowser(&browser);int choice=0;
        page.setDestinationChooser([&](QWidget*,const QUrl&,const QString&){return DownloadsPage::DestinationChoice{temporary->filePath(QStringLiteral("transfer-%1.bin").arg(++choice)),false};});
        browser.navigate(url("/file-slow"));QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QPushButton*>("downloadPause")->isEnabled(),10000);
        page.findChild<QPushButton*>("downloadPause")->click();QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("paused"),3000);QVERIFY(page.findChild<QPushButton*>("downloadResume")->isEnabled());
        QVERIFY(page.clearHistory());QCOMPARE(page.records().size(),1);QCOMPARE(state(page),"paused");
        page.findChild<QPushButton*>("downloadResume")->click();QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("completed"),10000);QCOMPARE(readFile(temporary->filePath("transfer-1.bin")),QByteArray(512*1024,'s'));
        browser.navigate(url("/file-slow?cancel"));QTRY_COMPARE_WITH_TIMEOUT(page.records().size(),2,10000);QTRY_VERIFY_WITH_TIMEOUT(page.findChild<QPushButton*>("downloadCancel")->isEnabled(),1000);page.findChild<QPushButton*>("downloadCancel")->click();QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("canceled"),3000);
        QVERIFY(page.clearHistory());QCOMPARE(page.records().size(),0);QCOMPARE(readFile(temporary->filePath("transfer-1.bin")),QByteArray(512*1024,'s'));
    }

    void restoredIncompleteAndMissingFilesAreHonest() {
        const QString path=temporary->filePath("partial.bin");
        QVERIFY(writeFile(temporary->filePath("downloads.json"),QJsonDocument(QJsonObject{{"version",1},{"records",QJsonArray{storedRecord(path,"in-progress")}}}).toJson()));
        DownloadsPage page(temporary->path());QCOMPARE(state(page),"interrupted");QVERIFY(page.records().first().toObject().value("error").toString().contains("not supported"));
        auto* list=page.findChild<QTreeWidget*>("downloadsList");list->setCurrentItem(list->topLevelItem(0));QVERIFY(!page.findChild<QPushButton*>("downloadResume")->isEnabled());QVERIFY(!page.findChild<QPushButton*>("downloadOpen")->isEnabled());
    }

    void malformedAndStaleHistoryAreNotOverwritten() {
        const auto path=temporary->filePath("downloads.json");const QByteArray corrupt="{broken download history";QVERIFY(writeFile(path,corrupt));
        {DownloadsPage page(temporary->path());QVERIFY(!page.storageError().isEmpty());QVERIFY(!page.clearHistory());}QCOMPARE(readFile(path),corrupt);
        QVERIFY(writeFile(path,QJsonDocument(QJsonObject{{"version",1},{"records",QJsonArray{}}}).toJson()));
        DownloadsPage page(temporary->path());QVERIFY(page.storageError().isEmpty());
        const QByteArray replacement=QJsonDocument(QJsonObject{{"version",1},{"records",QJsonArray{storedRecord(temporary->filePath("old.bin"),"completed")}}}).toJson();QVERIFY(writeFile(path,replacement));
        QVERIFY(!page.clearHistory());QCOMPARE(readFile(path),replacement);QVERIFY(page.storageError().contains("changed"));QVERIFY(page.reload());QCOMPARE(page.records().size(),1);
    }

    void closingBrowserMarksTransferInterrupted() {
        auto* browser=new BrowserWidget;QVERIFY(load(*browser));DownloadsPage page(temporary->path());page.attachBrowser(browser);
        page.setDestinationChooser([&](QWidget*,const QUrl&,const QString&){return DownloadsPage::DestinationChoice{temporary->filePath("partial.bin"),false};});
        browser->navigate(url("/file-slow"));QTRY_COMPARE_WITH_TIMEOUT(state(page),QString("in-progress"),10000);delete browser;
        QCOMPARE(state(page),"interrupted");QVERIFY(!page.findChild<QPushButton*>("downloadResume")->isEnabled());
        DownloadsPage restored(temporary->path());QCOMPARE(state(restored),"interrupted");
    }

    void managerDeletionDuringDownloadSignalIsSafe() {
        BrowserWidget browser;QVERIFY(load(browser));auto* page=new DownloadsPage(temporary->path());page->attachBrowser(&browser);QPointer<DownloadsPage> guard(page);
        connect(page,&DownloadsPage::downloadAdded,this,[page]{delete page;});browser.navigate(url("/file-slow"));QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(),10000);
        DownloadsPage restored(temporary->path());QCOMPARE(state(restored),"interrupted");
    }

    void historyLimitNeverAutomaticallyDeletesEntries() {
        QJsonArray records;for(int i=0;i<500;++i){auto record=storedRecord(temporary->filePath(QStringLiteral("old-%1.bin").arg(i)),"completed");record["id"]=QStringLiteral("old-%1").arg(i);records.append(record);}
        QVERIFY(writeFile(temporary->filePath("downloads.json"),QJsonDocument(QJsonObject{{"version",1},{"records",records}}).toJson()));
        BrowserWidget browser;QVERIFY(load(browser));DownloadsPage page(temporary->path());page.attachBrowser(&browser);int choices=0;
        page.setDestinationChooser([&](QWidget*,const QUrl&,const QString&){++choices;return DownloadsPage::DestinationChoice{};});
        QSignalSpy finished(&browser,&BrowserWidget::downloadFinished);browser.navigate(url("/file"));QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,10000);
        QVERIFY(!finished.first().at(2).toBool());QCOMPARE(choices,0);QCOMPARE(page.records().size(),500);QCOMPARE(page.records().first().toObject().value("id").toString(),"old-0");
    }
};
QTEST_MAIN(DownloadsTests)
#include "test_downloads.moc"
