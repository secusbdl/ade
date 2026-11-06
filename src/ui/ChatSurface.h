#pragma once
#include "Composer.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace ade {
// The empty history and composer form a vertically centered group. Once a
// message exists, history occupies the remaining height above the same footer.
class ChatSurface final : public QWidget {
  public:
    explicit ChatSurface(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("chatSurface");
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(4, 4, 4, 4);
        m_layout->setSpacing(0);
        m_header = new QWidget(this);
        m_header->setObjectName("chatHeader");
        m_header->setFixedHeight(28);
        m_headerLayout = new QHBoxLayout(m_header);
        m_headerLayout->setContentsMargins(0, 0, 4, 0);
        m_headerLayout->setSpacing(0);
        m_layout->addWidget(m_header);
        m_layout->addStretch(1);
        m_footer = new QWidget(this);
        m_footer->setObjectName("chatFooter");
        m_footer->setMaximumWidth(768);
        m_footer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        m_footer->installEventFilter(this);
        m_footerLayout = new QVBoxLayout(m_footer);
        m_footerLayout->setContentsMargins(4, 0, 4, 4);
        m_footerLayout->setSpacing(4);
        auto* center = new QHBoxLayout;
        center->setContentsMargins(0, 0, 0, 0);
        center->setSpacing(0);
        center->addStretch();
        center->addWidget(m_footer, 1);
        center->addStretch();
        m_layout->addLayout(center);
        m_layout->addStretch(1);
    }
    QHBoxLayout* headerLayout() const {
        return m_headerLayout;
    }
    QVBoxLayout* footerLayout() const {
        return m_footerLayout;
    }
    void setHistoryWidget(QWidget* history) {
        m_history = history;
        m_layout->insertWidget(2, history, 0);
        history->hide();
    }
    void setHistoryPresent(bool present) {
        if (!m_history)
            return;
        m_history->setVisible(present);
        m_layout->setStretch(1, present ? 0 : 1);
        m_layout->setStretch(2, present ? 1 : 0);
        m_layout->setStretch(m_layout->count() - 1, present ? 0 : 1);
    }
    void setUiScale(double scale) {
        m_header->setFixedHeight(qRound(28 * scale));
        m_footer->setMaximumWidth(qRound(768 * scale));
        m_layout->setContentsMargins(qRound(4 * scale), qRound(4 * scale), qRound(4 * scale),
                                     qRound(4 * scale));
        m_footerLayout->setContentsMargins(qRound(4 * scale), 0, qRound(4 * scale), qRound(4 * scale));
        m_footerLayout->setSpacing(qRound(4 * scale));
    }
    QSize minimumSizeHint() const override {
        return {0, 0};
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        scheduleComposerFit();
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == m_footer && event->type() == QEvent::LayoutRequest)
            scheduleComposerFit();
        return QWidget::eventFilter(object, event);
    }

  private:
    void scheduleComposerFit() {
        if (m_fitPending)
            return;
        m_fitPending = true;
        QTimer::singleShot(0, this, [this] {
            m_fitPending = false;
            auto* composer = m_footer->findChild<Composer*>();
            if (!composer)
                return;
            const int footerHeight = m_footer->hasHeightForWidth()
                                         ? m_footer->heightForWidth(m_footer->width())
                                         : m_footer->sizeHint().height();
            const int other = footerHeight - composer->height();
            composer->setAvailableHeightLimit(height() - m_header->height() - other -
                                              m_layout->contentsMargins().top() -
                                              m_layout->contentsMargins().bottom() - 8);
        });
    }
    bool m_fitPending = false;
    QVBoxLayout *m_layout, *m_footerLayout;
    QHBoxLayout* m_headerLayout;
    QWidget *m_header, *m_footer, *m_history = nullptr;
};
} // namespace ade
