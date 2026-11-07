#pragma once
#include "core/ProviderConfig.h"
#include <QDialog>
#include <QMap>
#include <functional>
#include <optional>
class QDialogButtonBox;class QLineEdit;class QPlainTextEdit;class QComboBox;class QSpinBox;class QCheckBox;class QLabel;class QPushButton;
namespace ade {
class CustomModelDialog final : public QDialog {
 Q_OBJECT
public:
 using Committer=std::function<bool(const CustomModelConfig&,QString*)>;
 CustomModelDialog(QList<ProviderConfig> providers,std::optional<CustomModelConfig> model,
     QString initialProviderId,Committer committer,QWidget* parent=nullptr);
protected:
 void done(int result)override;
 void changeEvent(QEvent*)override;
 bool eventFilter(QObject*,QEvent*)override;
private:
 std::optional<CustomModelConfig> candidate(QString* error)const;
 void save();
 void updateColors();
 QList<ProviderConfig> m_providers;
 Committer m_commit;
 bool m_saving=false,m_closed=false,m_coloring=false;
 QLineEdit *m_modelId=nullptr,*m_displayName=nullptr;
 QPlainTextEdit *m_description=nullptr,*m_options=nullptr,*m_headers=nullptr;
 QComboBox* m_provider=nullptr;
 QSpinBox* m_context=nullptr;
 QCheckBox *m_thinking=nullptr,*m_tools=nullptr;
 QMap<QString,QCheckBox*> m_capabilities;
 QLabel* m_status=nullptr;
 QPushButton* m_save=nullptr;
 QWidget* m_formWidget=nullptr;QDialogButtonBox* m_buttons=nullptr;
};
}
