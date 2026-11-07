#pragma once
#include <QComboBox>
namespace ade {
// Keeps the exact selected identity and editable query intact. Only the idle
// selected-label presentation is anchored at its beginning and right-elided.
class ModelSelector final : public QComboBox {
public:
    explicit ModelSelector(QWidget* parent = nullptr);
};
}
