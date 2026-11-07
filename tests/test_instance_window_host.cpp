// MainWindow consumers of the runtime services: instance activation, silent
// default effects for test hosts, keep-awake and notification wiring through
// a real agent turn. Before this change MainWindow had no
// handleInstanceRequest, no SystemEffects seam and no AgentActivityMonitor,
// so these tests could not build, and no turn ever reached a power request or
// a sound. Fakes replace winmm and power requests via the factory seam.
#include "ui/MainWindow.h"
#include "ui/Composer.h"
#include "ui/InstanceCoordinator.h"
#include "ui/NotificationSounds.h"
#include "ui/PowerRequest.h"
#include "ui/SessionStore.h"
#include "ui/SettingsPage.h"
#include "ui/SoundOutput.h"
#include "core/AgentEngine.h"
#include "core/ProviderStore.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <memory>

using namespace ade;

namespace {
bool seed(const QString& path, const QString& workspace = {}) {
    Conversation value;
    value.id = "fixture";
    value.title = "Fixture chat";
    value.providerId = "fixture";
    value.model = "fixture-model";
    value.workspace = workspace;
    value.titleLocked = true;
    return SessionStore(path).save({value});
}
void showHidden(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200, 800);
    window.show();
    QCoreApplication::processEvents();
}
QTabWidget* contentTabs(MainWindow& window) { return window.findChild<QTabWidget*>("contentTabs"); }

struct PowerCalls {
    int acquires = 0, releases = 0;
    bool held = false;
};
class FakePower final : public IPowerBackend {
public:
    explicit FakePower(std::shared_ptr<PowerCalls> calls) : m_calls(std::move(calls)) {}
    bool acquire() override { ++m_calls->acquires; m_calls->held = true; return true; }
    void release() override { ++m_calls->releases; m_calls->held = false; }
private:
    std::shared_ptr<PowerCalls> m_calls;
};
struct SoundCalls {
    QList<QByteArray> wavs;
    int stops = 0;
};
class FakeSound final : public ISoundOutput {
public:
    explicit FakeSound(std::shared_ptr<SoundCalls> calls) : m_calls(std::move(calls)) {}
    bool playWav(const QByteArray& wav) override { m_calls->wavs.append(wav); return true; }
    bool playFile(const QString&, double) override { return true; }
    void stop() override { ++m_calls->stops; }
private:
    std::shared_ptr<SoundCalls> m_calls;
};

