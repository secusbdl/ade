#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>

namespace ade {
// Value-only host callback. Backing roots and journal identity never come from
// model arguments. The callback is copied into a native worker for each dispatch.
struct FileMutationInvocation {
    QString requestId, tool, workspace, appsDirectory;
    QJsonObject arguments;
};
struct FileMutationChange {
    QString path;
    QByteArray before, after;
    bool existedBefore=false, existsAfter=false;
};
struct FileMutationOutcome {
    QString output;
    bool error=false;
    QJsonObject metadata;
    QList<FileMutationChange> changes;
};
using FileMutationExecutor=std::function<FileMutationOutcome(const FileMutationInvocation&,const std::atomic_bool*)>;
}
