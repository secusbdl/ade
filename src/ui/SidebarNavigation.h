#pragma once
#include "SidebarModel.h"
#include "git/GitTypes.h"
#include <QObject>
#include <QPointer>
class QListWidget;
class QToolButton;
class QLabel;
namespace ade {
class SettingsPage;
class GitService;
class SidebarNavigation final : public QObject {
    Q_OBJECT
  public:
    enum Role {
        KindRole = Qt::UserRole + 1,
        KeyRole,
        IndentRole,
        PinnedRole,
        CurrentRole,
        CollapsedRole,
        CollapsibleRole
    };
    SidebarNavigation(QListWidget* list, QToolButton* grouping, QLabel* label, SettingsPage* settings,
                      QObject* parent = nullptr);
    ~SidebarNavigation() override;
    void refresh(const QList<Conversation>& sessions, const QSet<QString>& busy, const QString& current,
                 const QString& query);
    void setGitService(GitService* service);
    void workspaceContextChanged();
    bool isPinned(const QString& id) const;
    bool setPinned(const QString& id, bool pinned);
    bool movePinned(const QString& id, int offset);
    bool forgetPinned(const QString& id);
    bool setGroupingMode(const QString& mode);
  signals:
    void errorOccurred(QString error);

  private:
    void rebuild();
    void showGroupingMenu();
    void toggleGroup(const QString& key);
    void pumpInspections();
    bool saveArray(const QString& path, const QStringList& values);
    void gitCompleted(quint64 id, const GitResult& result);
    bool clearGitState();
    QStringList preferenceList(const QString& path) const;
    QPointer<QListWidget> m_list;
    QPointer<QToolButton> m_grouping;
    QPointer<QLabel> m_label;
    QPointer<SettingsPage> m_settings;
    QPointer<GitService> m_git;
    QList<Conversation> m_sessions;
    QSet<QString> m_busy;
    QString m_current, m_query, m_previousCurrent;
    qint64 m_started = QDateTime::currentMSecsSinceEpoch();
    int m_visible = 10;
    QHash<QString, GitSnapshot> m_snapshots;
    QHash<QString, QList<SidebarMount>> m_worktrees;
    QSet<QString> m_inspected, m_worktreesRequested;
    QHash<quint64, QString> m_pending;
    QSet<quint64> m_worktreeRequests;
    QStringList m_queue;
    quint64 m_gitEpoch = 0;
};
} // namespace ade
