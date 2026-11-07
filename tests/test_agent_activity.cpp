// AgentActivityMonitor: attention events and active-agent accounting.
// Before this change ADE had no consumer of agent state for notifications or
// keep-awake: the monitor and AgentEngine::isAwaitingUser() did not exist, so
// none of these tests could compile or observe an event.
#include "ui/AgentActivityMonitor.h"
#include "core/AgentEngine.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>

using namespace ade;

namespace {
// Local OpenAI-compatible fixture (pattern from tests/test_core.cpp).
struct Response { QByteArray body; bool hold = false; };
class FixtureServer : public QObject {
public:
    QTcpServer server;
    int requests = 0;
    std::function<Response(int)> respond;
    QPointer<QTcpSocket> held; // the latest held request
    void release(const QByteArray& body) {
        if (!held) return;
        held->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n");
        held->write(body);
        held->disconnectFromHost();
    }
    FixtureServer() {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("handled").toBool()) { socket->readAll(); return; }
                    auto buffer = socket->property("buffer").toByteArray() + socket->readAll();
                    socket->setProperty("buffer", buffer);
                    const auto headerEnd = buffer.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    int length = 0;
                    for (const auto &line : buffer.left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (buffer.size() < headerEnd + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto response = respond ? respond(++requests) : Response{};
                    if (response.hold) { held = socket; return; }
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
                                  + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n");
                    socket->write(response.body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    ProviderConfig provider() const {
        return {"fixture", "Local fixture", QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()),
                "fixture-secret", "fixture-model", "openai"};
    }
};
QByteArray sseEvent(const QJsonObject &object) { return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n\n"; }
QByteArray toolResponse(const QString &name, const QJsonObject &arguments) {
    const QJsonObject function{{"name", name}, {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))}};
    const QJsonObject call{{"index", 0}, {"id", "call_test"}, {"type", "function"}, {"function", function}};
    const QJsonObject choice{{"delta", QJsonObject{{"tool_calls", QJsonArray{call}}}}, {"finish_reason", "tool_calls"}};
    return sseEvent(QJsonObject{{"choices", QJsonArray{choice}}}) + "data: [DONE]\n\n";
}

int count(const QSignalSpy& spy, AttentionEvent event) {
    int total = 0;
    for (const auto& args : spy) total += args.at(1).toInt() == int(event);
    return total;
}

struct Harness {
    AgentEngine engine;
    AgentActivityMonitor monitor;
    AgentSnapshot state;
    QSignalSpy attention{&monitor, &AgentActivityMonitor::attention};
    QSignalSpy active{&monitor, &AgentActivityMonitor::activeCountChanged};
    Harness() { monitor.watch("agent", &engine, [this] { return state; }); }
    void settle() { QCoreApplication::processEvents(); QCoreApplication::processEvents(); }
};
}

class AgentActivityTest : public QObject {
    Q_OBJECT
private slots:
    void doneOnSuccessfulFinishWithEmptyQueue() {
        Harness h;
        h.state = {false, false, true, true, true, false};
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 1);
        QCOMPARE(h.attention.first().at(0).toString(), QString("agent"));
        QCOMPARE(h.attention.first().at(1).toInt(), int(AttentionEvent::Done));
    }
    void noDoneWhenQueueNonEmpty() {
        // complete() emits finished before the queued follow-up starts.
        Harness h;
        h.state = {false, false, true, false, true, false};
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 0);
    }
    void noDoneWithoutAnyAssistantMessage() {
        Harness h;
        h.state = {false, false, true, true, false, false};
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 0);
    }
    void noDoneOnCancel() {
        // cancel(): busyChanged(false), finished, lastTurnSucceeded false.
        Harness h;
        h.state = {false, false, false, true, true, false};
        emit h.engine.busyChanged(false);
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 0);
    }
    void errorOnRealFailOnly() {
        // fail(): busy already false, then failed, then finished.
        Harness h;
        h.state = {false, false, false, true, true, false};
        emit h.engine.busyChanged(false);
        emit h.engine.failed("provider error");
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 1);
        QCOMPARE(count(h.attention, AttentionEvent::Error), 1);
    }
    void rejectedSendWhileBusyIsSilent() {
        // startUserMessage while busy emits failed without finished.
        Harness h;
        h.state = {true, false, false, true, true, false};
        emit h.engine.failed("This agent is already running.");
        QCOMPARE(h.attention.size(), 0);
        h.state = {false, false, true, true, true, false};
        emit h.engine.busyChanged(false);
        emit h.engine.finished();
        QCOMPARE(h.attention.size(), 1);
        QCOMPARE(count(h.attention, AttentionEvent::Done), 1);
    }
    void questionOnApprovalDedupedById() {
        Harness h;
        h.state = {true, true, false, true, false, false};
        emit h.engine.approvalRequired("a", "write", {});
        emit h.engine.approvalRequired("a", "write", {});
        emit h.engine.approvalRequired("b", "write", {});
        QCOMPARE(count(h.attention, AttentionEvent::Question), 2);
        h.state.busy = false;
        emit h.engine.busyChanged(false);
        h.state.busy = true;
        emit h.engine.busyChanged(true);
        emit h.engine.approvalRequired("a", "write", {});
        QCOMPARE(count(h.attention, AttentionEvent::Question), 3);
    }
    void questionToolTransitionEmitsOnceAcrossSteps() {
        for (const bool notify : {true, false}) {
            Harness h;
            h.monitor.notifyQuestionTool = notify;
            h.state.busy = true;
            for (const bool pending : {false, true, true, false, true}) {
                // The third value models a regenerated token for the next step.
                h.state.hasQuestion = pending;
                h.state.awaitingUser = pending;
                emit h.engine.questionChanged();
            }
            QCOMPARE(count(h.attention, AttentionEvent::Question), notify ? 2 : 0);
        }
    }
    void activeCountExcludesAwaitingUser() {
        Harness h;
        h.settle();
        h.active.clear();
        h.state.busy = true;
        emit h.engine.busyChanged(true);
        emit h.engine.displayChanged();
        emit h.engine.displayChanged();
        emit h.engine.toolStarted("t", "read", {});
        QCOMPARE(h.monitor.activeCount(), 0); // coalesced: not yet recomputed
        h.settle();
        QCOMPARE(h.monitor.activeCount(), 1);
        QCOMPARE(h.active.size(), 1);
        h.state.awaitingUser = true;
        emit h.engine.approvalRequired("x", "write", {});
        emit h.engine.displayChanged();
        h.settle();
        QCOMPARE(h.monitor.activeCount(), 0);
        QCOMPARE(h.active.size(), 2);
        emit h.engine.displayChanged();
        h.settle();
        QCOMPARE(h.active.size(), 2); // no emission without a change
    }
    void destroyedEngineReleasesActivity() {
        AgentActivityMonitor monitor;
        QSignalSpy active(&monitor, &AgentActivityMonitor::activeCountChanged);
        AgentSnapshot state{true, false, false, true, false, false};
        auto* engine = new AgentEngine;
        monitor.watch("gone", engine, [&state] { return state; });
        QTRY_COMPARE(monitor.activeCount(), 1);
        delete engine;
        QTRY_COMPARE(monitor.activeCount(), 0);
        QCOMPARE(active.size(), 2);
        // unwatch also releases.
        AgentEngine other;
        monitor.watch("other", &other, [&state] { return state; });
        QTRY_COMPARE(monitor.activeCount(), 1);
        monitor.unwatch("other");
        QTRY_COMPARE(monitor.activeCount(), 0);
        emit other.finished();
        QCOMPARE(monitor.isWatching("other"), false);
    }
    void realEngineAwaitingApprovalIsIdle() {
        QTemporaryDir root;
        FixtureServer server;
        // Every request is held: request 1 (streaming) is released as an
        // approval-gated write; request 2 (after the tool) keeps the agent busy.
        server.respond = [](int) { return Response{{}, true}; };
        AgentEngine engine;
        engine.setProvider(server.provider());
        engine.setWorkspace(root.path());
        AgentActivityMonitor monitor;
        monitor.watch("real", &engine);
        QSignalSpy approvals(&engine, &AgentEngine::approvalRequired);
        QSignalSpy attention(&monitor, &AgentActivityMonitor::attention);
        QSignalSpy active(&monitor, &AgentActivityMonitor::activeCountChanged);
        const auto counts = [&active] {
            QList<int> values;
            for (const auto& args : active) values.append(args.at(0).toInt());
            return values;
        };
        engine.submit("Create result.txt");
        // Streaming the first response: working.
        QTRY_COMPARE(server.requests, 1);
        QTRY_COMPARE(active.size(), 1);
        QCOMPARE(counts(), QList<int>({1}));
        QCOMPARE(monitor.activeCount(), 1);
        server.release(toolResponse("write", {{"path", "result.txt"}, {"content", "native"}}));
        // Awaiting the approval: still busy, but idle for keep-awake.
        QTRY_COMPARE(approvals.size(), 1);
        QVERIFY(engine.isBusy());
        QVERIFY(engine.isAwaitingUser());
        QTRY_COMPARE(active.size(), 2);
        QCOMPARE(counts(), QList<int>({1, 0}));
        QCOMPARE(count(attention, AttentionEvent::Question), 1);
        engine.approveTool(approvals.first().at(0).toString(), true);
        monitor.refresh("real");
        QVERIFY(!engine.isAwaitingUser());
        // Approved: working again on the held second request.
        QTRY_COMPARE(server.requests, 2);
        QVERIFY(engine.isBusy());
        QTRY_COMPARE(active.size(), 3);
        QCOMPARE(counts(), QList<int>({1, 0, 1}));
        engine.cancel();
        QTRY_COMPARE(active.size(), 4);
        QCOMPARE(counts(), QList<int>({1, 0, 1, 0}));
        QCOMPARE(count(attention, AttentionEvent::Done), 0);
        QCOMPARE(count(attention, AttentionEvent::Error), 0);
    }
};

QTEST_MAIN(AgentActivityTest)
#include "test_agent_activity.moc"
