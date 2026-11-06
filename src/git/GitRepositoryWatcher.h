#pragma once
#include "GitTypes.h"
#include <QObject>
#include <memory>
namespace ade {
struct GitRepositoryWatchOptions {
    int debounceMs=400,scanDelayMs=50,retryMs=5000,availabilityCheckMs=5000;
    int maxWorktrees=2048,maxHeadBytes=16384,maxTotalHeadBytes=4*1024*1024;
    int maxRepositories=128,maxOwners=4096;
};
// Consume snapshots already authorized by the host's GitService. This class
// neither discovers new workspace authority nor reads ordinary project files.
// HEAD/topology only: status/diff refresh remains a separate host responsibility.
class GitRepositoryWatcher final : public QObject {
    Q_OBJECT
public:
    explicit GitRepositoryWatcher(GitRepositoryWatchOptions options={},QObject *parent=nullptr);
    ~GitRepositoryWatcher() override;
    bool acquire(const QString &ownerId,const GitSnapshot &repository,QString *error=nullptr);
    void release(const QString &ownerId);
    void refresh(const QString &commonGitDir={});
    int repositoryCount() const;
    int ownerCount(const QString &commonGitDir) const;
    QStringList watchedPaths(const QString &commonGitDir) const;
signals:
    void ready(const QString &commonGitDir);
    void topologyChanged(const QString &commonGitDir,quint64 revision);
    void coverageChanged(const QString &commonGitDir,bool complete,const QString &reason);
private:
    struct Private;std::unique_ptr<Private> d;
    void scheduleScan(const QString &key);
    void scan(const QString &key);
    void probeAvailability(const QString &key);
    void publish(const QString &key);
};
}
