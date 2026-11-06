#pragma once
#include "GitTypes.h"
#include <QObject>
#include <QProcessEnvironment>
#include <memory>

namespace ade {
// Creation does not execute scripts. The host calls startAfterMount only after
// the workspace runtime is attached; a failed setup never removes the worktree.
class WorktreeSetupRunner final : public QObject {
    Q_OBJECT
public:
    explicit WorktreeSetupRunner(QObject *parent=nullptr);
    ~WorktreeSetupRunner() override;
    quint64 startAfterMount(const WorktreeSetupMetadata &metadata);
    void cancel(const QString &workspacePath);
    void cancelAll();
    void setTimeoutMs(int timeoutMs);
    // Optional explicit executable for deployment/testing; otherwise pwsh then powershell.
    void setShellExecutable(const QString &executable);
signals:
    void updated(const ade::WorktreeSetupRun &run);
private:
    struct Private; std::unique_ptr<Private> d;
};
}
