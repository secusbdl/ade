#include "browser/BrowserToolBridge.h"
#include "browser/browserwidget.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include "editor/EditorWidget.h"
#include "runtime/AgentToolHost.h"
#include "ui/AppStyle.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/MarkdownParts.h"
#include "ui/ImagePreview.h"
#include "ui/SessionStore.h"
#include <QClipboard>
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBlock>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
using namespace ade;
namespace {
bool put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
bool select(MainWindow& window, const QString& id) {
    auto* list = window.findChild<QListWidget*>("agentList");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == id) {
            list->setCurrentItem(list->item(i));
            return true;
        }
    return false;
}
QJsonObject message(const QString& text, QJsonArray attachments = {}) {
    QJsonObject result{{"role", "assistant"}, {"content", text}};
    if (!attachments.isEmpty())
        result["metadata"] = QJsonObject{{"attachments", attachments}};
    return result;
}
QJsonObject row(const QJsonArray& history) {
    return AgentEngine::displayHistory(history).last().toObject();
}
void click(ConversationView* view, const QString& chat, const QJsonObject& row, const QUrl& url) {
    emit view->scopedLinkActivated(chat, row["id"].toString(),
                                   row["parts"].toArray().first().toObject()["id"].toString(), url);
}
bool seed(const QString& dir, const QJsonArray& messages, const QString& workspace = {}) {
    QList<Conversation> chats;
    for (const auto& id : {"first", "second"}) {
        Conversation chat;
        chat.id = id;
        chat.title = id;
        chat.titleLocked = true;
        chat.workspace = workspace;
        chat.messages = messages;
        chats.append(chat);
    }
    return SessionStore(dir).save(chats);
}
class Provider : public QObject {
  public:
    QTcpServer server;
    int requests = 0;
    QString reply = "[Open current file](path:ws1/note.txt)";
    Provider() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0)
                        return;
                    int size = 0;
                    for (const auto& line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            size = line.mid(15).toInt();
                    if (bytes.size() < end + 4 + size || socket->property("handled").toBool())
                        return;
                    socket->setProperty("handled", true);
                    ++requests;
                    const QJsonObject answer{
                        {"choices", QJsonArray{QJsonObject{
                                        {"message", QJsonObject{{"role", "assistant"}, {"content", reply}}},
                                        {"finish_reason", "stop"}}}}};
                    const auto body = QJsonDocument(answer).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    bool save(const QString& dir) {
        ProviderConfig p;
        p.id = "fixture";
        p.name = "Fixture";
        p.model = "fixture";
        p.apiKey = "synthetic-link-fixture";
        p.baseUrl = QString("http://127.0.0.1:%1/v1").arg(server.serverPort());
        return ProviderStore::save(dir + "/providers.json", {p});
    }
};
QWidget* tab(MainWindow& window, const QString& id, const QString& owner = {}, bool mini = false,
             BrowserWidget* existing = nullptr) {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* browser = existing ? existing : new BrowserWidget;
    browser->setProperty("adeOwnerAgentId", owner);
    browser->setProperty("miniAppBrowser", mini);
    browser->setAttribute(Qt::WA_DontShowOnScreen);
    layout->addWidget(browser);
    if (mini) {
        page->setProperty("appInstanceId", "fixture-app");
        page->setProperty("appAgentId", owner);
    }
    page->setProperty("browserTabId", window.findChild<BrowserToolBridge*>()->registerTab(browser, id));
    window.findChild<QTabWidget*>("contentTabs")->addTab(page, id);
    return page;
}
} // namespace
class TranscriptLinkUiTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        initializeAppStyle(*qobject_cast<QApplication*>(qApp));
    }
    void rejectsStaleChatAndPartBeforeClipboardAction() {
        QTemporaryDir data;
        const QJsonArray history{message("[Color](color:%23ff0000)")};
        QVERIFY(seed(data.path(), history));
        MainWindow window(data.path());
        QVERIFY(select(window, "first"));
        auto* view = window.findChild<ConversationView*>();
        const auto saved = row(history);
        QApplication::clipboard()->setText("unchanged");
        emit view->scopedLinkActivated("first", saved["id"].toString(), "stale-part", QUrl("color:blue"));
        QCOMPARE(QApplication::clipboard()->text(), "unchanged");
        emit view->scopedLinkActivated("first", "stale-row",
                                       saved["parts"].toArray().first().toObject()["id"].toString(),
                                       QUrl("color:blue"));
        QCOMPARE(QApplication::clipboard()->text(), "unchanged");
        QVERIFY(select(window, "second"));
        click(view, "first", saved, QUrl("color:red"));
        QCOMPARE(QApplication::clipboard()->text(), "unchanged");
        QVERIFY(select(window, "first"));
        click(view, "first", saved, QUrl("color:%23ff0000"));
        QCOMPARE(QApplication::clipboard()->text(), "#ff0000");
        QCOMPARE(window.findChildren<EditorWidget*>().size(), 0);
    }
    void fixedChatAttachmentOpensReadOnlyWithoutSaveAndClosesWithHost() {
        QTemporaryDir data;
        AttachmentStore store(data.path());
        const auto ref = store.write("first", "report.txt", "SAFE ATTACHMENT\n");
        QVERIFY(!ref.path.isEmpty());
        const auto foreign = store.agentDirectory("second", nullptr, true);
        QVERIFY(put(foreign + '/' + ref.path.mid(4), "FOREIGN"));
        const QJsonArray history{message("[Report](path:" + ref.path + ")", QJsonArray{ref.toJson()})};
        QVERIFY(seed(data.path(), history));
        MainWindow window(data.path());
        QVERIFY(select(window, "first"));
        auto* view = window.findChild<ConversationView*>();
        click(view, "first", row(history), QUrl("path:" + ref.path));
        auto* text = window.findChild<QPlainTextEdit*>("transcriptAttachmentText");
        QVERIFY2(text, qPrintable(window.findChild<QLabel*>("sessionStatus")->text()));
        QVERIFY(text->isReadOnly());
        QCOMPARE(text->toPlainText(), "SAFE ATTACHMENT\n");
        QCOMPARE(window.findChildren<EditorWidget*>().size(), 0);
        auto* page = window.findChild<QWidget*>("transcriptAttachmentPreview");
        QVERIFY(page);
        QVERIFY(page->findChildren<QPushButton*>().isEmpty());
        QPointer<QWidget> guarded = page;
        delete window.findChild<AgentToolHost*>();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!guarded);
    }
    void binaryAttachmentRemainsExplicitlyUnsupported() {
        QTemporaryDir data;
        AttachmentStore store(data.path());
        const auto ref = store.write("first", "archive.zip", QByteArray::fromHex("504b03040000ff"));
        const QJsonArray history{message("[Archive](path:" + ref.path + ")")};
        QVERIFY(seed(data.path(), history));
        MainWindow window(data.path());
        QVERIFY(select(window, "first"));
        click(window.findChild<ConversationView*>(), "first", row(history), QUrl("path:" + ref.path));
        QVERIFY(!window.findChild<QWidget*>("transcriptAttachmentPreview"));
        QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("limited to supported text"));
    }
    void newTurnWorkspaceAuthorityDoesNotUpgradeOldHistoryOrSurviveWorkspaceSwap() {
        QTemporaryDir data, workspace, other;
        QVERIFY(put(workspace.filePath("note.txt"), "OWNED"));
        QVERIFY(put(other.filePath("note.txt"), "UNRELATED"));
        const QJsonArray history{message("[Old](path:ws1/note.txt)")};
        QVERIFY(seed(data.path(), history, workspace.path()));
        Provider provider;
        QVERIFY(provider.save(data.path()));
        MainWindow window(data.path());
        QVERIFY(select(window, "first"));
        auto* view = window.findChild<ConversationView*>();
        click(view, "first", row(history), QUrl("path:ws1/note.txt"));
        QVERIFY(window.findChildren<EditorWidget*>().isEmpty());
        QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("binding"));
        window.submitPrompt("Return a current link");
        QTRY_COMPARE(provider.requests, 1);
        auto* engine = window.findChild<AgentEngine*>();
        QVERIFY(engine);
        QTRY_VERIFY(!engine->isBusy());
        QTRY_VERIFY(view->toPlainText().contains("Open current file"));
        const auto current = engine->displayMessages().last().toObject();
        click(view, "first", current, QUrl("path:ws1/note.txt"));
        QCOMPARE(window.findChildren<EditorWidget*>().size(), 1);
        QCOMPARE(window.findChild<EditorWidget*>()->filePath(),
                 QFileInfo(workspace.filePath("note.txt")).canonicalFilePath());
        click(view, "first", row(history), QUrl("path:ws1/note.txt"));
        QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("binding"));
        window.connectWorkspace(other.path());
        click(view, "first", current, QUrl("path:ws1/note.txt"));
        QCOMPARE(window.findChildren<EditorWidget*>().size(), 1);
        QVERIFY(window.findChild<QLabel*>("sessionStatus")->text().contains("binding"));
    }
    void realMarkdownEmitsScopedIdentityAndToleratesOwnerDeletion() {
        auto view = std::make_unique<ConversationView>();
        const auto rows = AgentEngine::displayHistory(QJsonArray{message("[Color](color:orange)")});
        view->setConversation("first", rows, false);
        auto* markdown = view->findChild<MarkdownParts*>();
        QVERIFY(markdown);
        QSignalSpy scoped(view.get(), &ConversationView::scopedLinkActivated);
        emit markdown->linkActivated(QUrl("color:orange"));
        QCOMPARE(scoped.size(), 1);
        QCOMPARE(scoped.first()[0].toString(), "first");
        QCOMPARE(scoped.first()[1].toString(), rows.first().toObject()["id"].toString());
        connect(view.get(), &ConversationView::scopedLinkActivated, qApp, [&] { view.reset(); });
        emit markdown->linkActivated(QUrl("color:orange"));
        QVERIFY(!view);
    }
    void restartRestoresOnlySavedWorkspaceProvenance() {
        QTemporaryDir data, workspace;
        QVERIFY(put(workspace.filePath("note.txt"), "OWNED"));
        QVERIFY(seed(data.path(), {}, workspace.path()));
        Provider provider;
        QVERIFY(provider.save(data.path()));
        QJsonObject current;
        {
            MainWindow window(data.path());
            QVERIFY(select(window, "first"));
            window.submitPrompt("Return a current link");
            QTRY_COMPARE(provider.requests, 1);
            auto* engine = window.findChild<AgentEngine*>();
            QVERIFY(engine);
            QTRY_VERIFY(!engine->isBusy());
            auto* view = window.findChild<ConversationView*>();
            QTRY_VERIFY(view->toPlainText().contains("Open current file"));
            current = engine->displayMessages().last().toObject();
            click(view, "first", current, QUrl("path:ws1/note.txt"));
            QCOMPARE(window.findChildren<EditorWidget*>().size(), 1);
        }
        MainWindow restarted(data.path());
        QVERIFY(select(restarted, "first"));
        click(restarted.findChild<ConversationView*>(), "first", current, QUrl("path:ws1/note.txt"));
        QCOMPARE(restarted.findChildren<EditorWidget*>().size(),1);
        QCOMPARE(restarted.findChild<EditorWidget*>()->filePath(),QFileInfo(workspace.filePath("note.txt")).canonicalFilePath());
        QCOMPARE(provider.requests, 1);
    }
    void restartRejectsReplacedWorkspaceIdentity(){
        QTemporaryDir data,container;const auto workspace=container.filePath("workspace");QVERIFY(QDir().mkpath(workspace));QVERIFY(put(workspace+"/note.txt","ORIGINAL"));QVERIFY(seed(data.path(),{},workspace));
        Provider provider;QVERIFY(provider.save(data.path()));QJsonObject saved;
        {MainWindow window(data.path());QVERIFY(select(window,"first"));window.submitPrompt("Return a link");QTRY_COMPARE(provider.requests,1);auto* engine=window.findChild<AgentEngine*>();QVERIFY(engine);QTRY_VERIFY(!engine->isBusy());saved=engine->displayMessages().last().toObject();}
        QVERIFY(QDir().rename(workspace,container.filePath("old-workspace")));QVERIFY(QDir().mkpath(workspace));QVERIFY(put(workspace+"/note.txt","REPLACEMENT"));
        MainWindow restarted(data.path());QVERIFY(select(restarted,"first"));click(restarted.findChild<ConversationView*>(),"first",saved,QUrl("path:ws1/note.txt"));QVERIFY(restarted.findChildren<EditorWidget*>().isEmpty());QVERIFY(restarted.findChild<QLabel*>("sessionStatus")->text().contains("binding"));
    }
    void trustedAttachmentImageUsesHostResolver(){
        QTemporaryDir data;QImage image(32,24,QImage::Format_ARGB32);image.fill(QColor("#00b8c8"));QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));
        const auto ref=AttachmentStore(data.path()).write("first","diagram.png",bytes);QVERIFY(!ref.path.isEmpty());QVERIFY(seed(data.path(),{message("![diagram](path:"+ref.path+")")}));
        MainWindow window(data.path());QVERIFY(select(window,"first"));auto* view=window.findChild<ConversationView*>();
        auto imageCount=[&]{int count=0;for(auto* text:view->findChildren<TranscriptText*>())count+=text->document()->property("trustedImageCount").toInt();return count;};
        QTRY_COMPARE(imageCount(),1);click(view,"first",row({message("![diagram](path:"+ref.path+")")}),QUrl("path:"+ref.path));QVERIFY(window.findChild<ImagePreview*>());
        QVERIFY(select(window,"second"));QTest::qWait(50);QCOMPARE(imageCount(),0);
    }
    void hostRejectsSvgAndUnboundWorkspaceImages(){
        QTemporaryDir data,workspace;const auto ref=AttachmentStore(data.path()).write("first","diagram.svg","<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\" fill=\"red\"/></svg>");QVERIFY(!ref.path.isEmpty());QImage image(10,10,QImage::Format_ARGB32);image.fill(Qt::red);QVERIFY(image.save(workspace.filePath("image.png")));
        QVERIFY(seed(data.path(),{message("![svg](path:"+ref.path+") ![unbound](path:ws1/image.png)")},workspace.path()));MainWindow window(data.path());QVERIFY(select(window,"first"));QTest::qWait(100);
        for(auto* text:window.findChild<ConversationView*>()->findChildren<TranscriptText*>())QCOMPARE(text->document()->property("trustedImageCount").toInt(),0);
    }
    void liveTabsRejectUnknownCrossChatAndClosedMiniApps() {
        QTemporaryDir data;
        const QJsonArray history{message("[Legacy tab](tab:fixture-live)")};
        QVERIFY(seed(data.path(), history));
        Provider provider;
        provider.reply = "[Live](tab:fixture-live) [Foreign](tab:fixture-foreign) [App](tab:fixture-app)";
        QVERIFY(provider.save(data.path()));
        auto window = std::make_unique<MainWindow>(data.path());
        QVERIFY(select(*window, "first"));
        auto* live = tab(*window, "fixture-live");
        tab(*window, "fixture-foreign", "second");
        auto* mini = tab(*window, "fixture-app", "first", true);
        auto* tabs = window->findChild<QTabWidget*>("contentTabs");
        auto* neutral = new QWidget;
        tabs->addTab(neutral, "Neutral");
        tabs->setCurrentWidget(neutral);
        auto* view = window->findChild<ConversationView*>();
        click(view, "first", row(history), QUrl("tab:fixture-live"));
        QCOMPARE(tabs->currentWidget(), neutral);
        window->submitPrompt("Reference browser tabs");
        QTRY_COMPARE(provider.requests, 1);
        auto* engine = window->findChild<AgentEngine*>();
        QVERIFY(engine);
        QTRY_VERIFY(!engine->isBusy());
        QTRY_VERIFY(view->toPlainText().contains("fixture-foreign"));
        const auto current = engine->displayMessages().last().toObject();
        click(view, "first", current, QUrl("tab:fixture-live"));
        QCOMPARE(tabs->currentWidget(), live);
        tabs->setCurrentWidget(neutral);
        click(view, "first", current, QUrl("tab:fixture-foreign"));
        QCOMPARE(tabs->currentWidget(), neutral);
        click(view, "first", current, QUrl("tab:missing"));
        QCOMPARE(tabs->currentWidget(), neutral);
        click(view, "first", current, QUrl("tab:fixture-app"));
        QCOMPARE(tabs->currentWidget(), mini);
        tabs->setCurrentWidget(neutral);
        tabs->removeTab(tabs->indexOf(mini));
        delete mini;
        const auto count = tabs->count();
        click(view, "first", current, QUrl("tab:fixture-app"));
        QCOMPARE(tabs->currentWidget(), neutral);
        QCOMPARE(tabs->count(), count);
        connect(tabs, &QTabWidget::currentChanged, qApp, [&](int index) {
            if (window && tabs->widget(index) == live)
                window.reset();
        });
        click(view, "first", current, QUrl("tab:fixture-live"));
        QVERIFY(!window);
    }
    void closedOrdinaryTabReopensOnlyCapturedLoopbackUrl() {
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("Real WebView fixture requires QT_QPA_PLATFORM=windows; run the same suite with the native "
                  "Windows platform.");
        QTemporaryDir data, profiles;
        QVERIFY(seed(data.path(), {}));
        Provider provider;
        provider.reply = "[Original](tab:fixture-original)";
        QVERIFY(provider.save(data.path()));
        QTcpServer pages;
        QVERIFY(pages.listen(QHostAddress::LocalHost));
        int loads = 0;
        connect(&pages, &QTcpServer::newConnection, &pages, [&] {
            while (auto* socket = pages.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    socket->readAll();
                    ++loads;
                    const QByteArray body =
                        "<!doctype html><title>Local tab fixture</title><p>LOCAL ONLY</p>";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        const QUrl original(QString("http://127.0.0.1:%1/original").arg(pages.serverPort()));
        auto browser = std::make_unique<BrowserWidget>();
        browser->setAttribute(Qt::WA_DontShowOnScreen);
        browser->setProfileDirectory(profiles.path());
        browser->resize(500, 300);
        browser->navigate(original);
        browser->show();
        QTRY_COMPARE_WITH_TIMEOUT(browser->currentUrl(), original, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady(), 15000);
        MainWindow window(data.path());
        window.setAttribute(Qt::WA_DontShowOnScreen);
        QVERIFY(select(window, "first"));
        auto* page = tab(window, "fixture-original", {}, false, browser.release());
        auto* tabs = window.findChild<QTabWidget*>("contentTabs");
        auto* view = window.findChild<ConversationView*>();
        window.submitPrompt("Reference original browser tab");
        QTRY_COMPARE(provider.requests, 1);
        auto* engine = window.findChild<AgentEngine*>();
        QVERIFY(engine);
        QTRY_VERIFY(!engine->isBusy());
        QTRY_VERIFY(view->toPlainText().contains("fixture-original"));
        const auto current = engine->displayMessages().last().toObject();
        const auto index = tabs->indexOf(page);
        emit tabs->tabCloseRequested(index);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!window.findChild<BrowserToolBridge*>()->canAccessTab("fixture-original", "first"));
        const auto count = tabs->count();
        click(view, "first", current, QUrl("tab:fixture-original"));
        QCOMPARE(tabs->count(), count + 1);
        auto* reopened = tabs->currentWidget()->findChild<BrowserWidget*>();
        QVERIFY(reopened);
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(reopened->currentUrl(), original, 15000);
        QTRY_VERIFY(loads >= 2);
    }
    void ordinaryLinkToleratesOwnerDeletion_data() {
        QTest::addColumn<bool>("atRegistration");
        QTest::newRow("registration") << true;
        QTest::newRow("activation") << false;
    }
    void ordinaryLinkToleratesOwnerDeletion() {
        QFETCH(bool, atRegistration);
        QTemporaryDir data;
        const QJsonArray history{message("[Local](http://127.0.0.1:9/never-navigated)")};
        QVERIFY(seed(data.path(), history));
        auto window = std::make_unique<MainWindow>(data.path());
        QVERIFY(select(*window, "first"));
        auto* view = window->findChild<ConversationView*>();
        QPointer<BrowserWidget> provisionalBrowser;
        bool wasOwned = false;
        const auto dispose = [&] {
            if (window) {
                provisionalBrowser = window->findChild<BrowserWidget*>();
                wasOwned = bool(provisionalBrowser);
                window.reset();
            }
        };
        if (atRegistration)
            connect(window->findChild<BrowserToolBridge*>(), &BrowserToolBridge::tabsChanged, qApp, dispose);
        else
            connect(window->findChild<QTabWidget*>("contentTabs"), &QTabWidget::currentChanged, qApp,
                    [&](int) { dispose(); });
        click(view, "first", row(history), QUrl("http://127.0.0.1:9/never-navigated"));
        QVERIFY(!window);
        QVERIFY(wasOwned);
        QVERIFY(!provisionalBrowser);
    }
void nonBrowserContentClearsActiveBrowserContext() {
  QTemporaryDir data;
  QVERIFY(seed(data.path(), {}));
  MainWindow window(data.path());
  QVERIFY(select(window, "first"));
  auto *browserPage = tab(window, "active-fixture");
  auto *content = window.findChild<QTabWidget *>("contentTabs");
  auto *bridge = window.findChild<BrowserToolBridge *>();
  content->setCurrentWidget(browserPage);
  QCOMPARE(bridge->activeTabId(), "active-fixture");
  auto *file = new QWidget;
  content->addTab(file, "Fixture editor");
  content->setCurrentWidget(file);
  QVERIFY2(bridge->activeTabId().isEmpty(), qPrintable(bridge->activeTabId()));
  QVERIFY(bridge->snapshotForAgent("first").activeTabId.isEmpty());
}

void browserCurrentRemovalAndReentryUseLatestSelection() {
  QTemporaryDir data;
  QVERIFY(seed(data.path(), {}));
  MainWindow window(data.path());
  QVERIFY(select(window, "first"));
  auto *a = tab(window, "first-context");
  auto *b = tab(window, "second-context");
  auto *content = window.findChild<QTabWidget *>("contentTabs");
  auto *bridge = window.findChild<BrowserToolBridge *>();
  auto *neutral = new QWidget;
  content->addTab(neutral, "Neutral");
  content->setCurrentWidget(neutral);
  bool once = true;
  connect(bridge, &BrowserToolBridge::tabsChanged, &window, [&] {
    if (once && bridge->activeTabId() == "first-context") {
      once = false;
      content->setCurrentWidget(b);
    }
  });
  content->setCurrentWidget(a);
  QCOMPARE(content->currentWidget(), b);
  QCOMPARE(bridge->activeTabId(), "second-context");
  content->setCurrentWidget(neutral);
  auto *background = tab(window, "background-context");
  QVERIFY(bridge->activeTabId().isEmpty());
  content->setCurrentWidget(background);
  content->removeTab(content->indexOf(background));
  delete background;
  const auto *current = content->currentWidget();
  QCOMPARE(bridge->activeTabId(),
           current ? current->property("browserTabId").toString() : QString());
  while (content->count() > 0) {
    auto *page = content->widget(0);
    content->removeTab(0);
    page->setParent(&window);
  }
  QVERIFY(bridge->activeTabId().isEmpty());
}
void renderedTabAnchorActivatesOnlyCurrentOwnedTab() {
  QTemporaryDir data;
  QVERIFY(seed(data.path(), {}));
  Provider provider;
  provider.reply =
      "[Visible browser](tab:owned-link) [Foreign browser](tab:other-link)";
  QVERIFY(provider.save(data.path()));
  MainWindow window(data.path());
  QVERIFY(select(window, "first"));
  auto *owned = tab(window, "owned-link", "first", true);
  tab(window, "other-link", "second", true);
  auto *tabs = window.findChild<QTabWidget *>("contentTabs");
  auto *neutral = new QWidget;
  tabs->addTab(neutral, "Neutral");
  tabs->setCurrentWidget(neutral);
  window.submitPrompt("Return descriptive links");
  QTRY_COMPARE(provider.requests, 1);
  auto *engine = window.findChild<AgentEngine *>();
  QVERIFY(engine);
  QTRY_VERIFY(!engine->isBusy());
  auto *view = window.findChild<ConversationView *>();
  TranscriptText *target = nullptr;
  bool anchor = false;
  for (auto *text : view->findChildren<TranscriptText *>())
    if (text->source().contains("[Visible browser]")) {
      target = text;
      for (auto block = text->document()->begin(); block.isValid();
           block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
          if (it.fragment().charFormat().anchorHref() == "tab:owned-link")
            anchor = true;
    }
  QVERIFY(target && anchor);
  // Upstream AttachmentLinkRouter renders tab: links as a TabMentionBadge whose
  // label is the tab title or, without one, the tab id; the link text is ignored.
  QVERIFY2(target->toPlainText().contains("owned-link"),
           qPrintable(target->toPlainText()));
  QVERIFY(!target->toPlainText().contains("Visible browser"));
  emit target->anchorClicked(QUrl("tab:owned-link"));
  QCOMPARE(tabs->currentWidget(), owned);
  tabs->setCurrentWidget(neutral);
  emit target->anchorClicked(QUrl("tab:other-link"));
  QCOMPARE(tabs->currentWidget(), neutral);
  owned->findChild<BrowserWidget *>()->setProperty("adeOwnerAgentId", "second");
  emit target->anchorClicked(QUrl("tab:owned-link"));
  QCOMPARE(tabs->currentWidget(), neutral);
}
};
QTEST_MAIN(TranscriptLinkUiTest)
#include "test_transcript_link_ui.moc"