QByteArray sseEvent(const QJsonObject& object) { return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n"; }
QByteArray textResponse(const QString& text) {
    const QJsonObject choice{{"delta", QJsonObject{{"content", text}}}, {"finish_reason", "stop"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
QByteArray toolResponse(const QString& name, const QJsonObject& arguments) {
    const QJsonObject function{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}};
    const QJsonObject call{{"index", 0}, {"id", "call_host"}, {"type", "function"}, {"function", function}};
    const QJsonObject choice{{"delta", QJsonObject{{"tool_calls", QJsonArray{call}}}}, {"finish_reason", "tool_calls"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}
// Responds after a delay so the agent is observably busy between requests.
class DelayedServer : public QTcpServer {
public:
    int requests = 0;
    DelayedServer() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto& line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const int index = ++requests;
                    const auto body = index == 1 ? toolResponse("write", {{"path", "result.txt"}, {"content", "x"}}) : textResponse("Finished.");
                    QTimer::singleShot(400, socket, [socket, body] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
};
}

class InstanceWindowHostTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { MainWindow::setSystemEffectsFactory({}); }

    void mainWindowUsesNullEffectsOffscreen() {
        QCOMPARE(QGuiApplication::platformName(), QString("offscreen"));
        const auto effects = MainWindow::defaultSystemEffects();
        const auto sound = effects.sound();
        const auto power = effects.power();
        QVERIFY(dynamic_cast<NullSoundOutput*>(sound.get()));
        QVERIFY(dynamic_cast<NullPowerBackend*>(power.get()));
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        showHidden(window);
        QVERIFY(window.close());
    }
    void handleInstanceRequestRestoresAndOpensTabs() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        showHidden(window);
        window.showMinimized();
        QVERIFY(window.isMinimized());
        auto* tabs = contentTabs(window);
        QVERIFY(tabs);
        const int before = tabs->count();
        window.handleInstanceRequest({{QUrl("http://127.0.0.1:9/a"), QUrl("http://127.0.0.1:9/b")}, {}});
        QVERIFY(!window.isMinimized());
        QVERIFY(window.isVisible());
        QCOMPARE(tabs->count(), before + 2);
        // An empty request (a plain second launch) only activates.
        window.handleInstanceRequest({});
        QCOMPARE(tabs->count(), before + 2);
    }
    void handleInstanceRequestDuringTransitionOnlyActivates() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        showHidden(window);
        auto* tabs = contentTabs(window);
        const int before = tabs->count();
        // An accepted close leaves the window in its terminal transition.
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
        window.handleInstanceRequest({{QUrl("http://127.0.0.1:9/late")}, {}});
        QVERIFY(!window.isVisible()); // never re-shown after an accepted close
        QTest::qWait(150);
        QCOMPARE(tabs->count(), before);
        window.openStartupUrls({QUrl("http://127.0.0.1:9/later")});
        QCOMPARE(tabs->count(), before);
        QVERIFY(!window.isVisible());
    }
    void startupUrlsOpenWithoutActivation() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        showHidden(window);
        window.showMinimized();
        auto* tabs = contentTabs(window);
        const int before = tabs->count();
        window.openStartupUrls({QUrl("http://127.0.0.1:9/start")});
        QCOMPARE(tabs->count(), before + 1);
        QVERIFY(window.isMinimized()); // no restore or activation request
    }
    void recoveryDialogIsRaised() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        MainWindow window(data.path());
        showHidden(window);
        auto* reload = window.findChild<QPushButton*>("reloadConversationsButton");
        QVERIFY(reload);
        reload->click();
        auto* dialog = window.findChild<QMessageBox*>("sessionReloadConfirmation");
        QVERIFY(dialog);
        auto* tabs = contentTabs(window);
        const int before = tabs->count();
        window.handleInstanceRequest({{QUrl("http://127.0.0.1:9/blocked")}, {}});
        QCOMPARE(tabs->count(), before);
        QVERIFY(dialog->isVisible());
        // The URL waits for the decision instead of being dropped.
        auto* status = window.findChild<QLabel*>("sessionStatus");
        QVERIFY(status);
        QVERIFY(status->text().contains("after this decision"));
        const QPointer<QMessageBox> box(dialog);
        dialog->reject();
        QTRY_VERIFY(!box);
        QTRY_COMPARE(tabs->count(), before + 1);
    }
    void hostBusyAcquiresAwaitingReleasesPrefOffReleases() {
        QTemporaryDir data, workspace;
        DelayedServer server;
        QVERIFY(server.isListening());
        const ProviderConfig config{"fixture", "Local fixture", QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
                                    "synthetic-host-key", "fixture-model", "openai"};
        QString error;
        QVERIFY2(ProviderStore::save(data.filePath("providers.json"), {config}, &error), qPrintable(error));
        QVERIFY(seed(data.path(), workspace.path()));
        auto power = std::make_shared<PowerCalls>();
        auto sounds = std::make_shared<SoundCalls>();
        MainWindow::setSystemEffectsFactory({[sounds] { return std::unique_ptr<ISoundOutput>(std::make_unique<FakeSound>(sounds)); },
                                             [power] { return std::unique_ptr<IPowerBackend>(std::make_unique<FakePower>(power)); }});
        {
            MainWindow window(data.path());
            showHidden(window);
            // Minimized: the chat is not visible, so every attention event chimes.
            window.showMinimized();
            auto* input = window.findChild<Composer*>();
            auto* send = window.findChild<QToolButton*>("sendButton");
            QVERIFY(input && send);
            input->setPlainText("Create result.txt");
            send->click();
            auto* engine = window.findChild<AgentEngine*>();
            QVERIFY(engine);
            QSignalSpy approvals(engine, &AgentEngine::approvalRequired);
            QTRY_VERIFY_WITH_TIMEOUT(power->held, 5000);           // busy, streaming
            QTRY_COMPARE_WITH_TIMEOUT(approvals.size(), 1, 5000);
            QVERIFY(engine->isAwaitingUser());
            QTRY_VERIFY_WITH_TIMEOUT(!power->held, 5000);          // awaiting approval is idle
            QTRY_VERIFY_WITH_TIMEOUT(!sounds->wavs.isEmpty(), 5000); // approval chimed
            QCOMPARE(sounds->wavs.first(), NotificationSoundService::scalePcm16Wav(
                NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Question), 0.251));
            engine->approveTool(approvals.first().first().toString(), false);
            QTRY_VERIFY_WITH_TIMEOUT(power->held, 5000);           // working again
            auto* settings = window.findChild<SettingsPage*>();
            QVERIFY(settings->setPreference("agent.keepAwake", false));
            QVERIFY(!power->held);                                  // preference off releases at once
            QVERIFY(engine->isBusy());
            QTRY_VERIFY_WITH_TIMEOUT(!engine->isBusy(), 5000);
            QVERIFY(engine->lastTurnSucceeded());
            QCOMPARE(power->acquires, 2);
            QCOMPARE(power->releases, 2);
            QVERIFY(window.close());
        }
        // Teardown stops audio and leaves no request held.
        QVERIFY(sounds->stops >= 1);
        QVERIFY(!power->held);
    }
};

QTEST_MAIN(InstanceWindowHostTest)
#include "test_instance_window_host.moc"
