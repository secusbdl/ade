#include "SidebarModel.h"
#include <QTimeZone>
#include <algorithm>
namespace ade {
QString SidebarModel::normalizePath(QString path) {
    return path.replace('\\', '/');
}
QString SidebarModel::baseName(QString path) {
    path = normalizePath(path);
    while (path.endsWith('/') && path.size() > 1)
        path.chop(1);
    return path.section('/', -1).isEmpty() ? path : path.section('/', -1);
}
QString SidebarModel::workspaceKey(const SidebarMount& mount) {
    return mount.git ? "git:" + mount.repositoryId : "dir:" + normalizePath(mount.path);
}
QString SidebarModel::worktreeKey(const SidebarMount& mount) {
    return mount.git ? (mount.worktree ? "worktree:" + normalizePath(mount.path) : "root")
                     : "dir:" + normalizePath(mount.path);
}
QString SidebarModel::ageLabel(qint64 timestamp, const QDateTime& now) {
    if (!timestamp)
        return "Today";
    const auto date = QDateTime::fromMSecsSinceEpoch(timestamp, now.timeZone()).date();
    const auto days = date.daysTo(now.date());
    if (days <= 0)
        return "Today";
    if (days == 1)
        return "Yesterday";
    if (days <= 7)
        return "Last 7 days";
    if (days <= 30)
        return "Last 30 days";
    return "Older";
}
SidebarProjection SidebarModel::project(QList<SidebarEntry> entries,
                                        const SidebarProjectionOptions& options) {
    entries.removeIf([&](const auto& entry) {
        return entry.conversation.archived ||
               (!options.query.trimmed().isEmpty() &&
                !entry.conversation.title.contains(options.query.trimmed(), Qt::CaseInsensitive));
    });
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return qMax(a.conversation.created, a.conversation.lastMessageAt) >
               qMax(b.conversation.created, b.conversation.lastMessageAt);
    });
    SidebarProjection result;
    QHash<QString, SidebarEntry> byId;
    for (const auto& entry : entries)
        byId.insert(entry.conversation.id, entry);
    QSet<QString> pins;
    for (const auto& id : options.pinnedIds)
        if (byId.contains(id) && !pins.contains(id)) {
            pins.insert(id);
            result.rows.append(
                {SidebarRow::Agent, "pin/" + id, byId[id].conversation.title, id, {}, 0, true});
            ++result.pinnedCount;
        }
    const auto agentRow = [&](const SidebarEntry& entry, const QString& key, const QString& path,
                              int indent) {
        result.rows.append({SidebarRow::Agent, key, entry.conversation.title, entry.conversation.id, path,
                            indent, pins.contains(entry.conversation.id)});
    };
    const auto header = [&](const QString& key, const QString& title, const QString& path, int indent,
                            bool collapsible) {
        const bool collapsed = collapsible && options.collapsedKeys.contains(key);
        result.rows.append({SidebarRow::Header, key, title, {}, path, indent, false, collapsible, collapsed});
        return collapsed;
    };
    if (options.mode != "workspace") {
        QList<SidebarEntry> unpinned;
        int createdThisSession = 0;
        for (const auto& entry : entries)
            if (!pins.contains(entry.conversation.id)) {
                unpinned.append(entry);
                if (options.sessionStartedAt > 0 && entry.conversation.created >= options.sessionStartedAt)
                    ++createdThisSession;
            }
        const int visible = options.query.trimmed().isEmpty()
                                ? qMax(10, qMax(options.visibleCount, createdThisSession))
                                : unpinned.size();
        QString previous;
        int group = 0;
        for (int i = 0; i < unpinned.size(); ++i) {
            const auto& entry = unpinned[i];
            if (i >= visible && entry.conversation.id != options.currentId)
                continue;
            const auto label = ageLabel(entry.conversation.lastMessageAt, options.now);
            if (label != previous) {
                header("age/" + QString::number(group++), label, {}, 0, false);
                previous = label;
            }
            agentRow(entry, "age-agent/" + entry.conversation.id, {}, 0);
        }
        if (visible < unpinned.size())
            result.rows.append({SidebarRow::ShowMore, "show-more", "Show more...", {}, {}});
        return result;
    }
    struct Tree {
        SidebarMount mount;
        QList<SidebarEntry> agents;
    };
    struct Group {
        SidebarMount mount;
        QList<SidebarEntry> direct;
        QList<Tree> trees;
    };
    QHash<QString, Group> groups;
    QStringList discovered;
    QList<SidebarEntry> noWorkspace;
    for (const auto& entry : entries) {
        if (entry.mounts.isEmpty()) {
            noWorkspace.append(entry);
            continue;
        }
        QSet<QString> mounts;
        for (const auto& mount : entry.mounts) {
            const auto key = workspaceKey(mount), tree = worktreeKey(mount);
            if (mounts.contains(key + '/' + tree))
                continue;
            mounts.insert(key + '/' + tree);
            if (!groups.contains(key)) {
                groups.insert(key, {mount, {}, {}});
                discovered.append(key);
            }
            auto& group = groups[key];
            if (!mount.git) {
                group.direct.append(entry);
                continue;
            }
            auto found = std::find_if(group.trees.begin(), group.trees.end(),
                                      [&](const auto& value) { return worktreeKey(value.mount) == tree; });
            if (found == group.trees.end()) {
                group.trees.append({mount, {}});
                found = group.trees.end() - 1;
            }
            found->agents.append(entry);
        }
    }
    // Newly discovered repositories precede persisted manual order. Missing
    // preference keys stay stored, but never create phantom headers.
    for (const auto& key : discovered)
        if (!options.workspaceOrder.contains(key))
            result.workspaceKeys.append(key);
    for (const auto& key : options.workspaceOrder)
        if (groups.contains(key) && !result.workspaceKeys.contains(key))
            result.workspaceKeys.append(key);
    if (!noWorkspace.isEmpty() && !header("__no-workspace__", "No workspace", {}, 0, true))
        for (const auto& entry : noWorkspace)
            agentRow(entry, "no-workspace/" + entry.conversation.id, {}, 1);
    for (const auto& key : result.workspaceKeys) {
        auto group = groups[key];
        const auto root = group.mount.mainPath.isEmpty() ? group.mount.path : group.mount.mainPath;
        if (header(key, baseName(root), root, 0, true))
            continue;
        if (!group.mount.git) {
            for (const auto& entry : group.direct)
                agentRow(entry, key + '/' + entry.conversation.id, group.mount.path, 1);
            continue;
        }
        for (const auto& known : options.repositoryWorktrees.value(group.mount.repositoryId)) {
            auto found = std::find_if(group.trees.begin(), group.trees.end(), [&](const auto& tree) {
                return worktreeKey(tree.mount) == worktreeKey(known);
            });
            if (found == group.trees.end())
                group.trees.append({known, {}});
            else
                found->mount = known;
        }
        std::stable_sort(group.trees.begin(), group.trees.end(), [](const auto& a, const auto& b) {
            if (a.mount.worktree != b.mount.worktree)
                return !a.mount.worktree;
            if (a.mount.createdAt != b.mount.createdAt)
                return a.mount.createdAt > b.mount.createdAt;
            return worktreeKey(a.mount) < worktreeKey(b.mount);
        });
        for (const auto& tree : group.trees) {
            QString label = tree.mount.worktree ? baseName(tree.mount.path) : "Root";
            const auto ref = tree.mount.branch.isEmpty() ? tree.mount.headSha.left(7) : tree.mount.branch;
            if (!ref.isEmpty() && ref != label)
                label += " (" + ref + ")";
            const auto treeId = key + '/' + worktreeKey(tree.mount);
            if (header(treeId, label, tree.mount.path, 1, true))
                continue;
            for (const auto& entry : tree.agents)
                agentRow(entry, treeId + '/' + entry.conversation.id, tree.mount.path, 2);
        }
    }
    return result;
}
} // namespace ade
