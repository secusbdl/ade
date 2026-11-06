#include "SidebarNavigation.h"
#include "Icons.h"
#include "SettingsPage.h"
#include "git/GitService.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
namespace ade {
namespace {
class SidebarDelegate final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem adjusted(option);
        initStyleOption(&adjusted, index);
        const int indent = index.data(SidebarNavigation::IndentRole).toInt();
        adjusted.rect.adjust(indent * 12, 0, 0, 0);
        if (index.data(SidebarNavigation::KindRole).toInt() == SidebarRow::Header) {
            adjusted.state &= ~QStyle::State_Selected;
            const auto foreground = adjusted.palette.color(QPalette::PlaceholderText);
            painter->save();
            painter->setPen(foreground);
            auto font = adjusted.font;
            font.setPixelSize(qMax(10, qRound(qApp->font().pixelSize() * 12.0 / 13)));
            painter->setFont(font);
            auto rect = adjusted.rect.adjusted(6, 0, -4, 0);
            if (index.data(SidebarNavigation::CollapsibleRole).toBool()) {
                painter->drawText(rect.adjusted(0, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                                  index.data(SidebarNavigation::CollapsedRole).toBool()
                                      ? QStringLiteral("›")
                                      : QStringLiteral("⌄"));
                rect.adjust(14, 0, 0, 0);
            }
            painter->drawText(rect, Qt::AlignVCenter | Qt::AlignLeft,
                              painter->fontMetrics().elidedText(adjusted.text, Qt::ElideRight, rect.width()));
            painter->restore();
            return;
        }
        if (index.data(SidebarNavigation::CurrentRole).toBool())
            adjusted.state |= QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, adjusted, index);
    }
};
SidebarMount mountFor(const Conversation& session, const QHash<QString, GitSnapshot>& snapshots) {
    SidebarMount mount;
    mount.path = session.workspace;
    if (!snapshots.contains(session.workspace))
        return mount;
    const auto snapshot = snapshots.value(session.workspace);
    mount.git = true;
    mount.repositoryId = snapshot.repositoryId;
    mount.mainPath = snapshot.mainWorktreePath.isEmpty() ? snapshot.repoRoot : snapshot.mainWorktreePath;
    mount.branch = snapshot.branch;
    mount.headSha = snapshot.headSha;
    mount.worktree = snapshot.isWorktree;
    return mount;
}
} // namespace
SidebarNavigation::SidebarNavigation(QListWidget* list, QToolButton* grouping, QLabel* label,
                                     SettingsPage* settings, QObject* parent)
    : QObject(parent), m_list(list), m_grouping(grouping), m_label(label), m_settings(settings) {
    setObjectName("sidebarNavigation");
    grouping->setObjectName("agentGroupingButton");
    grouping->setAccessibleName("Change agent grouping mode");
    label->setObjectName("agentSectionLabel");
    list->setItemDelegate(new SidebarDelegate(list));
    connect(grouping, &QToolButton::clicked, this, &SidebarNavigation::showGroupingMenu);
    connect(settings, &SettingsPage::preferencesChanged, this, &SidebarNavigation::rebuild);
    connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto kind = item->data(KindRole).toInt();
        if (kind == SidebarRow::ShowMore) {
            m_visible += 20;
            rebuild();
        } else if (kind == SidebarRow::Header && item->data(CollapsibleRole).toBool())
            toggleGroup(item->data(KeyRole).toString());
    });
    auto* midnight = new QTimer(this);
    midnight->setInterval(60000);
    connect(midnight, &QTimer::timeout, this, &SidebarNavigation::rebuild);
    midnight->start();
}
SidebarNavigation::~SidebarNavigation() {
    const auto service = m_git;
    const auto ids = m_pending.keys();
    if (service)
        disconnect(service, nullptr, this, nullptr);
    for (const auto id : ids)
        if (service)
            service->cancel(id);
}
QStringList SidebarNavigation::preferenceList(const QString& path) const {
    QStringList result;
    if (m_settings)
        for (const auto& value : m_settings->preference(path).toArray())
            result.append(value.toString());
    return result;
}
bool SidebarNavigation::saveArray(const QString& path, const QStringList& values) {
    if (!m_settings)
        return false;
    QJsonArray array;
    for (const auto& value : values)
        array.append(value);
    const QPointer<SidebarNavigation> self(this);
    const bool saved = m_settings->setPreference(path, array);
    if (!self)
        return saved;
    if (!saved)
        emit errorOccurred("Could not save sidebar preferences. Previous state was preserved.");
    return saved;
}
bool SidebarNavigation::isPinned(const QString& id) const {
    return preferenceList("sidebar.pinnedAgentIds").contains(id);
}
bool SidebarNavigation::setPinned(const QString& id, bool pinned) {
    const auto found = std::find_if(m_sessions.begin(), m_sessions.end(), [&](const auto& session) {
        return session.id == id && !session.archived;
    });
    if (found == m_sessions.end())
        return false;
    auto values = preferenceList("sidebar.pinnedAgentIds");
    if (values.contains(id) == pinned)
        return true;
    values.removeAll(id);
    if (pinned)
        values.prepend(id);
    return saveArray("sidebar.pinnedAgentIds", values);
}
bool SidebarNavigation::forgetPinned(const QString& id) {
    auto values = preferenceList("sidebar.pinnedAgentIds");
    if (!values.removeAll(id))
        return true;
    return saveArray("sidebar.pinnedAgentIds", values);
}
bool SidebarNavigation::movePinned(const QString& id, int offset) {
    auto values = preferenceList("sidebar.pinnedAgentIds");
    const int index = values.indexOf(id);
    if (index < 0 || index + offset < 0 || index + offset >= values.size())
        return false;
    values.move(index, index + offset);
    return saveArray("sidebar.pinnedAgentIds", values);
}
bool SidebarNavigation::setGroupingMode(const QString& mode) {
    if (!m_settings || !QStringList{"age", "workspace"}.contains(mode))
        return false;
    const QPointer<SidebarNavigation> self(this);
    const bool saved = m_settings->setPreference("sidebar.agentListGroupingMode", mode);
    if (self && !saved)
        emit errorOccurred("Could not save sidebar grouping. Previous state was preserved.");
    return saved;
}
void SidebarNavigation::showGroupingMenu() {
    if (!m_grouping || !m_settings)
        return;
    auto* menu = new QMenu(m_grouping);
    menu->setObjectName("agentGroupingMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto* choices = new QActionGroup(menu);
    choices->setExclusive(true);
    for (const auto& mode : {QString("age"), QString("workspace")}) {
        auto* action = menu->addAction(mode == "age" ? "Age" : "Workspace");
        action->setObjectName("groupBy_" + mode);
        action->setCheckable(true);
        action->setChecked(m_settings->preference("sidebar.agentListGroupingMode", "age") == mode);
        choices->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] { setGroupingMode(mode); });
    }
    menu->popup(m_grouping->mapToGlobal(QPoint(0, m_grouping->height())));
}
void SidebarNavigation::toggleGroup(const QString& key) {
    auto values = preferenceList("sidebar.collapsedWorkspaceGroupKeys");
    if (values.contains(key))
        values.removeAll(key);
    else
        values.append(key);
    saveArray("sidebar.collapsedWorkspaceGroupKeys", values);
}
void SidebarNavigation::refresh(const QList<Conversation>& sessions, const QSet<QString>& busy,
                                const QString& current, const QString& query) {
    m_sessions = sessions;
    m_busy = busy;
    m_current = current;
    m_query = query;
    rebuild();
}
void SidebarNavigation::rebuild() {
    if (!m_list || !m_settings)
        return;
    SidebarProjectionOptions options;
    options.mode = m_settings->preference("sidebar.agentListGroupingMode", "age").toString();
    options.query = m_query;
    options.currentId = m_current;
    options.pinnedIds = preferenceList("sidebar.pinnedAgentIds");
    options.workspaceOrder = preferenceList("sidebar.workspaceGroupOrder");
    options.collapsedKeys = preferenceList("sidebar.collapsedWorkspaceGroupKeys");
    options.visibleCount = m_visible;
    options.sessionStartedAt = m_started;
    options.repositoryWorktrees = m_worktrees;
    QList<SidebarEntry> entries;
    for (const auto& session : m_sessions) {
        SidebarEntry entry;
        entry.conversation = session;
        if (!session.workspace.isEmpty())
            entry.mounts.append(mountFor(session, m_snapshots));
        entries.append(entry);
    }
    if (m_previousCurrent != m_current) {
        m_previousCurrent = m_current;
        for (const auto& entry : entries)
            if (entry.conversation.id == m_current) {
                if (entry.mounts.isEmpty())
                    options.collapsedKeys.removeAll("__no-workspace__");
                for (const auto& mount : entry.mounts) {
                    const auto key = SidebarModel::workspaceKey(mount);
                    options.collapsedKeys.removeAll(key);
                    options.collapsedKeys.removeAll(key + '/' + SidebarModel::worktreeKey(mount));
                }
            }
        // Selection reveals its current group immediately; explicit later group
        // collapses are retained. Preference persistence follows the normal path.
        if (options.collapsedKeys != preferenceList("sidebar.collapsedWorkspaceGroupKeys")) {
            const QPointer<SidebarNavigation> self(this);
            const bool saved = saveArray("sidebar.collapsedWorkspaceGroupKeys", options.collapsedKeys);
            if (!self)
                return;
            if (!saved)
                options.collapsedKeys = preferenceList("sidebar.collapsedWorkspaceGroupKeys");
        }
    }
    const auto projection = SidebarModel::project(entries, options);
    const auto oldCurrent =
        m_list->currentItem() ? m_list->currentItem()->data(KeyRole).toString() : QString();
    const int scroll = m_list->verticalScrollBar()->value();
    QSignalBlocker blocked(m_list);
    m_list->clear();
    QListWidgetItem* select = nullptr;
    const double scale = qMax(1, qApp->font().pixelSize()) / 13.0;
    for (const auto& row : projection.rows) {
        auto* item = new QListWidgetItem(row.title, m_list);
        item->setData(Qt::UserRole, row.agentId);
        item->setData(KindRole, row.kind);
        item->setData(KeyRole, row.key);
        item->setData(IndentRole, row.indent);
        item->setData(PinnedRole, row.pinned);
        item->setData(CurrentRole, row.agentId == m_current && !row.agentId.isEmpty());
        item->setData(CollapsedRole, row.collapsed);
        item->setData(CollapsibleRole, row.collapsible);
        item->setToolTip(row.path.isEmpty() ? row.title : row.title + '\n' + row.path);
        item->setSizeHint(QSize(0, qRound((row.kind == SidebarRow::Header ? 28 : 32) * scale)));
        if (row.kind == SidebarRow::Agent) {
            item->setIcon(icon("agent", QColor(m_busy.contains(row.agentId) ? "#2eab83" : "#009dda")));
            if (row.agentId == m_current && (!select || row.key == oldCurrent))
                select = item;
        } else
            item->setFlags(Qt::ItemIsEnabled);
    }
    if (select)
        m_list->setCurrentItem(select);
    m_list->verticalScrollBar()->setValue(scroll);
    if (m_label)
        m_label->setText(projection.pinnedCount ? "Pinned" : "Agents");
    if (m_grouping)
        m_grouping->setText(options.mode == "workspace" ? "Group by Workspace" : "Group by Age");
}
void SidebarNavigation::setGitService(GitService* service) {
    if (m_git == service)
        return;
    if (m_git)
        disconnect(m_git, nullptr, this, nullptr);
    const QPointer<GitService> next(service);
    if (!clearGitState())
        return;
    m_git = next;
    const QPointer<SidebarNavigation> self(this);
    rebuild();
    if (!self || m_git != next)
        return;
    if (!next)
        return;
    connect(next, &GitService::completed, this, [this, next](quint64 id, const GitResult& result) {
        if (next && m_git == next)
            gitCompleted(id, result);
    });
    connect(next, &GitService::repositoryTopologyChanged, this, [this, next] {
        if (!next || m_git != next || !clearGitState())
            return;
        const QPointer<SidebarNavigation> self(this);
        rebuild();
        if (self)
            workspaceContextChanged();
    });
    workspaceContextChanged();
}
bool SidebarNavigation::clearGitState() {
    // Retire IDs before cancellation can emit completed. Replacement services
    // can reuse request IDs; topology changes must not revive an old snapshot.
    const auto service = m_git;
    const auto pending = m_pending.keys();
    const auto epoch = ++m_gitEpoch;
    m_pending.clear();
    m_worktreeRequests.clear();
    m_queue.clear();
    m_inspected.clear();
    m_worktreesRequested.clear();
    m_snapshots.clear();
    m_worktrees.clear();
    const QPointer<SidebarNavigation> self(this);
    for (const auto id : pending) {
        if (service)
            service->cancel(id);
        if (!self || m_gitEpoch != epoch)
            return false;
    }
    return true;
}
void SidebarNavigation::workspaceContextChanged() {
    if (!m_git)
        return;
    QSet<QString> paths;
    for (const auto& session : m_sessions)
        if (!session.archived && !session.workspace.isEmpty())
            paths.insert(session.workspace);
    for (const auto& path : paths)
        if (!m_inspected.contains(path) && !m_queue.contains(path) && !m_pending.values().contains(path))
            m_queue.append(path);
    pumpInspections();
}
void SidebarNavigation::pumpInspections() {
    while (m_git && m_pending.size() < 4 && !m_queue.isEmpty()) {
        const auto path = m_queue.takeFirst();
        m_inspected.insert(path);
        m_pending.insert(m_git->inspect(path), path);
    }
}
void SidebarNavigation::gitCompleted(quint64 id, const GitResult& result) {
    if (!m_pending.contains(id))
        return;
    const auto path = m_pending.take(id);
    const bool trees = m_worktreeRequests.remove(id);
    const bool stillMounted = std::any_of(m_sessions.begin(), m_sessions.end(), [&](const auto& session) {
        return !session.archived && session.workspace == path;
    });
    if (stillMounted && result.ok) {
        if (!trees) {
            m_snapshots[path] = result.snapshot;
            const auto repo = result.snapshot.repositoryId;
            if (m_git && !m_worktreesRequested.contains(repo)) {
                m_worktreesRequested.insert(repo);
                const auto request = m_git->worktrees(path);
                m_pending.insert(request, path);
                m_worktreeRequests.insert(request);
            }
        } else if (m_snapshots.contains(path)) {
            const auto snapshot = m_snapshots.value(path);
            QList<SidebarMount> mounts;
            for (const auto& tree : result.worktrees) {
                SidebarMount mount;
                mount.path = tree.path;
                mount.git = true;
                mount.repositoryId = snapshot.repositoryId;
                mount.mainPath = snapshot.mainWorktreePath;
                mount.worktree = !tree.main;
                mount.createdAt = tree.createdAt;
                mount.branch = tree.branch;
                mount.headSha = tree.headSha;
                mounts.append(mount);
            }
            m_worktrees[snapshot.repositoryId] = mounts;
        }
        const QPointer<SidebarNavigation> self(this);
        rebuild();
        if (!self)
            return;
    }
    pumpInspections();
}
} // namespace ade
