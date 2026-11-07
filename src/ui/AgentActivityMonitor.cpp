#include "AgentActivityMonitor.h"
#include "core/AgentEngine.h"
#include <QJsonArray>
#include <QJsonObject>

namespace ade {

AgentActivityMonitor::AgentActivityMonitor(QObject* parent) : QObject(parent) {
    m_recompute.setSingleShot(true);
    m_recompute.setInterval(0);
    connect(&m_recompute, &QTimer::timeout, this, &AgentActivityMonitor::recompute);
}

AgentActivityMonitor::~AgentActivityMonitor() {
    for (const auto& agent : std::as_const(m_agents))
        for (const auto& connection : agent.connections) disconnect(connection);
}

AgentSnapshot AgentActivityMonitor::snapshotOf(const AgentEngine& engine) {
    AgentSnapshot snapshot;
    snapshot.busy = engine.isBusy();
    snapshot.awaitingUser = engine.isAwaitingUser();
    snapshot.succeeded = engine.lastTurnSucceeded();
    snapshot.queueEmpty = engine.queuedMessages().isEmpty();
    // Upstream checks whether any history row is an assistant message. Search
    // from the end: the latest turn normally contains one.
    const auto messages = engine.messages();
    for (qsizetype i = messages.size() - 1; i >= 0; --i)
        if (messages.at(i).toObject().value("role").toString() == "assistant") { snapshot.hasAssistant = true; break; }
    snapshot.hasQuestion = !engine.pendingQuestion().isEmpty();
    return snapshot;
}

AgentSnapshot AgentActivityMonitor::read(const Watched& agent) const {
    if (agent.snapshot) return agent.snapshot();
    if (!agent.engine) return {};
    return snapshotOf(*agent.engine);
}

void AgentActivityMonitor::watch(const QString& id, AgentEngine* engine, std::function<AgentSnapshot()> snapshot) {
    unwatch(id);
    if (!engine) return;
    Watched agent;
    agent.engine = engine;
    agent.snapshot = std::move(snapshot);
    auto& connections = agent.connections;
    connections.append(connect(engine, &AgentEngine::failed, this, [this, id](const QString&) {
        const auto it = m_agents.constFind(id);
        if (it == m_agents.cend()) return;
        // A send rejected while busy emits failed with the turn still running
        // and no finished; only a genuine fail() has already cleared busy.
        if (!read(*it).busy) emit attention(id, int(AttentionEvent::Error));
        schedule();
    }));
    connections.append(connect(engine, &AgentEngine::finished, this, [this, id] {
        const auto it = m_agents.constFind(id);
        if (it == m_agents.cend()) return;
        const auto state = read(*it);
        schedule();
        // Cancel and failure leave lastTurnSucceeded false; a queued follow-up
        // means the agent keeps working.
        if (state.succeeded && state.queueEmpty && !state.awaitingUser && state.hasAssistant)
            emit attention(id, int(AttentionEvent::Done));
    }));
    connections.append(connect(engine, &AgentEngine::approvalRequired, this, [this, id](const QString& approvalId, const QString&, const QJsonObject&) {
        auto it = m_agents.find(id);
        if (it == m_agents.end()) return;
        schedule();
        if (it->approvals.contains(approvalId)) return;
        it->approvals.insert(approvalId);
        emit attention(id, int(AttentionEvent::Question));
    }));
    connections.append(connect(engine, &AgentEngine::busyChanged, this, [this, id](bool busy) {
        auto it = m_agents.find(id);
        if (it == m_agents.end()) return;
        if (!busy) it->approvals.clear();
        schedule();
    }));
    connections.append(connect(engine, &AgentEngine::questionChanged, this, [this, id] {
        checkQuestion(id);
        schedule();
    }));
    connections.append(connect(engine, &AgentEngine::displayChanged, this, &AgentActivityMonitor::schedule));
    connections.append(connect(engine, &AgentEngine::toolStarted, this, &AgentActivityMonitor::schedule));
    connections.append(connect(engine, &AgentEngine::toolFinished, this, &AgentActivityMonitor::schedule));
    connections.append(connect(engine, &QObject::destroyed, this, &AgentActivityMonitor::schedule));
    m_agents.insert(id, std::move(agent));
    schedule();
}

void AgentActivityMonitor::unwatch(const QString& id) {
    const auto it = m_agents.find(id);
    if (it == m_agents.end()) return;
    for (const auto& connection : std::as_const(it->connections)) disconnect(connection);
    m_agents.erase(it);
    schedule();
}

void AgentActivityMonitor::refresh(const QString& id) {
    if (!m_agents.contains(id)) return;
    checkQuestion(id);
    schedule();
}

void AgentActivityMonitor::checkQuestion(const QString& id) {
    auto it = m_agents.find(id);
    if (it == m_agents.end()) return;
    const bool open = it->engine && read(*it).hasQuestion;
    // Multi-step questions regenerate their token per step; only the
    // empty-to-pending transition is a new question for the user.
    const bool opened = open && !it->questionOpen;
    it->questionOpen = open;
    if (opened && notifyQuestionTool) emit attention(id, int(AttentionEvent::Question));
}

void AgentActivityMonitor::schedule() {
    if (!m_recompute.isActive()) m_recompute.start();
}

void AgentActivityMonitor::recompute() {
    int active = 0;
    for (const auto& agent : std::as_const(m_agents)) {
        // A destroyed engine is inactive even when a test snapshot remains.
        if (!agent.engine) continue;
        const auto state = read(agent);
        if (state.busy && !state.awaitingUser) ++active;
    }
    if (active == m_activeCount) return;
    m_activeCount = active;
    emit activeCountChanged(active);
}

} // namespace ade
