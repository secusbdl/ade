#pragma once
#include "attachments/FileReadCapture.h"
#include <QJsonObject>
#include <QStringList>
#include <memory>

namespace ade {
// One host-owned append-only namespace. Readers capture a revocable value lease,
// never a QObject or a model-provided physical path.
class SessionLogStore {
    struct State;
public:
    static constexpr qint64 MaxLogBytes = 50 * 1024 * 1024;
    explicit SessionLogStore(QString dataDirectory, QString agentId);
    ~SessionLogStore();
    SessionLogStore(const SessionLogStore&) = delete;
    SessionLogStore& operator=(const SessionLogStore&) = delete;
    bool create(const QString& sessionId, QString* error = nullptr);
    bool append(const QString& sessionId, const QByteArray& plainUtf8, QString* error = nullptr);
    bool flush(const QString& sessionId, QString* error = nullptr);
    bool close(const QString& sessionId, QString* error = nullptr);
    QJsonObject stats(const QString& sessionId) const;
    QString rootDirectory() const;
    QStringList list(QString* error = nullptr) const;
    QByteArray rawRead(const QString& mountedPath, qint64 maxBytes,
        const std::atomic_bool* canceled = nullptr, QString* error = nullptr) const;
    FileReadSource formattedSource() const;
    bool cloneTo(const QString& targetAgentId, QString* error = nullptr) const;
    void revoke();
    static bool removeAgent(const QString& dataDirectory, const QString& agentId, QString* error = nullptr);
private:
    std::shared_ptr<State> m_state;
};
}
