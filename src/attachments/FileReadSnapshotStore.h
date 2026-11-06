#pragma once
#include "FileReadCapture.h"
#include <memory>

namespace ade {
class FileReadSnapshotStore {
    struct State;
public:
    struct Lease {
        Lease() = default;
        bool isValid() const { return bool(state); }
        bool belongsTo(const QString &agentId) const;
    private:
        std::shared_ptr<State> state;
        quint64 epoch = 0;
        friend class FileReadSnapshotStore;
    };
    explicit FileReadSnapshotStore(QString dataDirectory);
    // Only host lifecycle code acquires/reactivates a chat lease. Worker copies cannot.
    Lease acquire(const QString &agentId, QString *error = nullptr) const;
    static QJsonObject commit(const Lease &, const FileReadCapture &, const std::atomic_bool *cancelled, QString *error);
    static bool read(const Lease &, const QJsonObject &metadata, FileReadCapture *capture, QString *error);
    static void revoke(const Lease &);
    bool removeAgent(const QString &agentId, QString *error = nullptr) const;
    bool cloneAgent(const QString &sourceAgentId, const QString &targetAgentId, QString *error = nullptr) const;
private:
    std::shared_ptr<State> stateFor(const QString &agentId, QString *error) const;
    QString m_directory;
};
}
