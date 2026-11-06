#pragma once
#include <QEvent>
#include <QLayout>
#include <QScrollArea>

namespace ade {
// QScrollArea caches its first child size hint. These controls are initially
// empty, then receive a question or queue, so use the current content hint.
// A small minimum keeps short windows resizable; overflow stays scrollable.
class ContentControlsScrollArea final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;
    QSize sizeHint() const override {
        const auto* body = widget();
        if (!body) return QSize(0, 0);
        const int width = qMax(1, viewport()->width());
        const auto* layout = body->layout();
        const int height = layout && layout->hasHeightForWidth()
            ? layout->totalHeightForWidth(width) : body->sizeHint().height();
        return QSize(0, qBound(0, height + 2 * frameWidth(), maximumHeight()));
    }
    QSize minimumSizeHint() const override { return QSize(0, 48); }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == widget() && event->type() == QEvent::LayoutRequest)
            updateGeometry();
        return QScrollArea::eventFilter(watched, event);
    }
};
}
