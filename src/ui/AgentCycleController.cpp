#include "AgentCycleController.h"
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QListWidget>
#include <QPainter>
#include <QScrollBar>
#include <QWidget>
#include <algorithm>

namespace ade {
class AgentCycleOverlay final : public QWidget {
public:
    explicit AgentCycleOverlay(QListWidget* list) : QWidget(list->viewport()), m_list(list) {
        setObjectName("agentCyclePreview");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }
    void display(QString id, QString current) {
        const QPointer<AgentCycleOverlay> self(this);
        const auto epoch = ++m_epoch;
        const auto alive = [self, epoch] { return self && self->m_epoch == epoch; };
        m_id = id;
        m_current = current;
        setProperty("previewAgentId", id);
        if (!alive()) return;
        setAccessibleName(id.isEmpty() ? QString() : QStringLiteral("Preview agent: ") + id);
        if (!alive() || !m_list) return;
        setGeometry(m_list->viewport()->rect());
        if (!alive()) return;
        setVisible(!id.isEmpty());
        if (!alive()) return;
        if (!id.isEmpty()) { raise(); if (alive()) update(); }
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_list || m_id.isEmpty() || m_id == m_current) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        auto color = palette().color(QPalette::Text);
        color.setAlpha(20); // pinned foreground/8 preview background
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int i = 0; i < m_list->count(); ++i) {
            const auto* item = m_list->item(i);
            if (item->data(Qt::UserRole).toString() == m_id && !item->isHidden())
                painter.drawRoundedRect(m_list->visualItemRect(item).adjusted(1, 1, -1, -1), 5, 5);
        }
    }
private:
    QPointer<QListWidget> m_list;
    QString m_id, m_current;
    quint64 m_epoch = 0;
};

