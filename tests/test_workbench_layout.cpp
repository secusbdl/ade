#include "browser/browserwidget.h"
#include "editor/CodeEditor.h"
#include "editor/EditorWidget.h"
#include "terminal/TerminalWidget.h"
#include "ui/AppStyle.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/MainWindow.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include <QComboBox>
#include <QFileSystemModel>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>
using namespace ade;
namespace {
bool seed(const QString& directory, bool history = false, const QString& workspace = {}) {
    Conversation chat;
    chat.id = "fixture";
    chat.title = "Layout fixture";
    chat.workspace = workspace;
    if (history)
        chat.messages = {QJsonObject{{"role", "user"}, {"content", "Read the fixture"}},
                         QJsonObject{{"role", "assistant"}, {"content", "READY"}}};
    return SessionStore(directory).save({chat});
}
void expose(MainWindow& window, QSize size = {1540, 940}) {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(size);
    window.show();
    QTest::qWait(40);
}
QToolButton* action(MainWindow& window, const QString& name) {
    return window.findChild<QToolButton*>(name);
}
QTabWidget* tabs(MainWindow& window) {
    return window.findChild<QTabWidget*>("contentTabs");
}
QSplitter* split(MainWindow& window) {
    return window.findChild<QSplitter*>("workspaceSplitter");
}
void browser(MainWindow& window) {
    if (auto* button = action(window, "openBrowserButton"); button && button->isVisible())
        button->click();
    else if (auto* button = action(window, "newBrowserTabButton"))
        button->click();
    QCoreApplication::processEvents();
}
QRect in(QWidget* child, QWidget* parent) {
    return QRect(child->mapTo(parent, QPoint()), child->size());
}
QJsonArray readRows(const QString& path) {
    return {
        QJsonObject{
            {"id", "read-row"},
            {"role", "assistant"},
            {"parts", QJsonArray{QJsonObject{{"id", "read-part"},
                                             {"type", "tool"},
                                             {"name", "read"},
                                             {"toolCallId", "read-call"},
                                             {"state", "output-available"},
                                             {"input", QJsonObject{{"path", path}}},
                                             {"output", "File content retained for the provider."}}}}},
        QJsonObject{
            {"id", "ready-row"},
            {"role", "assistant"},
            {"parts", QJsonArray{QJsonObject{
                          {"id", "ready-part"}, {"type", "text"}, {"text", "READY"}, {"state", "done"}}}}}};
}
} // namespace
class WorkbenchLayoutTest final : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        initializeAppStyle(*qApp);
    }
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void freshChatHasNoContent() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window);
        QCOMPARE(tabs(window)->count(), 0);
        QVERIFY(!tabs(window)->isVisible());
        auto* open = action(window, "openBrowserButton");
        QVERIFY(open && open->isVisible());
    }
    void composerCenteredUntilHistory() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow empty(data.path());
        expose(empty);
        auto* chat = empty.findChild<QWidget*>("chatSurface");
        auto* footer = empty.findChild<QWidget*>("chatFooter");
        QVERIFY(footer);
        QVERIFY(footer->width() <= 768);
        QVERIFY(qAbs(in(footer, chat).center().x() - chat->rect().center().x()) < 3);
        QVERIFY(qAbs(in(footer, chat).center().y() - chat->rect().center().y()) < 25);
        QTemporaryDir populated;
        QVERIFY(seed(populated.path(), true));
        MainWindow filled(populated.path());
        expose(filled);
        chat = filled.findChild<QWidget*>("chatSurface");
        footer = filled.findChild<QWidget*>("chatFooter");
        QVERIFY(chat->height() - in(footer, chat).bottom() < 12);
        QVERIFY(filled.findChild<ConversationView*>()->isVisible());
    }
    void openHideRestoreAndLastClose() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window);
        browser(window);
        QCOMPARE(tabs(window)->count(), 1);
        QVERIFY(tabs(window)->isVisible());
        auto* page = tabs(window)->currentWidget();
        auto* address = page->findChild<QLineEdit*>("browserAddress");
        QVERIFY(address);
        QVERIFY(address->hasFocus());
        address->setText("unsent address");
        action(window, "hideContentPanelButton")->click();
        QVERIFY(!tabs(window)->isVisible());
        QCOMPARE(tabs(window)->count(), 1);
        QVERIFY(action(window, "showContentPanelButton")->isVisible());
        action(window, "showContentPanelButton")->click();
        QVERIFY(tabs(window)->isVisible());
        QCOMPARE(tabs(window)->currentWidget(), page);
        QCOMPARE(address->text(), QString("unsent address"));
        tabs(window)->tabCloseRequested(0);
        QCOMPARE(tabs(window)->count(), 0);
        QVERIFY(!tabs(window)->isVisible());
        QVERIFY(action(window, "openBrowserButton")->isVisible());
    }
    void fullscreenResizePreservesHiddenChrome() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window);
        action(window, "toggleFileTreeButton")->click();
        browser(window);
        auto* embedded = tabs(window)->currentWidget()->findChild<BrowserWidget*>();
        QVERIFY(embedded);
        emit embedded->fullscreenChanged(true);
        QTest::qWait(30);
        window.resize(1200, 700);
        QCoreApplication::processEvents();
        QVERIFY(!window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QVERIFY(!window.findChild<QWidget*>("sidebar")->isVisible());
        emit embedded->fullscreenChanged(false);
        QTest::qWait(30);
        QVERIFY(window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QVERIFY(window.findChild<QWidget*>("chatSurface")->isVisible());
    }
    void terminalSurvivesHideAndReopen() {
        QTemporaryDir data, workspace;
        QVERIFY(seed(data.path(), false, workspace.path()));
        MainWindow window(data.path());
        expose(window);
        auto* terminal = new TerminalWidget;
        QString error;
        QVERIFY2(terminal->start(workspace.path(), &error,
                                 {qEnvironmentVariable("SystemRoot") + "/System32/cmd.exe", "/D", "/Q"}),
                 qPrintable(error));
        tabs(window)->setCurrentIndex(tabs(window)->addTab(terminal, "Fixture terminal"));
        QTRY_VERIFY_WITH_TIMEOUT(terminal->isRunning(), 3000);
        const auto processId = terminal->processId();
        QVERIFY(processId);
        action(window, "hideContentPanelButton")->click();
        QVERIFY(terminal->isRunning());
        action(window, "showContentPanelButton")->click();
        QCOMPARE(tabs(window)->currentWidget(), terminal);
        QCOMPARE(terminal->processId(), processId);
        terminal->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!terminal->isRunning(), 5000);
    }
    void contentWidthAndCollapsePersist() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        double expected = 0;
        {
            MainWindow window(data.path());
            expose(window);
            browser(window);
            auto* pane = split(window);
            pane->setSizes({600, 400, 0});
            emit pane->splitterMoved(600, 1);
            const auto sizes = pane->sizes();
            expected = double(sizes[1]) / (sizes[0] + sizes[1]);
            action(window, "hideContentPanelButton")->click();
            QVERIFY(window.close());
        }
        QSettings prefs(data.filePath("window.ini"), QSettings::IniFormat);
        QVERIFY(prefs.value("contentCollapsed").toBool());
        MainWindow restored(data.path());
        expose(restored);
        QCOMPARE(tabs(restored)->count(), 0);
        browser(restored);
        QVERIFY(tabs(restored)->isVisible());
        const auto sizes = split(restored)->sizes();
        QVERIFY(qAbs(double(sizes[1]) / (sizes[0] + sizes[1]) - expected) < .02);
    }
    void legacySplitMigratesWithoutOpeningContent() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        {
            QSplitter old;
            old.addWidget(new QWidget);
            old.addWidget(new QWidget);
            old.resize(1000, 600);
            old.setSizes({650, 350});
            QSettings settings(data.filePath("window.ini"), QSettings::IniFormat);
            settings.setValue("splitter", old.saveState());
        }
        MainWindow window(data.path());
        expose(window);
        QCOMPARE(tabs(window)->count(), 0);
        browser(window);
        const auto sizes = split(window)->sizes();
        QVERIFY(qAbs(double(sizes[1]) / (sizes[0] + sizes[1]) - .35) < .02);
        QVERIFY(window.close());
        QSettings settings(data.filePath("window.ini"), QSettings::IniFormat);
        QVERIFY(settings.contains("contentPanelPercent"));
    }
    void filesIndependentWithoutCreatingBrowser() {
        QTemporaryDir data, workspace;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window);
        action(window, "toggleFileTreeButton")->click();
        auto* panel = window.findChild<QWidget*>("fileTreePanel");
        QVERIFY(panel && panel->isVisible());
        QVERIFY(panel->findChild<QWidget*>("fileTreeEmpty")->isVisible());
        QCOMPARE(tabs(window)->count(), 0);
        window.connectWorkspace(workspace.path());
        QVERIFY(!panel->findChild<QWidget*>("fileTreeEmpty")->isVisible());
        QVERIFY(window.findChild<QTreeView*>("workspaceFiles")->isVisible());
        browser(window);
        QVERIFY(panel->isVisible());
        action(window, "hideContentPanelButton")->click();
        QVERIFY(panel->isVisible());
        QVERIFY(!tabs(window)->isVisible());
        action(window, "closeFileTreeButton")->click();
        QVERIFY(!panel->isVisible());
    }
    void hiddenDirtyEditorIsPreservedAndRevealed() {
        QTemporaryDir data, workspace;
        QFile file(workspace.filePath("fixture.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original\n");
        file.close();
        QVERIFY(seed(data.path(), false, workspace.path()));
        MainWindow window(data.path());
        expose(window);
        action(window, "toggleFileTreeButton")->click();
        auto* tree = window.findChild<QTreeView*>("workspaceFiles");
        auto* model = qobject_cast<QFileSystemModel*>(tree->model());
        QVERIFY(model);
        const auto index = model->index(file.fileName());
        QVERIFY(index.isValid());
        emit tree->doubleClicked(index);
        QCoreApplication::processEvents();
        auto* editor = qobject_cast<EditorWidget*>(tabs(window)->currentWidget());
        QVERIFY(editor);
        editor->editor()->insertPlainText("unsaved ");
        QVERIFY(editor->isModified());
        action(window, "hideContentPanelButton")->click();
        QVERIFY(!tabs(window)->isVisible());
        QVERIFY(editor->isModified());
        emit tree->doubleClicked(index);
        QVERIFY(tabs(window)->isVisible());
        QCOMPARE(tabs(window)->currentWidget(), editor);
        QVERIFY(editor->editor()->toPlainText().contains("unsaved"));
        editor->editor()->document()->setModified(false);
    }
    void responsiveMinimumAndZoom_data() {
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("zoom");
        QTest::addColumn<int>("height");
        for (int width : {350, 768})
            for (int zoom : {70, 100, 130})
                for (int height : {250, 500})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(width).arg(zoom).arg(height)))
                        << width << zoom << height;
    }
    void responsiveMinimumAndZoom() {
        QFETCH(int, width);
        QFETCH(int, zoom);
        QFETCH(int, height);
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        window.findChild<SettingsPage*>()->setPreference("personalization.uiZoom", zoom);
        expose(window, {width, height});
        QCOMPARE(window.width(), width);
        QCOMPARE(window.height(), height);
        auto* box = window.findChild<QWidget*>("composerBox");
        auto* send = action(window, "sendButton");
        QVERIFY(window.rect().contains(in(box, &window)));
        QVERIFY(box->rect().contains(in(send, box)));
        auto* model = window.findChild<QComboBox*>("modelSelector");
        QVERIFY(box->rect().contains(in(model, box)));
        auto* open = action(window, "openBrowserButton");
        QVERIFY(open->isVisible());
        QVERIFY(window.rect().contains(in(open, &window)));
    }
    void compactContentKeepsCloseAndFilesReachable() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window, {350, 250});
        browser(window);
        QVERIFY(tabs(window)->isVisible());
        QVERIFY(!window.findChild<QWidget*>("chatSurface")->isVisible());
        auto* hide = action(window, "hideContentPanelButton");
        QVERIFY(hide->isVisible());
        QVERIFY(window.rect().contains(in(hide, &window)));
        action(window, "contentFileTreeButton")->click();
        QVERIFY(!tabs(window)->isVisible());
        QVERIFY(window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QVERIFY(window.findChild<QWidget*>("chatSurface")->isVisible());
        action(window, "showContentPanelButton")->click();
        QVERIFY(tabs(window)->isVisible());
        QVERIFY(!window.findChild<QWidget*>("fileTreePanel")->isVisible());
        hide->click();
        QVERIFY(window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QCOMPARE(tabs(window)->count(), 1);
    }
    void resizingRestoresFilesAndPlaceholderFits() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window);
        action(window, "toggleFileTreeButton")->click();
        browser(window);
        window.resize(350, 250);
        QTest::qWait(30);
        QVERIFY(!window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QVERIFY(action(window, "contentFileTreeButton")->isVisible());
        action(window, "contentFileTreeButton")->click();
        auto* input = window.findChild<Composer*>();
        QTest::qWait(30);
        QVERIFY(input->height() >= input->fontMetrics().lineSpacing() * 2);
        window.resize(1540, 940);
        QTest::qWait(30);
        QVERIFY(window.findChild<QWidget*>("fileTreePanel")->isVisible());
        QVERIFY(window.findChild<QWidget*>("chatSurface")->isVisible());
    }
    void longDraftKeepsSendReachableAtMinimumHeight() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window, {350, 250});
        auto* input = window.findChild<Composer*>();
        const auto draft = QString("Long retained draft line\n").repeated(80);
        input->setPlainText(draft);
        QTest::qWait(40);
        QCOMPARE(window.height(), 250);
        auto* send = action(window, "sendButton");
        QVERIFY(window.rect().contains(in(send, &window)));
        QVERIFY(window.rect().contains(in(input, &window)));
        QCOMPARE(input->toPlainText(), draft);
        QVERIFY(input->verticalScrollBar()->maximum() > 0);
    }
    void compactNavigationKeepsWidePreference() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        expose(window, {350, 500});
        auto* shell = window.findChild<QSplitter*>("sidebarSplitter");
        QCOMPARE(shell->sizes()[0], 0);
        auto* reopen = action(window, "sidebarReopenButton");
        QVERIFY(reopen->isVisible());
        reopen->click();
        QVERIFY(window.findChild<QListWidget*>("agentList")->isVisible());
        QVERIFY(!split(window)->isVisible());
        for (auto* button : window.findChildren<QToolButton*>())
            if (button->accessibleName() == "Toggle sidebar")
                button->click();
        QVERIFY(split(window)->isVisible());
        window.resize(1540, 940);
        QTest::qWait(30);
        QVERIFY(shell->sizes()[0] >= 176);
        QSettings prefs(data.filePath("window.ini"), QSettings::IniFormat);
        QVERIFY(!prefs.value("sidebarCollapsed", false).toBool());
    }
    void transcriptBackgroundUsesChatSurface_data() {
        QTest::addColumn<QString>("theme");
        QTest::newRow("light") << QString("light");
        QTest::newRow("dark") << QString("dark");
    }
    void transcriptBackgroundUsesChatSurface() {
        QFETCH(QString, theme);
        QTemporaryDir data;
        QVERIFY(seed(data.path(), true));
        MainWindow window(data.path());
        QVERIFY(window.findChild<SettingsPage*>()->setPreference("personalization.appearance", theme));
        expose(window);
        auto* view = window.findChild<ConversationView*>();
        auto* scroll = view->findChild<QScrollArea*>("transcriptScroll");
        QVERIFY(scroll);
        const auto image = window.grab().toImage();
        const auto point = scroll->viewport()->mapTo(
            &window, QPoint(scroll->viewport()->width() / 2, scroll->viewport()->height() / 2));
        const auto expected = QColor(theme == "dark" ? "#21242b" : "#fdfdfd");
        QCOMPARE(image.pixelColor(point), expected);
    }
    void compactReadMountLabels_data() {
        QTest::addColumn<QString>("path");
        for (const auto& path : {"ws1/probe.txt", "w1/probe.txt", "w123abcf/probe.txt"})
            QTest::newRow(path) << QString(path);
    }
    void compactReadMountLabels() {
        QFETCH(QString, path);
        QTemporaryDir data;
        QVERIFY(seed(data.path(), true));
        MainWindow window(data.path());
        expose(window);
        auto* view = window.findChild<ConversationView*>();
        view->setConversation("fixture", readRows(path), false);
        QTest::qWait(20);
        bool found = false;
        for (auto* button : view->findChildren<QToolButton*>())
            if (button->text() == "Read probe.txt")
                found = true;
        QVERIFY2(found, qPrintable("Compact label did not strip " + path));
    }
    void arbitraryFolderIsNotTreatedAsNativeMount() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), true));
        MainWindow window(data.path());
        expose(window);
        auto* view = window.findChild<ConversationView*>();
        view->setConversation("fixture", readRows("ws19/probe.txt"), false);
        QTest::qWait(20);
        bool found = false;
        for (auto* button : view->findChildren<QToolButton*>())
            if (button->text() == "Read ws19/probe.txt")
                found = true;
        QVERIFY(found);
    }
    void messageRowsStayCenteredAndBounded() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), true));
        MainWindow window(data.path());
        expose(window);
        auto* view = window.findChild<ConversationView*>();
        view->setConversation("fixture", readRows("ws1/probe.txt"), false);
        QTest::qWait(30);
        auto* row = view->findChild<QWidget*>("message_ready-row");
        QVERIFY(row);
        auto* viewport = view->findChild<QScrollArea*>("transcriptScroll")->viewport();
        QVERIFY(row->width() <= 768);
        QVERIFY(qAbs(in(row, viewport).center().x() - viewport->rect().center().x()) < 3);
    }
};
QTEST_MAIN(WorkbenchLayoutTest)
#include "test_workbench_layout.moc"
