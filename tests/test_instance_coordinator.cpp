// Per-data-directory single instance. Before this change every launch built
// its own MainWindow over the same data directory: InstanceCoordinator did
// not exist, so a second launch could neither detect the first nor forward
// URLs, and none of these tests could build. Coordinators here use temporary
// data directories only, so they never meet a real ADE instance.
#include "ui/InstanceCoordinator.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <QtEndian>
#include <QtTest>

using namespace ade;
using Role = InstanceCoordinator::Role;

namespace {
QByteArray frame(const QByteArray& json) {
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(quint32(json.size()), bytes.data());
    return bytes + json;
}
QByteArray frame(const QJsonObject& object) { return frame(QJsonDocument(object).toJson(QJsonDocument::Compact)); }

// Sends raw bytes from the primary's own thread and pumps events until the
// server closes the connection. Returns every byte the server replied.
QByteArray exchange(const QString& server, const QByteArray& bytes, int timeoutMs = 4000) {
    QLocalSocket socket;
    QByteArray received;
    QObject::connect(&socket, &QLocalSocket::readyRead, [&] { received += socket.readAll(); });
    socket.connectToServer(server);
    if (!socket.waitForConnected(2000)) return "connect-failed";
    socket.write(bytes);
    socket.flush();
    QElapsedTimer clock;
    clock.start();
    while (socket.state() != QLocalSocket::UnconnectedState && clock.elapsed() < timeoutMs) QTest::qWait(10);
    received += socket.readAll();
    return received;
}

QFuture<Role> forwardFromWorker(const QString& directory, const InstanceRequest& request, int timeoutMs = 5000, bool refuse = false) {
    // acquire() blocks; the primary's server needs this thread's event loop.
    return QtConcurrent::run([directory, request, timeoutMs, refuse] {
        InstanceCoordinator client(directory);
        return client.acquire(request, timeoutMs, nullptr, refuse);
    });
}

QString helperPath() {
#ifdef ADE_INSTANCE_CLIENT_HELPER
    return QStringLiteral(ADE_INSTANCE_CLIENT_HELPER);
#else
    return QCoreApplication::applicationDirPath() + "/instance_client_helper.exe";
#endif
}

QJsonObject sidecar(const QString& directory) {
    QFile file(directory + "/ade-instance.json");
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
}

class InstanceCoordinatorTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<ade::InstanceRequest>(); }

    void firstIsPrimaryAndWritesSidecar() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QString error;
        QCOMPARE(primary.acquire({}, 1000, &error), Role::Primary);
        QVERIFY(primary.isPrimary());
        QVERIFY(primary.isServing());
        QVERIFY(QFileInfo::exists(data.filePath("ade-instance.lock")));
        const auto meta = sidecar(data.path());
        QCOMPARE(meta["v"].toInt(), 1);
        QCOMPARE(qint64(meta["pid"].toDouble()), QCoreApplication::applicationPid());
        QCOMPARE(meta["server"].toString(), primary.serverName());
        QVERIFY(QRegularExpression(QRegularExpression::anchoredPattern("ade-[0-9a-f]{32}")).match(primary.serverName()).hasMatch());
        QLocalSocket probe;
        probe.connectToServer(primary.serverName());
        QVERIFY(probe.waitForConnected(2000));
        probe.abort();
    }
    void secondSameDirForwardsAndPrimaryReceives() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QCOMPARE(primary.acquire({}), Role::Primary);
        primary.markReady();
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        auto future = forwardFromWorker(data.path(), {{QUrl("https://example.test/a"), QUrl("http://example.test/b?q=1")}, "C:/work"});
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 10000);
        QCOMPARE(future.result(), Role::Forwarded);
        QCOMPARE(received.size(), 1);
        const auto request = received.first().first().value<InstanceRequest>();
        QCOMPARE(request.urls, QList<QUrl>({QUrl("https://example.test/a"), QUrl("http://example.test/b?q=1")}));
        QCOMPARE(request.cwd, QString("C:/work"));
    }
    void crossProcessForward() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QCOMPARE(primary.acquire({}), Role::Primary);
        primary.markReady();
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        QProcess helper;
        helper.start(helperPath(),
                     {data.path(), "https://one.test/a", "--flag", "javascript:alert(1)", "http://two.test/b"});
        QVERIFY(helper.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(helper.state() == QProcess::NotRunning, 15000);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        QCOMPARE(received.size(), 1);
        QCOMPARE(received.first().first().value<InstanceRequest>().urls,
                 QList<QUrl>({QUrl("https://one.test/a"), QUrl("http://two.test/b")}));
        // An exclusive launch is refused while the primary runs.
        helper.start(helperPath(), {"--refuse", data.path()});
        QVERIFY(helper.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(helper.state() == QProcess::NotRunning, 15000);
        QCOMPARE(helper.exitCode(), 3);
        QCOMPARE(received.size(), 1);
    }
    void differentDataDirsBothPrimary() {
        QTemporaryDir first, second;
        InstanceCoordinator a(first.path()), b(second.path());
        QCOMPARE(a.acquire({}, 500), Role::Primary);
        QCOMPARE(b.acquire({}, 500), Role::Primary);
        QVERIFY(a.serverName() != b.serverName());
    }
    void refusesExclusiveLaunchWhileRunning() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QCOMPARE(primary.acquire({}), Role::Primary);
        primary.markReady();
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        // A running primary answers the probe at once; no activation follows.
        QElapsedTimer clock;
        clock.start();
        auto exclusive = forwardFromWorker(data.path(), {{QUrl("https://never.test/")}, {}}, 8000, true);
        QTRY_VERIFY_WITH_TIMEOUT(exclusive.isFinished(), 5000);
        QCOMPARE(exclusive.result(), Role::Refused);
        QVERIFY(clock.elapsed() < 4000);
        QTest::qWait(50);
        QCOMPARE(received.size(), 0);
        // Without any answer (the primary's thread is blocked) the deadline refuses.
        InstanceCoordinator blocked(data.path());
        QCOMPARE(blocked.acquire({}, 300, nullptr, true), Role::Refused);
        QVERIFY(!blocked.isPrimary());
    }
    void exclusiveLaunchWaitsForClosingPrimary() {
        QTemporaryDir data;
        auto* primary = new InstanceCoordinator(data.path());
        QCOMPARE(primary->acquire({}), Role::Primary);
        primary->markReady();
        primary->setClosing();
        auto exclusive = forwardFromWorker(data.path(), {}, 8000, true);
        QTest::qWait(400);
        QVERIFY(!exclusive.isFinished()); // retrying against a closing primary
        delete primary; // releases the lock
        QTRY_VERIFY_WITH_TIMEOUT(exclusive.isFinished(), 10000);
        QCOMPARE(exclusive.result(), Role::Primary);
    }
    void lockErrorContinuesUncoordinated() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        int attempts = 0;
        primary.setLockFunction([&attempts](QLockFile&) { ++attempts; return QLockFile::PermissionError; });
        QString error;
        QCOMPARE(primary.acquire({}, 1000, &error), Role::Primary);
        QCOMPARE(attempts, 1);
        QVERIFY(!error.isEmpty());
        QVERIFY(!primary.isPrimary());
        QVERIFY(!primary.isServing());
        QVERIFY(!QFileInfo::exists(data.filePath("ade-instance.json")));
        InstanceCoordinator exclusive(data.path());
        exclusive.setLockFunction([](QLockFile&) { return QLockFile::UnknownError; });
        QCOMPARE(exclusive.acquire({}, 1000, nullptr, true), Role::Primary);
    }
    void filtersUrls() {
        const auto longPath = "https://ok.test/" + QString(2100, 'p');
        const auto urls = InstanceCoordinator::filterUrls({"-flag", "--data-dir", "javascript:alert(1)", "file:///C:/x.txt",
                                                          "stagewise://open", "https://ok.test/a", "HTTP://Upper.test/b",
                                                          longPath, "https://", "not a url"});
        QCOMPARE(urls, QList<QUrl>({QUrl("https://ok.test/a"), QUrl("http://upper.test/b")}));
        QStringList many;
        for (int i = 0; i < 17; ++i) many.append(QString("https://site%1.test/").arg(i));
        QCOMPARE(InstanceCoordinator::filterUrls(many).size(), 16);
        // URLs that would overflow the 64 KiB frame are dropped by the client.
        InstanceRequest big{{}, QString(40000, 'c')};
        for (int i = 0; i < 16; ++i) big.urls.append(QUrl(QString("https://site%1.test/").arg(i) + QString(1900, 'x')));
        const auto encoded = InstanceCoordinator::encodeRequest(big);
        QVERIFY(encoded.size() <= InstanceCoordinator::maxFrameBytes);
        const auto sent = QJsonDocument::fromJson(encoded).object()["urls"].toArray();
        QVERIFY(sent.size() > 0 && sent.size() < 16);
        QCOMPARE(QJsonDocument::fromJson(encoded).object()["cwd"].toString().size(), 40000);
    }
    void rejectsOversizedMalformedWrongVersionFrames() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QCOMPARE(primary.acquire({}), Role::Primary);
        primary.markReady();
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        QByteArray oversized(4, '\0');
        qToBigEndian<quint32>(70000, oversized.data());
        const QList<QByteArray> rejected{
            oversized + QByteArray(64, 'x'),
            frame(QByteArray("{not json")),
            frame(QJsonObject{{"v", 2}, {"type", "activate"}, {"cwd", ""}, {"urls", QJsonArray{}}}),
            frame(QJsonObject{{"v", 1}, {"type", "quit"}, {"cwd", ""}, {"urls", QJsonArray{}}}),
            frame(QJsonObject{{"v", 1}, {"type", "activate"}, {"cwd", ""}, {"urls", QJsonArray{5}}}),
            frame(QByteArray()) + "trailing",
        };
        for (const auto& bytes : rejected) QCOMPARE(exchange(primary.serverName(), bytes), QByteArray());
        QCOMPARE(received.size(), 0);
        // Still serving; URLs failing the policy are dropped server-side.
        const auto reply = exchange(primary.serverName(),
            frame(QJsonObject{{"v", 1}, {"type", "activate"}, {"cwd", "C:/"}, {"urls", QJsonArray{"https://ok.test/", "file:///C:/x"}}}));
        QCOMPARE(reply, frame(QJsonObject{{"ok", true}}));
        QCOMPARE(received.size(), 1);
        QCOMPARE(received.first().first().value<InstanceRequest>().urls, QList<QUrl>({QUrl("https://ok.test/")}));
    }
    void slowClientTimesOut() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        primary.setFrameTimeoutMs(200);
        QCOMPARE(primary.acquire({}), Role::Primary);
        primary.markReady();
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        QLocalSocket slow;
        slow.connectToServer(primary.serverName());
        QVERIFY(slow.waitForConnected(2000));
        slow.write(QByteArray(2, '\0'));
        slow.flush();
        QElapsedTimer clock;
        clock.start();
        QTRY_VERIFY_WITH_TIMEOUT(slow.state() == QLocalSocket::UnconnectedState, 3000);
        QVERIFY(clock.elapsed() >= 150);
        QCOMPARE(slow.readAll(), QByteArray());
        QCOMPARE(received.size(), 0);
        QCOMPARE(exchange(primary.serverName(), frame(QJsonObject{{"v", 1}, {"type", "activate"}, {"cwd", ""}, {"urls", QJsonArray{}}})),
                 frame(QJsonObject{{"ok", true}}));
    }
    void requestsBufferedUntilReady() {
        QTemporaryDir data;
        InstanceCoordinator primary(data.path());
        QCOMPARE(primary.acquire({}), Role::Primary);
        QSignalSpy received(&primary, &InstanceCoordinator::requestReceived);
        for (int i = 0; i < InstanceCoordinator::maxBufferedRequests; ++i) {
            auto future = forwardFromWorker(data.path(), {{QUrl(QString("https://site%1.test/").arg(i))}, {}});
            QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 10000);
            QCOMPARE(future.result(), Role::Forwarded);
        }
        QCOMPARE(received.size(), 0);
        // A full buffer answers "busy"; that client gives up at its deadline.
        auto overflow = forwardFromWorker(data.path(), {{QUrl("https://late.test/")}, {}}, 600);
        QTRY_VERIFY_WITH_TIMEOUT(overflow.isFinished(), 10000);
        QCOMPARE(overflow.result(), Role::Failed);
        primary.markReady();
        QCOMPARE(received.size(), InstanceCoordinator::maxBufferedRequests);
        QCOMPARE(received.first().first().value<InstanceRequest>().urls.first(), QUrl("https://site0.test/"));
    }
    void closingPrimaryRefusesAndLockHandsOver() {
        QTemporaryDir data;
        auto* primary = new InstanceCoordinator(data.path());
        QCOMPARE(primary->acquire({}), Role::Primary);
        primary->markReady();
        primary->setClosing();
        QSignalSpy received(primary, &InstanceCoordinator::requestReceived);
        QCOMPARE(exchange(primary->serverName(), frame(QJsonObject{{"v", 1}, {"type", "activate"}, {"cwd", ""}, {"urls", QJsonArray{}}})),
                 frame(QJsonObject{{"ok", false}, {"reason", "closing"}}));
        auto next = forwardFromWorker(data.path(), {{QUrl("https://after.test/")}, {}}, 8000);
        QTest::qWait(400);
        QVERIFY(!next.isFinished()); // retrying against a closing primary
        QCOMPARE(received.size(), 0);
        delete primary; // releases the lock
        QTRY_VERIFY_WITH_TIMEOUT(next.isFinished(), 10000);
        QCOMPARE(next.result(), Role::Primary);
    }
    void lockHeldButNoServerFails() {
        QTemporaryDir data;
        QLockFile lock(data.filePath("ade-instance.lock"));
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock(0));
        InstanceCoordinator client(data.path());
        QString error;
        QElapsedTimer clock;
        clock.start();
        QCOMPARE(client.acquire({}, 300, &error), Role::Failed);
        QVERIFY(clock.elapsed() < 2000);
        QVERIFY(!error.isEmpty());
        QVERIFY(!client.isPrimary());
    }
    void primaryReleaseAllowsNewPrimary() {
        QTemporaryDir data;
        QString first;
        {
            InstanceCoordinator a(data.path());
            QCOMPARE(a.acquire({}), Role::Primary);
            first = a.serverName();
        }
        QVERIFY(!QFileInfo::exists(data.filePath("ade-instance.json")));
        InstanceCoordinator b(data.path());
        QCOMPARE(b.acquire({}, 1000), Role::Primary);
        QVERIFY(b.serverName() != first);
        QCOMPARE(sidecar(data.path())["server"].toString(), b.serverName());
    }
    void listenFailureStillPrimary() {
        QTemporaryDir data;
        // A crashed owner's sidecar is removed as soon as the lock is taken.
        QFile stale(data.filePath("ade-instance.json"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.write(QJsonDocument(QJsonObject{{"v", 1}, {"pid", 4242}, {"server", "ade-" + QString(32, '0')}}).toJson(QJsonDocument::Compact));
        stale.close();
        InstanceCoordinator primary(data.path());
        primary.setListenFunction([](QLocalServer&, const QString&) { return false; });
        QCOMPARE(primary.acquire({}), Role::Primary);
        QVERIFY(primary.isPrimary());
        QVERIFY(!primary.isServing());
        QVERIFY(!QFileInfo::exists(data.filePath("ade-instance.json")));
        // Never a second primary while the lock is held.
        auto next = forwardFromWorker(data.path(), {}, 300);
        QTRY_VERIFY_WITH_TIMEOUT(next.isFinished(), 5000);
        QCOMPARE(next.result(), Role::Failed);
    }
};

QTEST_GUILESS_MAIN(InstanceCoordinatorTest)
#include "test_instance_coordinator.moc"
