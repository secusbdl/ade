#pragma once
#include "SessionStore.h"
#include <QDateTime>
#include <QHash>
#include <QSet>
namespace ade {
struct SidebarMount {
    QString path, repositoryId, mainPath, branch, headSha;
    bool git = false, worktree = false;
    qint64 createdAt = -1;
};
struct SidebarEntry {
    Conversation conversation;
    QList<SidebarMount> mounts;
};
struct SidebarProjectionOptions {
    QString mode = "age", query, currentId;
    QStringList pinnedIds, workspaceOrder, collapsedKeys;
    int visibleCount = 10;
    qint64 sessionStartedAt = 0;
    QDateTime now = QDateTime::currentDateTime();
    QHash<QString, QList<SidebarMount>> repositoryWorktrees;
};
struct SidebarRow {
    enum Kind { Header, Agent, ShowMore } kind = Agent;
    QString key, title, agentId, path;
    int indent = 0;
    bool pinned = false, collapsible = false, collapsed = false;
};
struct SidebarProjection {
    QList<SidebarRow> rows;
    QStringList workspaceKeys;
    int pinnedCount = 0;
};
class SidebarModel final {
  public:
    static QString ageLabel(qint64 timestamp, const QDateTime& now);
    static QString normalizePath(QString path);
    static QString baseName(QString path);
    static QString workspaceKey(const SidebarMount& mount);
    static QString worktreeKey(const SidebarMount& mount);
    static SidebarProjection project(QList<SidebarEntry> entries, const SidebarProjectionOptions& options);
};
} // namespace ade
