#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <functional>
class QWidget;
class QListWidget;
namespace ade {
class AgentCycleOverlay;
// A session-local MRU snapshot. Preview never selects a QListWidget item or
// changes the open conversation; only a terminal commit requests that change.
class AgentCycleController final : public QObject {
    Q_OBJECT
public:
    AgentCycleController(QWidget* window, QListWidget* agents, QObject* parent = nullptr);
    ~AgentCycleController() override;
    void setActivationGuard(std::function<bool()> guard);
    void setAvailableIds(const QStringList& ids);
    void observeCurrent(const QString& id);
    void cycle(int direction);
    void stepImmediately(int direction);
    void commit();
    void cancel();
    bool active() const { return m_active; }
    QString previewId() const { return m_preview; }
    QString currentId() const { return m_current; }
    QStringList snapshot() const { return m_snapshot; }
    QStringList history() const { return m_history; }
signals:
    void previewChanged(QString id);
    void openRequested(QString id);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    bool allowed();
    QStringList visibleIds() const;
    bool publish(quint64 epoch, bool reveal);
    void reset();
    QPointer<QWidget> m_window;
    QPointer<QListWidget> m_agents;
    QPointer<AgentCycleOverlay> m_overlay;
    std::function<bool()> m_guard;
    QSet<QString> m_available;
    QStringList m_history, m_snapshot;
    QString m_current, m_preview;
    int m_cursor = -1;
    bool m_active = false;
    quint64 m_epoch = 0;
};
}
