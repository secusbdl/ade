#include "ModelSelector.h"
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionFrame>
namespace ade {
namespace {
class ModelLabelEdit final : public QLineEdit {
public:
    explicit ModelLabelEdit(QComboBox* owner) : QLineEdit(owner), m_owner(owner) {}
protected:
    void paintEvent(QPaintEvent* event) override {
        if (hasFocus() || !m_owner || m_owner->currentIndex() < 0
            || text() != m_owner->itemText(m_owner->currentIndex())) {
            QLineEdit::paintEvent(event);
            return;
        }
        // QLineEdit's hidden editing caret may scroll the selected text to its
        // tail after a programmatic restore. Paint a stable idle label instead
        // of moving that caret, which would disrupt genuine search/editing.
        QStyleOptionFrame option;
        initStyleOption(&option);
        QPainter painter(this);
        style()->drawPrimitive(QStyle::PE_PanelLineEdit, &option, &painter, this);
        auto content = style()->subElementRect(QStyle::SE_LineEditContents, &option, this);
        const auto margins = textMargins();
        content.adjust(margins.left()+2, margins.top(), -margins.right()-2, -margins.bottom());
        painter.setClipRect(content);
        painter.setFont(font());
        const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        painter.setPen(palette().color(group, QPalette::Text));
        painter.drawText(content, QStyle::visualAlignment(layoutDirection(), Qt::AlignLeft | Qt::AlignVCenter),
                         fontMetrics().elidedText(text(), Qt::ElideRight, qMax(0, content.width())));
    }
private:
    QPointer<QComboBox> m_owner;
};
}
ModelSelector::ModelSelector(QWidget* parent) : QComboBox(parent) {
    setLineEdit(new ModelLabelEdit(this));
    // Child text changes are not suppressed by the combo's QSignalBlocker
    // during catalog/chat restoration. Full identity remains available on hover.
    connect(lineEdit(), &QLineEdit::textChanged, this,
            [this](const QString& text) { setToolTip(text); });
}
}
