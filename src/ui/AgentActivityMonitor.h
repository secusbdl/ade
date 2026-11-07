#pragma once
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <functional>

namespace ade {
class AgentEngine;

// Semantic agent events that may ask for the user's attention.
enum class AttentionEvent { Done = 0, Question = 1, Error = 2 };

// The state the monitor needs from one agent at the moment of a signal.
struct AgentSnapshot {
    bool busy = false;
    bool awaitingUser = false;
    bool succeeded = false;
    bool queueEmpty = true;
    bool hasAssistant = false;
    bool hasQuestion = false;
};

// Derives attention events (done/question/error) and the number of actively
// working agents from AgentEngine signals. Waiting for an approval or a
// question counts as idle. Recomputation of the active count is coalesced.
class AgentActivityMonitor final : public QObject {
    Q_OBJECT
public:
    explicit AgentActivityMonitor(QObject* parent = nullptr);
    ~AgentActivityMonitor() override;
    // The snapshot defaults to reading the engine. Tests may supply their own.
    void watch(const QString& id, AgentEngine* engine, std::function<AgentSnapshot()> snapshot = {});
    void unwatch(const QString& id);
    // Re-derive the active count after a host action (for example approveTool)
    // that may change state without an engine signal.
    void refresh(const QString& id);
    int activeCount() const { return m_activeCount; }
    bool isWatching(const QString& id) const { return m_agents.contains(id); }
    static AgentSnapshot snapshotOf(const AgentEngine& engine);
    // ADE deviation from upstream: also chime when the ask-user question tool
    // opens. Set false for strict parity (approvals only).
    bool notifyQuestionTool = true;

signals:
    void activeCountChanged(int count);
    void attention(QString id, int event);

private:
    struct Watched {
        QPointer<AgentEngine> engine;
        std::function<AgentSnapshot()> snapshot;
        QSet<QString> approvals;
        bool questionOpen = false;
        QList<QMetaObject::Connection> connections;
    };
    AgentSnapshot read(const Watched& agent) const;
    void schedule();
    void recompute();
    void checkQuestion(const QString& id);
    QHash<QString, Watched> m_agents;
    QTimer m_recompute;
    int m_activeCount = 0;
};

} // namespace ade
