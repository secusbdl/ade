#pragma once
#include "core/ProviderConfig.h"
#include <QDialog>
class QLineEdit; class QSpinBox;
class QComboBox;
class QListWidget;
class QLabel;
namespace ade {
class ProviderDialog : public QDialog {
    Q_OBJECT
public:
    ProviderDialog(QList<ProviderConfig> providers, QJsonObject catalogs = {}, QWidget* parent = nullptr);
    QList<ProviderConfig> providers() const { return m_providers; }
    QJsonObject modelCatalogs() const { return m_catalogs; }
private:
    void selectProvider(int row);
    void applyProvider();
    void importKeys();
    QList<ProviderConfig> m_providers;
    QJsonObject m_catalogs;
    ProviderConfig m_discoveryProvider;
    QListWidget* m_list;
    QLineEdit *m_name, *m_url, *m_key, *m_model;
    QComboBox *m_protocol,*m_reasoning,*m_thinking;
    QSpinBox *m_thinkingBudget,*m_maxTokens;
    QLabel* m_feedback;
    int m_selected=-1;
};
}