AgentCycleController::AgentCycleController(QWidget* window, QListWidget* agents, QObject* parent)
    : QObject(parent ? parent : window), m_window(window), m_agents(agents),
      m_overlay(new AgentCycleOverlay(agents)) {
    setObjectName("agentCycleController");
    qApp->installEventFilter(this);
    connect(agents->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_overlay) m_overlay->update();
    });
    connect(agents->model(), &QAbstractItemModel::modelReset, this, [this] {
        if (m_overlay) m_overlay->update();
    });
}
AgentCycleController::~AgentCycleController() {
    if (qApp) qApp->removeEventFilter(this);
    delete m_overlay.data();
}
void AgentCycleController::setActivationGuard(std::function<bool()> guard) { m_guard = std::move(guard); }
bool AgentCycleController::allowed() {
    if (!m_window || !m_agents || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    const auto guard = m_guard;
    const QPointer<AgentCycleController> self(this);
    const bool enabled = !guard || guard();
    return self && enabled;
}
QStringList AgentCycleController::visibleIds() const {
    QStringList ids;
    if (!m_agents) return ids;
    for (int i = 0; i < m_agents->count(); ++i) {
        const auto* item = m_agents->item(i);
        const auto id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty() && m_available.contains(id) && !item->isHidden()
            && (item->flags() & Qt::ItemIsSelectable) && !ids.contains(id)) ids.append(id);
    }
    return ids;
}
void AgentCycleController::setAvailableIds(const QStringList& ids) {
    QSet<QString> available;
    for (const auto& id : ids) if (!id.isEmpty()) available.insert(id);
    if (available == m_available) return;
    m_available = std::move(available);
    m_history.removeIf([this](const QString& id) { return !m_available.contains(id); });
    // Keep the original order/cursor. Removed IDs are skipped without admitting
    // newly inserted chats into an in-flight snapshot.
    if (!m_preview.isEmpty() && !m_available.contains(m_preview)) {
        m_preview.clear();
        publish(++m_epoch, false);
    }
}
void AgentCycleController::observeCurrent(const QString& id) {
    if (id == m_current) return;
    m_current = id;
    if (id.isEmpty()) m_history.clear();
    else { m_history.removeAll(id); m_history.append(id); }
    reset();
    publish(m_epoch, false);
}
void AgentCycleController::reset() {
    ++m_epoch;
    m_active = false;
    m_snapshot.clear();
    m_preview.clear();
    m_cursor = -1;
}
bool AgentCycleController::publish(quint64 epoch, bool reveal) {
    const QPointer<AgentCycleController> self(this);
    if (reveal && m_agents && !m_preview.isEmpty()) {
        for (int i = 0; i < m_agents->count(); ++i) {
            auto* item = m_agents->item(i);
            if (item->data(Qt::UserRole).toString() == m_preview && !item->isHidden()) {
                m_agents->scrollToItem(item, QAbstractItemView::EnsureVisible);
                break;
            }
        }
    }
    if (!self || m_epoch != epoch) return false;
    if (m_overlay) m_overlay->display(m_preview, m_current);
    if (!self || m_epoch != epoch) return false;
    emit previewChanged(m_preview);
    return self && m_epoch == epoch;
}
void AgentCycleController::cycle(int direction) {
    const QPointer<AgentCycleController> self(this);
    if (!allowed() || !self) return;
    direction = direction < 0 ? -1 : 1;
    if (!m_active) {
        const auto ordered = visibleIds();
        if (ordered.isEmpty()) return;
        m_snapshot.clear();
        for (auto it = m_history.crbegin(); it != m_history.crend(); ++it)
            if (ordered.contains(*it)) m_snapshot.append(*it);
        for (const auto& id : ordered) if (!m_snapshot.contains(id)) m_snapshot.append(id);
        m_cursor = m_snapshot.indexOf(m_current);
        if (m_cursor < 0) m_cursor = direction > 0 ? -1 : 0;
        m_active = true;
    }
    for (int tries = 0; tries < m_snapshot.size(); ++tries) {
        m_cursor = (m_cursor + direction + m_snapshot.size()) % m_snapshot.size();
        if (m_available.contains(m_snapshot[m_cursor])) {
            m_preview = m_snapshot[m_cursor];
            publish(++m_epoch, true);
            return;
        }
    }
    cancel();
}
void AgentCycleController::stepImmediately(int direction) {
    const QPointer<AgentCycleController> self(this);
    const auto expected = m_epoch + 1;
    cycle(direction);
    // Do not commit a replacement/canceled preview created by an observer.
    if (self && m_epoch == expected && m_active) commit();
}
void AgentCycleController::commit() {
    if (!m_active) return;
    const QPointer<AgentCycleController> self(this);
    const auto id = m_preview;
    const auto original = m_current;
    reset();
    const auto epoch = m_epoch;
    if (!publish(epoch, false) || !self) return;
    if (id.isEmpty() || id == original || !m_available.contains(id)) return;
    if (!allowed() || !self || m_epoch != epoch) return;
    emit openRequested(id); // host validates/persists before observeCurrent
}
void AgentCycleController::cancel() {
    if (!m_active && m_preview.isEmpty()) return;
    reset();
    publish(m_epoch, false);
}
bool AgentCycleController::eventFilter(QObject* watched, QEvent* event) {
    const QPointer<AgentCycleController> self(this);
    const QPointer<QObject> receiver(watched);
    const bool enabled = allowed();
    if (!self) return !receiver;
    if (!enabled) { cancel(); return !receiver; }
    if (watched == m_window && (event->type() == QEvent::Close || event->type() == QEvent::Hide)) {
        cancel();
        return !receiver;
    }
    if (watched == m_window && event->type() == QEvent::WindowDeactivate) {
        commit(); // pinned window blur commits; it does not cancel
        return !receiver;
    }
    auto* target = qobject_cast<QWidget*>(watched);
    if (!target || target->window() != m_window) return false;
    if (event->type() == QEvent::Resize && m_agents && target == m_agents->viewport()) {
        if (m_overlay) m_overlay->setGeometry(m_agents->viewport()->rect());
        return false;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
        && event->type() != QEvent::ShortcutOverride) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    const auto modifiers = key->modifiers();
    const bool tab = (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab)
        && modifiers.testFlag(Qt::ControlModifier)
        && !(modifiers & (Qt::AltModifier | Qt::MetaModifier));
    const bool escape = m_active && key->key() == Qt::Key_Escape;
    if (event->type() == QEvent::ShortcutOverride && (tab || escape)) { key->accept(); return true; }
    if (event->type() == QEvent::KeyPress && tab) {
        key->accept();
        if (!key->isAutoRepeat()) cycle(key->key() == Qt::Key_Backtab || modifiers.testFlag(Qt::ShiftModifier) ? -1 : 1);
        return true;
    }
    if (event->type() == QEvent::KeyPress && escape) { key->accept(); cancel(); return true; }
    if (event->type() == QEvent::KeyRelease && m_active && key->key() == Qt::Key_Control && !key->isAutoRepeat()) {
        key->accept(); commit(); return true;
    }
    return false;
}
}
