#pragma once
#include <QPainter>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QToolButton>

namespace ade {
// A visual suffix only. Activation, focus and menu behavior remain QToolButton's.
class ChevronToolButton final : public QToolButton {
public:
    using QToolButton::QToolButton;
    void setChevronVisible(bool visible) {
        if (m_chevronVisible == visible) return;
        m_chevronVisible = visible;
        updateGeometry();
        update();
    }
    bool chevronVisible() const { return m_chevronVisible; }
    QSize sizeHint() const override {
        auto size = QToolButton::sizeHint();
        if (m_chevronVisible) size.rwidth() += suffixWidth();
        return size;
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent* event) override {
        if (!m_chevronVisible) { QToolButton::paintEvent(event); return; }
        QPainter painter(this);
        QStyleOptionToolButton frame;
        initStyleOption(&frame);
        auto label = frame;
        // Draw the full hover/pressed background, then its ordinary label in a
        // smaller rectangle. The reserved suffix never overlaps text or icon.
        frame.text.clear();
        frame.icon = {};
        frame.arrowType = Qt::NoArrow;
        style()->drawComplexControl(QStyle::CC_ToolButton, &frame, &painter, this);
        const int suffix = suffixWidth();
        const auto content = QRect(0, 0, qMax(0, width() - suffix), height());
        label.rect = QStyle::visualRect(layoutDirection(), rect(), content);
        style()->drawControl(QStyle::CE_ToolButtonLabel, &label, &painter, this);
        const auto arrow = QStyle::visualRect(layoutDirection(), rect(),
                                             QRect(width() - suffix, 0, suffix, height()));
        const qreal scale = qMax(0.7, fontMetrics().height() / 16.0);
        const QPointF center(arrow.center().x(), arrow.center().y());
        painter.setRenderHint(QPainter::Antialiasing);
        const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        painter.setPen(QPen(label.palette.color(group, QPalette::ButtonText),
                            qMax(1.0, 1.2 * scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF{center + QPointF(-3 * scale, -1.5 * scale),
                                       center + QPointF(0, 1.5 * scale),
                                       center + QPointF(3 * scale, -1.5 * scale)});
    }
private:
    int suffixWidth() const { return qMax(10, qRound(fontMetrics().height() * 0.8)); }
    bool m_chevronVisible = true;
};
}
