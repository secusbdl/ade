#pragma once
#include "core/ProviderConfig.h"
#include <QDialog>
#include <QList>
class QLineEdit;
class QLabel;
class QScrollArea;
class QVBoxLayout;
namespace ade {
class AddProviderDialog final : public QDialog {
    Q_OBJECT
public:
    explicit AddProviderDialog(const QList<ProviderConfig>& providers = {}, QWidget* parent = nullptr);
    QString selectedTemplateId() const { return m_selected; }
    QStringList visibleTemplateIds() const;
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    void rebuildLayout();
    void updateColors();
    QLineEdit* m_search = nullptr;
    QLabel* m_empty = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_content = nullptr;
    QWidget* m_modelHeading = nullptr;
    QVBoxLayout* m_sections = nullptr;
    QString m_selected;
    bool m_layoutPending = false, m_coloring = false;
    struct Section;
    QList<Section*> m_groups;
};
}
