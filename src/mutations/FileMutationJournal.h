#pragma once
#include "FileMutationExecutor.h"
#include <QJsonArray>
#include <memory>

namespace ade {
// Each lease belongs to one chat and one lifetime. Forks start a new journal;
// copying conversation metadata never grants undo authority over another chat.
class FileMutationJournal {
    struct State;
public:
    explicit FileMutationJournal(QString dataDirectory,QString agentId);
    bool bindMount(const QString& prefix,const QString& directory,bool writable,QString* error=nullptr);
    void unbindMount(const QString& prefix);
    FileMutationExecutor executor() const;
    FileMutationOutcome undo(const QStringList& mutationIds,const QString& requestId,const std::atomic_bool* canceled=nullptr) const;
    FileMutationOutcome recover(const std::atomic_bool* canceled=nullptr)const;
    QJsonArray history(QString* error=nullptr,int limit=200) const;
    QJsonObject turnFiles(const QStringList& mutationIds,const QString& path={},QString* error=nullptr) const;
    QJsonArray settledInvocations(const QStringList& invocationIds,QString* error=nullptr) const;
    void revoke();
    // Deterministic timing seam for local crash/lifetime fixtures. It receives
    // no filesystem authority and is never exposed through a model or page API.
    void setBoundaryObserverForTests(std::function<void(const QString&,int)> observer);
    static bool removeAgent(const QString& dataDirectory,const QString& agentId,QString* error=nullptr);
private:
    std::shared_ptr<State> m_state;
};
}
