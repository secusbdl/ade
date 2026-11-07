// Keep-awake guard. Before this change agent.keepAwake was stored but nothing
// consumed it: PowerRequestGuard did not exist, so no test could observe an
// acquire or release. A fake backend records calls; the real Windows backend
// is exercised only when ADE_POWER_SMOKE=1, so CTest never changes the
// machine's power state.
#include "ui/PowerRequest.h"
#include <QtTest>
#include <memory>

using namespace ade;

namespace {
struct Calls {
    int acquires = 0;
    int releases = 0;
    bool held = false;
    bool fail = false;
};
class FakeBackend final : public IPowerBackend {
public:
    explicit FakeBackend(std::shared_ptr<Calls> calls) : m_calls(std::move(calls)) {}
    bool acquire() override {
        ++m_calls->acquires;
        if (m_calls->fail) return false;
        m_calls->held = true;
        return true;
    }
    void release() override {
        ++m_calls->releases;
        m_calls->held = false;
    }

private:
    std::shared_ptr<Calls> m_calls;
};
}

class PowerRequestTest : public QObject {
    Q_OBJECT
private slots:
    void acquireOnlyWhenEnabledAndActive() {
        auto calls = std::make_shared<Calls>();
        PowerRequestGuard guard(std::make_unique<FakeBackend>(calls));
        guard.update(true, 0);
        guard.update(false, 3);
        QCOMPARE(calls->acquires, 0);
        QVERIFY(!guard.held());
        guard.update(true, 1);
        guard.update(true, 2);
        guard.update(true, 1);
        QCOMPARE(calls->acquires, 1);
        QVERIFY(guard.held() && calls->held);
        guard.update(true, 0);
        QCOMPARE(calls->releases, 1);
        QVERIFY(!guard.held() && !calls->held);
        guard.update(true, 0);
        QCOMPARE(calls->releases, 1);
    }
    void disablingReleasesImmediately() {
        auto calls = std::make_shared<Calls>();
        PowerRequestGuard guard(std::make_unique<FakeBackend>(calls));
        guard.update(true, 2);
        QVERIFY(guard.held());
        guard.update(false, 2);
        QVERIFY(!guard.held());
        QCOMPARE(calls->releases, 1);
        guard.update(true, 2);
        QCOMPARE(calls->acquires, 2);
    }
    void destructorReleases() {
        auto calls = std::make_shared<Calls>();
        {
            PowerRequestGuard guard(std::make_unique<FakeBackend>(calls));
            guard.update(true, 1);
        }
        QCOMPARE(calls->acquires, 1);
        QCOMPARE(calls->releases, 1);
        QVERIFY(!calls->held);
        {
            PowerRequestGuard idle(std::make_unique<FakeBackend>(calls));
        }
        QCOMPARE(calls->releases, 1); // nothing held, nothing released
    }
    void acquireFailureIsRetriedOnNextUpdate() {
        auto calls = std::make_shared<Calls>();
        PowerRequestGuard guard(std::make_unique<FakeBackend>(calls));
        calls->fail = true;
        guard.update(true, 1);
        QVERIFY(!guard.held());
        QCOMPARE(calls->acquires, 1);
        calls->fail = false;
        guard.update(true, 1);
        QVERIFY(guard.held());
        QCOMPARE(calls->acquires, 2);
        QCOMPARE(calls->releases, 0);
    }
    void nullBackendNeverHolds() {
        PowerRequestGuard guard(std::make_unique<NullPowerBackend>());
        guard.update(true, 4);
        QVERIFY(!guard.held());
    }
    void winBackendRoundTrip() {
        if (qEnvironmentVariable("ADE_POWER_SMOKE") != "1") QSKIP("Set ADE_POWER_SMOKE=1 to take and release a real power request.");
        WinPowerBackend backend;
        QVERIFY(backend.acquire());
        QVERIFY(backend.acquire()); // idempotent while held
        backend.release();
        // A second cycle after release succeeds: the handle was not leaked.
        QVERIFY(backend.acquire());
        backend.release();
        backend.release();
    }
};

QTEST_MAIN(PowerRequestTest)
#include "test_power_request.moc"
