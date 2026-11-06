#include "ui/MainWindow.h"
#include "ui/Composer.h"
#include "ui/AppStyle.h"
#include "ui/SettingsPage.h"
#include "ui/SessionStore.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

using namespace ade;
namespace {
bool seed(const QString& path){Conversation value;value.id="fixture";value.title="Fixture chat";value.draft="";return SessionStore(path).save({value});}
void showHidden(MainWindow& window){window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1540,940);window.show();QCoreApplication::processEvents();}
QToolButton* sidebarToggle(MainWindow& window){for(auto* button:window.findChildren<QToolButton*>())if(button->accessibleName()=="Toggle sidebar")return button;return nullptr;}
class ComposerReplyServer : public QTcpServer {
public:
    QList<QJsonObject> requests;
    ComposerReplyServer(){
        listen(QHostAddress::LocalHost);
        connect(this,&QTcpServer::newConnection,this,[this]{while(auto* socket=nextPendingConnection()){
            connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
                if(socket->property("handled").toBool()){socket->readAll();return;}
                const auto bytes=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",bytes);
                const auto end=bytes.indexOf("\r\n\r\n");if(end<0)return;
                int length=0;for(const auto& line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
                if(bytes.size()<end+4+length)return;
                socket->setProperty("handled",true);requests.append(QJsonDocument::fromJson(bytes.mid(end+4,length)).object());
                const QByteArray first="data: {\"choices\":[{\"delta\":{\"content\":\"Local \"}}]}\n\n";
                const QByteArray last="data: {\"choices\":[{\"delta\":{\"content\":\"reply.\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "+QByteArray::number(first.size()+last.size())+"\r\nConnection: close\r\n\r\n"+first);
                QTimer::singleShot(25,socket,[socket,last]{socket->write(last);socket->disconnectFromHost();});
            });
        }});
    }
};
}
class MainChromeTest:public QObject{
    Q_OBJECT
private slots:
    void settingsHasVisibleLabelAndOpens_data(){QTest::addColumn<int>("zoom");for(int zoom:{70,100,130})QTest::newRow(qPrintable(QString::number(zoom)))<<zoom;}
    void settingsHasVisibleLabelAndOpens(){
        QFETCH(int,zoom);QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        auto* page=window.findChild<SettingsPage*>();QVERIFY(page);QVERIFY(page->setPreference("personalization.uiZoom",zoom));
        auto* shell=window.findChild<QSplitter*>("sidebarSplitter");QVERIFY(shell);shell->setSizes({qRound(176*zoom/100.0),window.width()});QCoreApplication::processEvents();
        auto* settings=window.findChild<QToolButton*>("settingsButton");QVERIFY(settings&&settings->isVisible());
        QCOMPARE(settings->text(),QString("Settings"));QCOMPARE(settings->toolButtonStyle(),Qt::ToolButtonTextBesideIcon);
        QVERIFY(settings->width()>=settings->fontMetrics().horizontalAdvance(settings->text())+settings->iconSize().width()+8);
        QVERIFY(settings->parentWidget()->rect().contains(settings->geometry()));
        settings->click();QCoreApplication::processEvents();QVERIFY(page->isVisible());QCOMPARE(page->currentSection(),QString("models-providers"));
        auto* import=page->findChild<QPushButton*>("settingsImportProviderKeys");QVERIFY(import&&import->isVisible());
        auto* back=page->findChild<QPushButton*>("settingsBack");QVERIFY(back);back->click();QCoreApplication::processEvents();QVERIFY(settings->isVisible());
    }
    void providerImportReportsVisibleOutcomeAndRefreshesModels(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        window.findChild<QToolButton*>("settingsButton")->click();auto* page=window.findChild<SettingsPage*>();QVERIFY(page&&page->isVisible());
        const auto path=data.filePath("synthetic.env");{QFile fixture(path);QVERIFY(fixture.open(QIODevice::WriteOnly));const QByteArray bytes("OPENAI_API_KEY=fixture-not-a-live-key\n");QCOMPARE(fixture.write(bytes),qint64(bytes.size()));}
        window.importKeys(path);QCoreApplication::processEvents();
        auto* status=page->findChild<QLabel*>("settingsStatus");QVERIFY(status&&status->isVisible());QVERIFY(status->text().contains("Imported 1 provider key(s)"));
        bool providerCard=false;for(auto* label:page->findChildren<QLabel*>())if(label->isVisible()&&label->text()=="OpenAI")providerCard=true;QVERIFY(providerCard);
        auto* models=window.findChild<QComboBox*>("modelSelector");QVERIFY(models);bool importedModel=false;for(int i=0;i<models->count();++i)if(models->itemText(i).contains("gpt-4o-mini"))importedModel=true;QVERIFY(importedModel);
        const auto modelCount=models->count();{QFile fixture(path);QVERIFY(fixture.open(QIODevice::WriteOnly|QIODevice::Truncate));QVERIFY(fixture.write("This is a malformed import fixture.")>0);}
        window.importKeys(path);QCoreApplication::processEvents();status=page->findChild<QLabel*>("settingsStatus");QVERIFY(status&&status->isVisible());QVERIFY(status->text().contains("No PROVIDER_API_KEY entries were found"));QCOMPARE(models->count(),modelCount);
        for(auto* label:page->findChildren<QLabel*>())QVERIFY(!label->text().contains("fixture-not-a-live-key"));
    }
    void repeatedShownWindowTeardownPreservesStyleObservers(){
        for(int iteration=0;iteration<6;++iteration){
            QTemporaryDir data;QVERIFY(seed(data.path()));
            {MainWindow window(data.path());showHidden(window);auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings);
                QVERIFY(settings->setPreference("personalization.appearance",iteration%2?"light":"dark"));QCoreApplication::processEvents();QVERIFY(window.close());}
            QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QCoreApplication::processEvents();
        }
    }
    void sidebarReallyCollapses(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        auto* sidebar=window.findChild<QWidget*>("sidebar");auto* toggle=sidebarToggle(window);QVERIFY(sidebar);QVERIFY(toggle);
        toggle->click();QCoreApplication::processEvents();QCOMPARE(sidebar->width(),0);
        auto* reopen=window.findChild<QToolButton*>("sidebarReopenButton");QVERIFY(reopen);QVERIFY(reopen->isVisible());QVERIFY(!reopen->accessibleName().isEmpty());
        reopen->click();QCoreApplication::processEvents();QVERIFY(sidebar->width()>=176);QVERIFY(sidebar->width()<=672);
    }
    void newlyOpenedContentUsesSourceDefault(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        window.findChild<QToolButton*>("openBrowserButton")->click();auto* split=window.findChild<QSplitter*>("workspaceSplitter");QVERIFY(split);const auto sizes=split->sizes();QCOMPARE(sizes.size(),3);
        const double ratio=double(sizes[1])/(sizes[0]+sizes[1]);QVERIFY2(ratio>0.66&&ratio<0.74,qPrintable(QString::number(ratio)));
    }
    void idleStopAndEmptySendReflectState(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        auto* stop=window.findChild<QToolButton*>("stopButton");auto* send=window.findChild<QToolButton*>("sendButton");auto* input=window.findChild<Composer*>();QVERIFY(stop&&send&&input);
        QVERIFY(stop->isHidden());QVERIFY(!send->isEnabled());input->setPlainText("Ready");QVERIFY(send->isEnabled());input->clear();QVERIFY(!send->isEnabled());
    }
    void legacyChatCanSendAndRestart_data(){
        QTest::addColumn<bool>("enter");QTest::newRow("send-button")<<false;QTest::newRow("enter-key")<<true;
    }
    void legacyChatCanSendAndRestart(){
        QFETCH(bool,enter);QTemporaryDir data;ComposerReplyServer provider;QVERIFY(data.isValid());QVERIFY(provider.isListening());
        QString error;const ProviderConfig config{"fixture","Local fixture",QString("http://127.0.0.1:%1/v1").arg(provider.serverPort()),"synthetic-composer-key","fixture-model","openai"};
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"),{config},&error),qPrintable(error));
        const QJsonArray history{QJsonObject{{"role","user"},{"content","Earlier question"}},QJsonObject{{"role","assistant"},{"content","Earlier answer"}}};
        const QJsonObject legacy{{"id","legacy-chat"},{"title","Retained chat"},{"providerId","fixture"},{"model","fixture-model"},{"draft",""},{"messages",history},{"titleLocked",true}};
        // An actual older file, not produced by the current writer: cleanup and
        // newer optional fields are absent. This is the user's startup failure.
        const auto original=QJsonDocument(QJsonObject{{"schemaVersion",1},{"sessions",QJsonArray{legacy}}}).toJson();
        {QFile file(data.filePath("sessions.json"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),qint64(original.size()));}
        {
            MainWindow window(data.path());showHidden(window);QCOMPARE(window.currentSessionId(),QString("legacy-chat"));
            auto* input=window.findChild<Composer*>();auto* send=window.findChild<QToolButton*>("sendButton");QVERIFY(input&&send);
            QVERIFY(!window.findChild<QLabel*>("sessionStatus")->text().contains("cleanup records are invalid"));
            {QFile file(data.filePath("sessions.json"));QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),original);}
            QVERIFY(!send->isEnabled());QTest::keyClicks(input,"hi there");QVERIFY(send->isEnabled());
            if(enter)QTest::keyClick(input,Qt::Key_Return);else send->click();
            QTRY_COMPARE_WITH_TIMEOUT(provider.requests.size(),1,5000);
            auto* engine=window.findChild<AgentEngine*>();QVERIFY(engine);QTRY_VERIFY_WITH_TIMEOUT(!engine->isBusy()&&engine->lastTurnSucceeded(),5000);
            const auto wire=QJsonDocument(provider.requests.first()).toJson();QVERIFY(wire.contains("Earlier question"));QVERIFY(wire.contains("Earlier answer"));QVERIFY(wire.contains("hi there"));
            const auto stored=SessionStore(data.path()).load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(stored.size(),1);QCOMPARE(stored[0].id,QString("legacy-chat"));
            QCOMPARE(stored[0].messages.size(),4);QCOMPARE(stored[0].messages[0],history[0]);QCOMPARE(stored[0].messages[1],history[1]);
            QCOMPARE(stored[0].messages.last().toObject()["content"].toString(),QString("Local reply."));QVERIFY(input->toPlainText().isEmpty());
        }
        MainWindow reopened(data.path());showHidden(reopened);QCOMPARE(reopened.currentSessionId(),QString("legacy-chat"));
        auto* input=reopened.findChild<Composer*>();input->setPlainText("Next message");QVERIFY(reopened.findChild<QToolButton*>("sendButton")->isEnabled());
        const auto stored=SessionStore(data.path()).load(&error);QVERIFY(error.isEmpty());QCOMPARE(stored[0].messages.size(),4);QCOMPARE(stored[0].title,QString("Retained chat"));
    }
    void persistedSidebarAndZoomPreserveCollapsedState(){
        QTemporaryDir data;QVERIFY(seed(data.path()));
        {MainWindow window(data.path());showHidden(window);auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings->setPreference("personalization.uiZoom",130));
            auto* shell=window.findChild<QSplitter*>("sidebarSplitter");QVERIFY(shell);shell->setSizes({416,window.width()-418});emit shell->splitterMoved(416,1);
            sidebarToggle(window)->click();QCOMPARE(shell->sizes()[0],0);QVERIFY(window.close());}
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        {MainWindow window(data.path());showHidden(window);auto* shell=window.findChild<QSplitter*>("sidebarSplitter");QVERIFY(shell);QCOMPARE(shell->sizes()[0],0);
            auto* reopen=window.findChild<QToolButton*>("sidebarReopenButton");QVERIFY(reopen&&reopen->isVisible());reopen->click();QVERIFY(qAbs(shell->sizes()[0]-416)<=2);
            auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings->setPreference("personalization.uiZoom",70));QVERIFY(qAbs(shell->sizes()[0]-224)<=2);
            sidebarToggle(window)->click();QCOMPARE(shell->sizes()[0],0);QVERIFY(settings->setPreference("personalization.uiZoom",100));QCOMPARE(shell->sizes()[0],0);reopen->click();QVERIFY(qAbs(shell->sizes()[0]-320)<=2);
            QVERIFY(window.close());}
    }
    void restoredInnerSplitIsNotReplacedByFreshDefault(){
        QTemporaryDir data;QVERIFY(seed(data.path()));double expected=0;
        {MainWindow window(data.path());showHidden(window);window.findChild<QToolButton*>("openBrowserButton")->click();auto* split=window.findChild<QSplitter*>("workspaceSplitter");split->setSizes({650,350,0});emit split->splitterMoved(650,1);const auto sizes=split->sizes();expected=double(sizes[1])/(sizes[0]+sizes[1]);QVERIFY(window.close());}
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        MainWindow restored(data.path());showHidden(restored);restored.findChild<QToolButton*>("openBrowserButton")->click();const auto sizes=restored.findChild<QSplitter*>("workspaceSplitter")->sizes();const double actual=double(sizes[1])/(sizes[0]+sizes[1]);QVERIFY(qAbs(actual-expected)<0.015);
    }
    void sidebarHalfWindowLimitPreservesPreferredWidth(){
        QTemporaryDir data;QVERIFY(seed(data.path()));{QSettings saved(data.filePath("window.ini"),QSettings::IniFormat);saved.setValue("sidebarWidth",672);}
        MainWindow window(data.path());showHidden(window);auto* shell=window.findChild<QSplitter*>("sidebarSplitter");QVERIFY(shell);QCOMPARE(shell->sizes()[0],672);
        window.resize(960,600);QCoreApplication::processEvents();QVERIFY2(shell->sizes()[0]<=shell->width()/2,qPrintable(QString("side=%1 shell=%2 max=%3 preferred=%4").arg(shell->sizes()[0]).arg(shell->width()).arg(shell->widget(0)->maximumWidth()).arg(shell->widget(0)->width())));
        sidebarToggle(window)->click();QCOMPARE(shell->sizes()[0],0);window.findChild<QToolButton*>("sidebarReopenButton")->click();
        window.resize(1540,940);QCoreApplication::processEvents();QCOMPARE(shell->sizes()[0],672);
    }
    void narrowSelectorsRemainInsideComposer_data(){QTest::addColumn<int>("zoom");for(int zoom:{70,100,130})QTest::newRow(qPrintable(QString::number(zoom)))<<zoom;}
    void narrowSelectorsRemainInsideComposer(){
        QFETCH(int,zoom);QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);auto* settings=window.findChild<SettingsPage*>();QVERIFY(settings->setPreference("personalization.uiZoom",zoom));window.resize(960,600);
        window.findChild<QToolButton*>("openBrowserButton")->click();auto* split=window.findChild<QSplitter*>("workspaceSplitter");split->setSizes({1,1000,0});auto* model=window.findChild<QComboBox*>("modelSelector");auto* permission=window.findChild<QComboBox*>("permissionSelector");
        const QString label="Long provider / a-model-name-with-a-long-context-window";model->addItem(label);model->setCurrentIndex(model->count()-1);QCoreApplication::processEvents();
        auto* box=window.findChild<QWidget*>("composerBox");auto* rail=window.findChild<QWidget*>("composerActions");auto* send=window.findChild<QToolButton*>("sendButton");QVERIFY(rail&&box&&send);
        const QRect modelRect(model->mapTo(box,QPoint()),model->size()),permissionRect(permission->mapTo(box,QPoint()),permission->size()),railRect(rail->mapTo(box,QPoint()),rail->size());
        QVERIFY2(box->rect().contains(modelRect),qPrintable(QString("box%1x%2 model%3,%4 %5x%6").arg(box->width()).arg(box->height()).arg(modelRect.x()).arg(modelRect.y()).arg(modelRect.width()).arg(modelRect.height())));
        QVERIFY(box->rect().contains(permissionRect));QVERIFY(!modelRect.intersects(permissionRect));QVERIFY(!modelRect.intersects(railRect));QVERIFY(!permissionRect.intersects(railRect));
        QCOMPARE(send->width(),qRound(32*zoom/100.0));QCOMPARE(rail->width(),send->width());QCOMPARE(model->toolTip(),label);QCOMPARE(model->lineEdit()->cursorPosition(),0);QVERIFY(model->width()>100);QCOMPARE(window.width(),960);
    }
    void keyboardOpensCommandCenterWhileSidebarCollapsed(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);window.activateWindow();QApplication::setActiveWindow(&window);auto* input=window.findChild<Composer*>();input->setFocus();QCoreApplication::processEvents();
        QTest::keyClick(input,Qt::Key_B,Qt::ControlModifier);QCoreApplication::processEvents();auto* shell=window.findChild<QSplitter*>("sidebarSplitter");QVERIFY(shell);QCOMPARE(shell->sizes()[0],0);
        QTest::keyClick(window.findChild<QToolButton*>("sidebarReopenButton"),Qt::Key_K,Qt::ControlModifier);QCoreApplication::processEvents();auto* center=window.findChild<QDialog*>("commandCenter");QVERIFY(center&&center->isVisible());QCOMPARE(shell->sizes()[0],0);auto* query=center->findChild<QLineEdit*>("commandQuery");QVERIFY(query&&query->hasFocus());QTest::keyClick(query,Qt::Key_Escape);QVERIFY(!center->isVisible());QCOMPARE(shell->sizes()[0],0);
    }
    void busyStopIsVisibleAndEscapeCancels(){
        QTemporaryDir data;QTcpServer provider;QVERIFY(provider.listen(QHostAddress::LocalHost));ProviderConfig config;config.id="local";config.name="Local fixture";config.model="fixture";config.baseUrl=QString("http://127.0.0.1:%1/v1").arg(provider.serverPort());config.apiKey="fixture-not-a-secret";QVERIFY(ProviderStore::save(data.filePath("providers.json"),{config}));
        Conversation session;session.id="fixture";session.title="Local held request";session.providerId=config.id;session.model=config.model;QVERIFY(SessionStore(data.path()).save({session}));
        MainWindow window(data.path());showHidden(window);window.submitPrompt("Hold locally");QTRY_VERIFY(provider.hasPendingConnections());auto* stop=window.findChild<QToolButton*>("stopButton");QVERIFY(stop->isVisible()&&stop->isEnabled());QVERIFY(!window.findChild<QToolButton*>("sendButton")->isEnabled());
        auto* engine=window.findChild<AgentEngine*>();QVERIFY(engine&&engine->isBusy());QTest::keyClick(window.findChild<Composer*>(),Qt::Key_Escape);QVERIFY(!engine->isBusy());QVERIFY(stop->isHidden());
    }
    void composerGrowsForWrappedTextAndCaps(){
        Composer composer;composer.setAttribute(Qt::WA_DontShowOnScreen);composer.resize(320,80);composer.show();QCoreApplication::processEvents();const auto empty=composer.height();
        composer.setPlainText(QString("A wrapped sentence with spaces. ").repeated(14));QCoreApplication::processEvents();QVERIFY(composer.height()>empty);
        composer.setPlainText(QString("Long line\n").repeated(120));QCoreApplication::processEvents();QCOMPARE(composer.height(),256);QCOMPARE(composer.toPlainText(),QString("Long line\n").repeated(120));
        composer.clear();QCoreApplication::processEvents();QVERIFY(composer.height()<74);
    }
    void shortWrappedDraftHasNoScrollbars(){
        QTemporaryDir data;QVERIFY(seed(data.path()));MainWindow window(data.path());showHidden(window);
        auto* input=window.findChild<Composer*>();input->setPlainText(QString("Refine the layout while preserving the saved workspace. ").repeated(4));QCoreApplication::processEvents();
        QVERIFY(input->height()>26);QCOMPARE(input->horizontalScrollBarPolicy(),Qt::ScrollBarAlwaysOff);
        QTRY_COMPARE(input->verticalScrollBar()->maximum(),0);QVERIFY(!input->verticalScrollBar()->isVisible());
    }
    void enterAndShiftEnterPreserveInputContract(){
        Composer composer;composer.setAttribute(Qt::WA_DontShowOnScreen);composer.show();QSignalSpy submitted(&composer,&Composer::submitted);
        QTest::keyClicks(&composer,"one");QTest::keyClick(&composer,Qt::Key_Return,Qt::ShiftModifier);QTest::keyClicks(&composer,"two");QCOMPARE(composer.toPlainText(),QString("one\ntwo"));QCOMPARE(submitted.size(),0);
        QTest::keyClick(&composer,Qt::Key_Return);QCOMPARE(submitted.size(),1);QCOMPARE(composer.toPlainText(),QString("one\ntwo"));
    }
};
int main(int argc,char** argv){
    if(qEnvironmentVariableIsSet("ADE_TRACE_CRASH"))AddVectoredExceptionHandler(1,[](EXCEPTION_POINTERS* exception)->LONG{
        if(exception->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION){std::fprintf(stderr,"Image base %p; exception %p\n",static_cast<void*>(GetModuleHandleW(nullptr)),exception->ExceptionRecord->ExceptionAddress);void* frames[32];const auto count=CaptureStackBackTrace(0,32,frames,nullptr);for(USHORT i=0;i<count;i++)std::fprintf(stderr,"frame %p\n",frames[i]);}
        return EXCEPTION_CONTINUE_SEARCH;
    });
    qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);initializeAppStyle(app);MainChromeTest tests;return QTest::qExec(&tests,argc,argv);
}
#include "test_main_chrome.moc"
