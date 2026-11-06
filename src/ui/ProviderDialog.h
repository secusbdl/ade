#pragma once
#include "core/ProviderConfig.h"
#include <QDialog>
#include <QPointer>
class QLineEdit; class QSpinBox;
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QMessageBox;
class QGroupBox;
class QFormLayout;
namespace ade {
class CodexAppServer;
class OpenCodeModelDiscovery;
class ClaudeModelDiscovery;
class ProviderService;
class AddProviderDialog;
class ProviderDialog : public QDialog {
    Q_OBJECT
public:
    ProviderDialog(QList<ProviderConfig> providers, QJsonObject catalogs = {}, QWidget* parent = nullptr);
    ~ProviderDialog() override;
    QList<ProviderConfig> providers() const;
    QJsonObject modelCatalogs() const { return m_catalogs; }
    bool selectProviderId(const QString& providerId);
public slots:
    void startAddProvider();
protected:
    void done(int result) override;
private:
    void selectProvider(int row);
    void removeSelectedProvider();
    void removeProviderDraft(const QString& providerId);
    void applyProvider();
    void importKeys();
    ProviderConfig currentConfig() const;
    void beginHarnessConnection(bool login);
    void beginOpenCodeDiscovery();
    void beginClaudeDiscovery();
    void finishHarnessConnection();
    void beginApiOperation(bool validation);
    void cancelOperations();
    void updateOperationControls();
    void updateTemplateHelp();
    QPointer<AddProviderDialog> m_addPicker;
    QPointer<QMessageBox> m_removePrompt;
    QPushButton* m_removeProvider=nullptr;
    QList<QWidget*> m_selectedControls;
    QLabel* m_templateHelp=nullptr;
    QPushButton *m_keyHelp=nullptr,*m_planHelp=nullptr;
    QList<QWidget*> m_operationControls;
    QPointer<ProviderService> m_service;
    QPointer<OpenCodeModelDiscovery> m_openCodeDiscovery;
    QPointer<ClaudeModelDiscovery> m_claudeDiscovery;
    quint64 m_operationEpoch=0;
    bool m_apiBusy=false,m_closing=false;
    QList<ProviderConfig> m_providers;
    QJsonObject m_catalogs;
    ProviderConfig m_discoveryProvider;
    QListWidget* m_list;
    QLineEdit *m_name, *m_url, *m_key, *m_model;
    QComboBox *m_protocol,*m_reasoning,*m_thinking;
    QSpinBox *m_thinkingBudget,*m_maxTokens,*m_contextTokens;
    QLabel* m_feedback;
    int m_selected=-1;
    CodexAppServer* m_harness=nullptr;
    QGroupBox* m_harnessGroup=nullptr;
    QLabel* m_harnessHelp=nullptr;
    QPushButton* m_validateProvider=nullptr;
    QLineEdit* m_executable=nullptr;
    QPushButton *m_connectHarness=nullptr,*m_cancelHarness=nullptr;
    bool m_harnessLogin=false,m_harnessBusy=false;
    QFormLayout* m_form=nullptr;
};
}
