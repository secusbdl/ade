#include "JsonRpcProcess.h"
#include <QCoreApplication>
#include <QDir>
#include <QSignalSpy>
#include <QtTest>
using namespace ade;
class DeadlineTests : public QObject {
  Q_OBJECT
private slots:
  void onlyOwningRequestPauses() {
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started),
        responses(&process, &JsonRpcProcess::response);
    QVERIFY(process.start(RPC_HELPER, {}, QDir::tempPath(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    const auto held = process.request("hold", {}, 150);
    QVERIFY(!held.isEmpty());
    QVERIFY(process.suspendDeadline(held));
    const auto ordinary = process.request("hold", {}, 150);
    QVERIFY(!ordinary.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(responses.size(), 1, 1000);
    QCOMPARE(responses[0][0].toString(), ordinary);
    QTest::qWait(250);
    QCOMPARE(responses.size(), 1);
    QVERIFY(process.isRunning());
    QVERIFY(process.resumeDeadline(held));
    QTRY_COMPARE_WITH_TIMEOUT(responses.size(), 2, 1000);
    QCOMPARE(responses[1][0].toString(), held);
    QVERIFY(!process.resumeDeadline(held));
  }
  void resumesRemainingBudgetInsteadOfFullTimeout() {
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started),
        responses(&process, &JsonRpcProcess::response);
    QVERIFY(process.start(RPC_HELPER, {}, QDir::tempPath(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    const auto held = process.request("hold", {}, 1400);
    QTest::qWait(850);
    QVERIFY(process.suspendDeadline(held));
    QTest::qWait(800);
    QCOMPARE(responses.size(), 0);
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(process.resumeDeadline(held));
    QTRY_COMPARE_WITH_TIMEOUT(responses.size(), 1, 1100);
    QVERIFY2(
        elapsed.elapsed() < 1100,
        "Resumption must preserve the remaining budget, not reset to 1400 ms.");
    QCOMPARE(responses[0][0].toString(), held);
  }
  void processExitRemainsImmediateWhileWaiting() {
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started),
        failed(&process, &JsonRpcProcess::failed);
    QVERIFY(process.start(RPC_HELPER, {}, QDir::tempPath(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    const auto held = process.request("hold", {}, 100);
    QVERIFY(process.suspendDeadline(held));
    QVERIFY(!process.request("exit", {}, 100).isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 1000);
    QVERIFY(!process.isRunning());
    QVERIFY(!process.resumeDeadline(held));
  }
  void realReplyClearsSuspension() {
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started),
        responses(&process, &JsonRpcProcess::response);
    QVERIFY(process.start(RPC_HELPER, {}, QDir::tempPath(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    const auto id = process.request("ping", {}, 100);
    QVERIFY(process.suspendDeadline(id));
    QTRY_COMPARE(responses.size(), 1);
    QVERIFY(!process.resumeDeadline(id));
    QTest::qWait(250);
    QCOMPARE(responses.size(), 1);
  }
  void explicitStopClearsHeldState() {
    JsonRpcProcess process;
    QSignalSpy started(&process, &JsonRpcProcess::started);
    QVERIFY(process.start(RPC_HELPER, {}, QDir::tempPath(),
                          JsonRpcProcess::managedEnvironment()));
    QTRY_COMPARE(started.size(), 1);
    const auto held = process.request("hold", {}, 100);
    QVERIFY(process.suspendDeadline(held));
    process.stop();
    QVERIFY(!process.isRunning());
    QVERIFY(!process.resumeDeadline(held));
    QVERIFY(!process.suspendDeadline("unknown"));
  }
};
QTEST_GUILESS_MAIN(DeadlineTests)
#include "test_rpc_deadline.moc"
