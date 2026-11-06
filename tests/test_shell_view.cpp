#include "shell/SessionLogStore.h"
#include "shell/ShellService.h"
#include "terminal/TerminalWidget.h"
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
class ShellViewTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QVERIFY(QFontDatabase::addApplicationFont(
                QFileInfo(QString::fromUtf8(__FILE__))
                    .absoluteDir()
                    .filePath("../assets/fonts/RobotoMono-Variable.ttf")) >= 0);
  }
  void tallBorrowedViewPaintsHistoryBeyondFixedGrid() {
    TerminalModel model({10, 24});
    ConPtyProcess process;
    TerminalWidget view(&model, &process);
    view.resize(300, view.cellSize().height() * 40 + 16);
    view.show();
    QTest::qWait(30);
    QByteArray lines;
    for (int i = 0; i < 55; ++i)
      lines += "\x1b[41mX\x1b[0m\r\n";
    model.feed(lines);
    QTest::qWait(30);
    const int top = view.verticalScrollBar()->value(), row = 30;
    QVERIFY(top + row < model.totalLines());
    const auto expected = model.cell(top + row, 0).background;
    QVERIFY(expected != model.background());
    const auto rendered = view.viewport()->grab().toImage();
    QCOMPARE(rendered.pixelColor(9, 8 + row * view.cellSize().height() +
                                        view.cellSize().height() - 2),
             expected);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier,
                      {9, 8 + row * view.cellSize().height() + 2});
    QCOMPARE(view.selectedText(), QString("X"));
  }
  void borrowedViewSharesInputAndSurvivesClosure() {
    QTemporaryDir data;
    auto logs = std::make_shared<SessionLogStore>(data.path(), "chat");
    ShellService service("chat", logs);
    QVERIFY(service.setMounts({{"ws1", data.path()}}));
    QSignalSpy results(&service, &ShellService::completed);
    service.execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 15000);
    QVERIFY(!results[0][2].toBool());
    const auto id = QJsonDocument::fromJson(results[0][1].toString().toUtf8())
                        .object()["session_id"]
                        .toString();
    QVERIFY(service.ownsSession(id));
    QVERIFY(!service.ownsSession("foreign"));
    QVERIFY(!service.sessionModel("foreign"));
    auto *model = service.sessionModel(id);
    auto *process = service.sessionProcess(id);
    QVERIFY(model);
    QVERIFY(process);
    const auto cells = model->size();
    auto *first = new TerminalWidget(model, process);
    first->resize(600, 300);
    first->show();
    QTest::qWait(30);
    QVERIFY(first->borrowsSession());
    QCOMPARE(model->size(), cells);
    QString error;
    QVERIFY(!first->start(data.path(), &error));
    QVERIFY(!error.isEmpty());
    TerminalWidget second(model, process);
    second.resize(800, 450);
    second.show();
    first->setFocus();
    QTest::keyClicks(first, "Write-Output ('VIEW_' + 'PRINTED_ONCE')");
    QTest::keyClick(first, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(model->plainText().contains("VIEW_PRINTED_ONCE"),
                             5000);
    QCOMPARE(model->plainText().count("VIEW_PRINTED_ONCE"), 1);
    QVERIFY(first->verticalScrollBar()->value()<=model->historyLines()+model->cursor().y());
    const auto rendered = first->grab();
    QVERIFY(!rendered.isNull());
    if (const auto path = qEnvironmentVariable("ADE_SHELL_RENDER_PATH");
        !path.isEmpty())
      QVERIFY(rendered.save(path));
    first->stop();
    QVERIFY(process->isRunning());
    delete first;
    QVERIFY(process->isRunning());
    QCOMPARE(second.terminalModel(), model);
    QCOMPARE(model->size(), cells);
    second.setFocus();
    QTest::keyClicks(&second, "Start-Sleep -Seconds 60");
    QTest::keyClick(&second, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(service.snapshot()["sessions"]
                                 .toArray()[0]
                                 .toObject()["commandActive"]
                                 .toBool(),
                             5000);
    service.execute("kill", "executeShellCommand",
                    {{"session_id", id},
                     {"explanation", "Fixture cleanup"},
                     {"kill", true}});
    QVERIFY(!second.isRunning());
    QVERIFY(!service.snapshot()["sessions"]
                 .toArray()[0]
                 .toObject()["commandActive"]
                 .toBool());
  }
  void ownerDeletionMakesBorrowedViewInert() {
    QTemporaryDir data;
    auto logs = std::make_shared<SessionLogStore>(data.path(), "chat");
    auto *service = new ShellService("chat", logs);
    QVERIFY(service->setMounts({{"ws1", data.path()}}));
    QSignalSpy results(service, &ShellService::completed);
    service->execute("create", "createShellSession", {{"cwd", "ws1"}});
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 15000);
    const auto id = QJsonDocument::fromJson(results[0][1].toString().toUtf8())
                        .object()["session_id"]
                        .toString();
    TerminalWidget view(service->sessionModel(id), service->sessionProcess(id));
    view.resize(500, 250);
    view.show();
    delete service;
    QVERIFY(!view.isRunning());
    QVERIFY(!view.terminalModel());
    QVERIFY(!view.process());
    view.resize(640, 400);
    view.setDarkTheme(false);
    view.setZoomPercent(125);
    view.stop();
    QVERIFY(view.selectedText().isEmpty());
    view.pasteClipboard();
    QKeyEvent ignored(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
    QApplication::sendEvent(&view, &ignored);
    QVERIFY(!view.grab().isNull());
  }
};
QTEST_MAIN(ShellViewTest)
#include "test_shell_view.moc"
