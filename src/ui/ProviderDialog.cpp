#include "ProviderDialog.h"
#include "AddProviderDialog.h"
#include "ProviderCatalog.h"
#include "ModelSelection.h"
#include "core/ProviderStore.h"
#include "core/ProviderService.h"
#include "harness/CodexAppServer.h"
#include <QDesktopServices>
#include <QGroupBox>
#include <QPointer>
#include <QStandardItemModel>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QInputDialog>
#include <QSpinBox>
#include <QUrl>
#include <QUuid>
namespace ade {
ProviderDialog::ProviderDialog(QList<ProviderConfig> providers,QJsonObject catalogs,QWidget* parent)
    : QDialog(parent),m_providers(std::move(providers)),m_catalogs(std::move(catalogs)) {
    setWindowTitle("Models & Providers"); setObjectName("providerDialog"); resize(850,650);
    auto* outer=new QVBoxLayout(this); outer->setContentsMargins(26,24,26,24); outer->setSpacing(18);
    auto* title=new QLabel("Models & Providers"); title->setStyleSheet("font-size:22px;font-weight:600"); outer->addWidget(title);
    auto* description=new QLabel("Connect an API provider or use your subscription through an installed coding agent."); description->setWordWrap(true);outer->addWidget(description);
    auto* columns=new QHBoxLayout; outer->addLayout(columns,1);
    auto* left=new QVBoxLayout; columns->addLayout(left);m_list=new QListWidget; m_list->setMinimumWidth(210);left->addWidget(m_list);
    auto* add=new QPushButton("+ Add Provider");add->setObjectName("addProvider");left->addWidget(add);
    auto* addCodex=new QPushButton("+ Codex coding agent");addCodex->setObjectName("addCodexProvider");left->addWidget(addCodex);
    auto* import=new QPushButton("Import API key file…"); left->addWidget(import);
    auto* right=new QVBoxLayout; columns->addLayout(right,1); auto* form=new QFormLayout;m_form=form;right->addLayout(form);
    m_name=new QLineEdit;m_url=new QLineEdit;m_key=new QLineEdit;m_model=new QLineEdit;m_protocol=new QComboBox;
    m_name->setObjectName("providerName");m_url->setObjectName("providerBaseUrl");m_key->setObjectName("providerApiKey");m_model->setObjectName("providerModel");
    m_key->setEchoMode(QLineEdit::Password);m_key->setPlaceholderText("API key (encrypted when saved)");
    m_url->setPlaceholderText("https://api.example.com/v1");m_model->setPlaceholderText("Model ID from your provider");
    m_protocol->addItem("OpenAI compatible","openai");
    m_protocol->addItem("OpenAI Responses","openai-responses");
    m_protocol->addItem("Anthropic Messages","anthropic");m_protocol->addItem("Google Gemini","google");
    m_protocol->addItem("Codex managed subscription","codex-app-server");
    if(auto* model=qobject_cast<QStandardItemModel*>(m_protocol->model()))model->item(m_protocol->count()-1)->setEnabled(false);
    m_reasoning=new QComboBox;m_reasoning->addItem("Provider default",QString());for(const auto& effort:QStringList{"low","medium","high","xhigh","max","minimal","none"})m_reasoning->addItem(effort,effort);
    m_thinking=new QComboBox;m_thinking->addItem("Provider default",QString());m_thinking->addItem("Enabled","enabled");m_thinking->addItem("Disabled","disabled");
    m_thinkingBudget=new QSpinBox;m_thinkingBudget->setRange(0,200000);m_thinkingBudget->setSpecialValueText("Provider default");
    m_maxTokens=new QSpinBox;m_maxTokens->setRange(0,200000);m_maxTokens->setSpecialValueText("Provider default");
    m_contextTokens=new QSpinBox;m_contextTokens->setObjectName("providerContextWindow");m_contextTokens->setRange(0,100000000);m_contextTokens->setSpecialValueText("Unknown / use discovered model limit");
    m_contextTokens->setToolTip("Context tokens for this default model. Automatic compression uses half this limit, capped at 100,000 tokens. Unknown models keep manual /compact.");
    form->addRow("Name",m_name);form->addRow("Base URL",m_url);form->addRow("API key",m_key);form->addRow("Model",m_model);form->addRow("Protocol",m_protocol);
    form->addRow("Reasoning effort",m_reasoning);form->addRow("Thinking",m_thinking);form->addRow("Thinking budget",m_thinkingBudget);form->addRow("Output token limit",m_maxTokens);
    form->addRow("Model context limit",m_contextTokens);
    m_harnessGroup=new QGroupBox("Codex subscription");m_harnessGroup->setObjectName("codexConnectionGroup");
    auto* harnessLayout=new QVBoxLayout(m_harnessGroup);
    auto* harnessHelp=new QLabel("Uses your installed Codex CLI and its ChatGPT sign-in. ADE does not collect your subscription token. This optional coding agent runs its own tools in the selected workspace.");harnessHelp->setWordWrap(true);harnessLayout->addWidget(harnessHelp);
    m_executable=new QLineEdit;m_executable->setObjectName("codexExecutable");m_executable->setPlaceholderText("Find installed codex automatically");harnessLayout->addWidget(m_executable);
    auto* harnessButtons=new QHBoxLayout;m_connectHarness=new QPushButton("Connect ChatGPT subscription");m_connectHarness->setObjectName("connectCodexSubscription");m_cancelHarness=new QPushButton("Cancel sign-in");m_cancelHarness->setObjectName("cancelCodexSubscription");harnessButtons->addWidget(m_connectHarness);harnessButtons->addWidget(m_cancelHarness);harnessLayout->addLayout(harnessButtons);right->addWidget(m_harnessGroup);
    m_harness=new CodexAppServer(this);
    connect(m_connectHarness,&QPushButton::clicked,this,[this]{beginHarnessConnection(true);});
    connect(m_cancelHarness,&QPushButton::clicked,this,[this]{
        if(!m_harnessBusy)return;
        const QPointer<ProviderDialog> self(this);
        m_harness->cancelLogin();if(!self)return;cancelOperations();
        if(self)m_feedback->setText("Sign-in canceled. Your existing Codex account was not signed out.");
    });
    connect(m_model,&QLineEdit::textChanged,this,[this]{m_contextTokens->setValue(0);});
    connect(m_url,&QLineEdit::textChanged,this,[this]{m_contextTokens->setValue(0);});
    connect(m_protocol,&QComboBox::currentIndexChanged,this,[this]{m_contextTokens->setValue(0);});
    m_templateHelp=new QLabel;m_templateHelp->setObjectName("providerTemplateHelp");m_templateHelp->setTextFormat(Qt::PlainText);m_templateHelp->setWordWrap(true);right->addWidget(m_templateHelp);
    auto* helpButtons=new QHBoxLayout;m_keyHelp=new QPushButton("Get API key");m_keyHelp->setObjectName("providerKeyHelp");m_planHelp=new QPushButton("View plan");m_planHelp->setObjectName("providerPlanHelp");helpButtons->addWidget(m_keyHelp);helpButtons->addWidget(m_planHelp);helpButtons->addStretch();right->addLayout(helpButtons);
    connect(m_keyHelp,&QPushButton::clicked,this,[this]{const auto p=currentConfig();const auto* entry=ProviderCatalog::find(p.templateId);if(entry&&ProviderCatalog::matches(p,*entry)&&!entry->keyUrl.isEmpty())QDesktopServices::openUrl(QUrl(entry->keyUrl));});
    connect(m_planHelp,&QPushButton::clicked,this,[this]{const auto p=currentConfig();const auto* entry=ProviderCatalog::find(p.templateId);if(entry&&ProviderCatalog::matches(p,*entry)&&!entry->subscribeUrl.isEmpty())QDesktopServices::openUrl(QUrl(entry->subscribeUrl));});
    connect(m_url,&QLineEdit::textChanged,this,[this]{updateTemplateHelp();});connect(m_protocol,&QComboBox::currentIndexChanged,this,[this]{updateTemplateHelp();});
    m_feedback=new QLabel;m_feedback->setTextFormat(Qt::PlainText);m_feedback->setWordWrap(true);right->addWidget(m_feedback);right->addStretch();
    auto* apply=new QPushButton("Apply provider");right->addWidget(apply,0,Qt::AlignRight);
    auto* probeRow=new QHBoxLayout;auto* discover=new QPushButton("Fetch models");auto* validate=new QPushButton("Test connection");probeRow->addWidget(discover);probeRow->addWidget(validate);right->addLayout(probeRow);
    connect(discover,&QPushButton::clicked,this,[this]{if(currentConfig().isCodexHarness())beginHarnessConnection(false);else beginApiOperation(false);});
    connect(validate,&QPushButton::clicked,this,[this]{if(currentConfig().isCodexHarness())beginHarnessConnection(false);else beginApiOperation(true);});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);outer->addWidget(buttons);
    m_operationControls={m_list,add,addCodex,import,apply,discover,validate,buttons->button(QDialogButtonBox::Save),m_name,m_model,m_reasoning,m_executable,m_keyHelp,m_planHelp};
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{applyProvider();if(m_feedback->property("error").toBool())return;accept();});
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(apply,&QPushButton::clicked,this,&ProviderDialog::applyProvider);
    connect(import,&QPushButton::clicked,this,&ProviderDialog::importKeys);
    connect(add,&QPushButton::clicked,this,&ProviderDialog::startAddProvider);
    connect(addCodex,&QPushButton::clicked,this,[this]{if(m_apiBusy||m_harnessBusy||m_closing||m_addPicker)return;const auto p=ProviderCatalog::instantiate("codex");m_providers.append(p);m_list->addItem(p.name);m_list->setCurrentRow(m_providers.size()-1);});
    connect(m_list,&QListWidget::currentRowChanged,this,&ProviderDialog::selectProvider);
    for(const auto& p:m_providers)m_list->addItem(p.name);
    if(!m_providers.isEmpty())m_list->setCurrentRow(0);
    else {m_harnessGroup->hide();updateTemplateHelp();}
}
bool ProviderDialog::selectProviderId(const QString& providerId) {
    if(providerId.isEmpty()||m_apiBusy||m_harnessBusy||m_closing||m_addPicker)return false;
    for(int row=0;row<m_providers.size();++row){
        if(m_providers[row].id!=providerId)continue;
        const QPointer<ProviderDialog> self(this);
        m_list->setCurrentRow(row);
        return self&&!m_closing&&m_selected>=0&&m_selected<m_providers.size()&&m_providers[m_selected].id==providerId;
    }
    return false;
}
void ProviderDialog::startAddProvider() {
    if(m_apiBusy||m_harnessBusy||m_closing||m_addPicker)return;
    auto* picker=new AddProviderDialog(m_providers,this);picker->setAttribute(Qt::WA_DeleteOnClose);m_addPicker=picker;
    const auto epoch=m_operationEpoch;const QPointer<ProviderDialog> self(this);
    connect(picker,&QDialog::accepted,this,[this,self,picker,epoch]{
        if(m_closing||m_apiBusy||m_harnessBusy||m_operationEpoch!=epoch||m_addPicker!=picker)return;
        const auto p=ProviderCatalog::instantiate(picker->selectedTemplateId());if(p.id.isEmpty())return;
        m_providers.append(p);m_list->addItem(p.name);if(!self)return;
        m_list->setCurrentRow(m_providers.size()-1);
    });
    connect(picker,&QDialog::finished,this,[this,picker]{if(m_addPicker==picker)m_addPicker.clear();});
    picker->open();
}
void ProviderDialog::updateTemplateHelp() {
    if(!m_templateHelp)return;
    const auto p=currentConfig();const auto* entry=ProviderCatalog::find(p.templateId);
    const bool matched=entry&&ProviderCatalog::matches(p,*entry);
    m_templateHelp->setText(matched?entry->helpText:QString());m_templateHelp->setVisible(matched&&!entry->helpText.isEmpty());
    m_keyHelp->setVisible(matched&&!entry->keyUrl.isEmpty());m_planHelp->setVisible(matched&&!entry->subscribeUrl.isEmpty());
}
void ProviderDialog::selectProvider(int row) {
    const QPointer<ProviderDialog> self(this);const auto expected=m_operationEpoch+1;cancelOperations();if(!self||m_operationEpoch!=expected)return;
    m_selected=row;if(row<0||row>=m_providers.size())return;
    const auto& p=m_providers[row];m_name->setText(p.name);m_url->setText(p.baseUrl);m_key->setText(p.apiKey);m_model->setText(p.model);
    auto i=m_protocol->findData(p.protocol);m_protocol->setCurrentIndex(qMax(0,i));m_feedback->clear();m_feedback->setProperty("error",false);
    m_reasoning->setCurrentIndex(qMax(0,m_reasoning->findData(p.reasoningEffort)));m_thinking->setCurrentIndex(qMax(0,m_thinking->findData(p.thinkingMode)));m_thinkingBudget->setValue(p.thinkingBudget);m_maxTokens->setValue(p.maxOutputTokens);
    m_contextTokens->setValue(p.contextWindowTokens);
    m_executable->setText(p.harnessExecutable);m_harnessGroup->setVisible(p.isCodexHarness());
    for(auto* edit:{m_url,m_key})edit->setEnabled(!p.isCodexHarness());
    m_protocol->setEnabled(!p.isCodexHarness());m_thinking->setEnabled(!p.isCodexHarness());m_thinkingBudget->setEnabled(!p.isCodexHarness());m_maxTokens->setEnabled(!p.isCodexHarness());m_contextTokens->setEnabled(!p.isCodexHarness());
    updateOperationControls();
    for(QWidget* widget:QList<QWidget*>{m_url,m_key,m_protocol,m_thinking,m_thinkingBudget,m_maxTokens,m_contextTokens})m_form->setRowVisible(widget,!p.isCodexHarness());
    updateTemplateHelp();
}
void ProviderDialog::applyProvider() {
    m_feedback->setProperty("error",false);if(m_selected<0)return;
    if(m_apiBusy||m_harnessBusy){m_feedback->setProperty("error",true);m_feedback->setText("Wait for the provider operation or cancel it before saving.");return;}
    if(m_providers[m_selected].isCodexHarness()){
        const auto p=currentConfig();
        if(p.name.isEmpty()||p.model.isEmpty()||CodexAppServer::findExecutable(p.harnessExecutable).isEmpty()) {m_feedback->setProperty("error",true);m_feedback->setText("Select an installed Codex executable and connect to choose an available model.");return;}
        m_providers[m_selected]=p;m_list->item(m_selected)->setText(p.name);m_feedback->setText("Codex configured. Save to use this coding agent.");return;
    }
    const QUrl url(m_url->text().trimmed());
    if(m_name->text().trimmed().isEmpty()||!url.isValid()||url.host().isEmpty()||!url.userInfo().isEmpty()||url.hasQuery()||url.hasFragment()||m_model->text().trimmed().isEmpty()||
        (url.scheme()!="https" && !(url.scheme()=="http"&&(url.host()=="127.0.0.1"||url.host()=="localhost"||url.host()=="::1")))) {
        m_feedback->setText("Enter a name, model ID, and HTTPS endpoint (or local HTTP server) without credentials, a query, or a fragment.");m_feedback->setProperty("error",true);return;
    }
    auto& p=m_providers[m_selected];p.name=m_name->text().trimmed();p.baseUrl=url.toString();p.apiKey=m_key->text().trimmed();p.model=m_model->text().trimmed();p.protocol=m_protocol->currentData().toString();
    p.reasoningEffort=m_reasoning->currentData().toString();p.thinkingMode=m_thinking->currentData().toString();p.thinkingBudget=m_thinkingBudget->value();p.maxOutputTokens=m_maxTokens->value();
    p.contextWindowTokens=m_contextTokens->value();
    ProviderCatalog::clearChangedProvenance(p);updateTemplateHelp();
    m_list->item(m_selected)->setText(p.name);m_feedback->setText("Provider configured. Save to store it on this computer.");
}
void ProviderDialog::importKeys() {
    if(m_apiBusy||m_harnessBusy)return;
    const QPointer<ProviderDialog> self(this);const auto epoch=m_operationEpoch;
    const auto path=QFileDialog::getOpenFileName(this,"Import API keys",{},"Key files (*.md *.env *.txt);;All files (*)");if(!self||epoch!=m_operationEpoch||path.isEmpty())return;
    QString error;auto imported=ProviderStore::importKeyFile(path,&error);
    for(auto& p:imported){bool found=false;for(auto& existing:m_providers){if(existing.id==p.id){existing=p;found=true;break;}}if(!found)m_providers.append(p);}
    m_list->clear();for(const auto& p:m_providers)m_list->addItem(p.name);if(!m_providers.isEmpty())m_list->setCurrentRow(0);
    m_feedback->setText(QString("Imported %1 provider configurations. %2").arg(imported.size()).arg(error));
}
ProviderConfig ProviderDialog::currentConfig() const {
    ProviderConfig p;if(m_selected>=0&&m_selected<m_providers.size())p=m_providers[m_selected];
    p.name=m_name->text().trimmed();p.model=m_model->text().trimmed();p.reasoningEffort=m_reasoning->currentData().toString();
    if(p.isCodexHarness()){p.baseUrl.clear();p.apiKey.clear();p.protocol="codex-app-server";p.harnessExecutable=m_executable->text().trimmed();p.thinkingMode.clear();p.thinkingBudget=0;p.maxOutputTokens=0;p.contextWindowTokens=0;}
    else {p.baseUrl=m_url->text().trimmed();p.apiKey=m_key->text().trimmed();p.protocol=m_protocol->currentData().toString();p.thinkingMode=m_thinking->currentData().toString();p.thinkingBudget=m_thinkingBudget->value();p.maxOutputTokens=m_maxTokens->value();p.contextWindowTokens=m_contextTokens->value();}
    return p;
}
ProviderDialog::~ProviderDialog(){
    m_closing=true;++m_operationEpoch;m_apiBusy=false;m_harnessBusy=false;
    if(m_service){m_service->disconnect();m_service->cancel();}
    if(m_harness){m_harness->disconnect();m_harness->stop();}
}
void ProviderDialog::done(int result){m_closing=true;const QPointer<ProviderDialog> self(this);if(m_addPicker)m_addPicker->reject();if(!self)return;cancelOperations();if(self)QDialog::done(result);if(self)m_closing=false;}
void ProviderDialog::updateOperationControls(){
    const bool busy=m_apiBusy||m_harnessBusy;
    const bool harness=m_selected>=0&&m_selected<m_providers.size()&&m_providers[m_selected].isCodexHarness();
    for(auto* control:m_operationControls)control->setEnabled(!busy);
    for(QWidget* control:QList<QWidget*>{m_url,m_key,m_protocol,m_thinking,m_thinkingBudget,m_maxTokens,m_contextTokens})control->setEnabled(!busy&&!harness);
    m_connectHarness->setEnabled(!busy&&harness);m_cancelHarness->setEnabled(m_harnessBusy);
}
void ProviderDialog::cancelOperations(){
    const auto epoch=++m_operationEpoch;m_apiBusy=false;m_harnessBusy=false;m_harnessLogin=false;
    const QPointer<ProviderDialog> self(this);const auto service=m_service;m_service=nullptr;
    if(service){disconnect(service,nullptr,this,nullptr);service->cancel();if(service)service->deleteLater();}
    if(!self||m_operationEpoch!=epoch)return;
    m_harness->stop();if(self&&m_operationEpoch==epoch)updateOperationControls();
}
void ProviderDialog::finishHarnessConnection(){cancelOperations();}
void ProviderDialog::beginApiOperation(bool validation){
    if(m_closing||m_apiBusy||m_harnessBusy||m_selected<0||currentConfig().isCodexHarness())return;
    const auto provider=currentConfig();m_discoveryProvider=provider;m_apiBusy=true;
    const auto epoch=++m_operationEpoch;const QPointer<ProviderDialog> self(this);
    auto* service=new ProviderService(this);m_service=service;
    const auto valid=[self,service,epoch,provider]{return self&&self->m_service==service&&self->m_operationEpoch==epoch&&self->m_apiBusy&&self->m_selected>=0&&self->m_selected<self->m_providers.size()&&self->m_providers[self->m_selected].id==provider.id&&!self->m_providers[self->m_selected].isCodexHarness();};
    connect(service,&ProviderService::discoveryFailed,this,[this,valid](const QString& error){if(!valid())return;m_feedback->setText(error);cancelOperations();});
    connect(service,&ProviderService::validationFinished,this,[this,valid](bool ok,const QString& detail){if(!valid())return;m_feedback->setText((ok?"Connection verified: ":"Connection failed: ")+detail);cancelOperations();});
    connect(service,&ProviderService::modelsReady,this,[this,self,valid,provider](const QJsonArray& models){
        if(!valid())return;
        m_catalogs[provider.id]=cleanModelCatalog(provider,models);
        QStringList names,ids;for(const auto& value:models){const auto model=value.toObject();const auto id=model["id"].toString();if(id.isEmpty())continue;ids.append(id);names.append(model["name"].toString(id)+"  ["+id+"]");}
        if(ids.isEmpty()){m_feedback->setText("The provider returned no selectable models.");cancelOperations();return;}
        bool ok=false;const auto selected=QInputDialog::getItem(this,"Available models","Select a default model",names,qMax(0,ids.indexOf(m_model->text())),false,&ok);
        if(!self||!valid())return;
        if(ok)m_model->setText(ids.value(names.indexOf(selected)));
        if(!valid())return;
        m_feedback->setText(QString("Discovered %1 models. Save to include them in the model picker.").arg(ids.size()));cancelOperations();
    });
    updateOperationControls();if(!valid())return;
    if(validation)service->test(provider);else service->discover(provider);
}
void ProviderDialog::beginHarnessConnection(bool login){
    if(m_closing||m_apiBusy||m_harnessBusy||!currentConfig().isCodexHarness())return;
    m_discoveryProvider=currentConfig();const auto provider=m_discoveryProvider;m_harnessLogin=login;m_harnessBusy=true;
    const auto epoch=++m_operationEpoch;const QPointer<ProviderDialog> self(this);
    const auto valid=[self,epoch,provider]{return self&&self->m_operationEpoch==epoch&&self->m_harnessBusy&&self->m_selected>=0&&self->m_selected<self->m_providers.size()&&self->m_providers[self->m_selected].id==provider.id&&self->m_providers[self->m_selected].isCodexHarness();};
    disconnect(m_harness,nullptr,this,nullptr);
    connect(m_harness,&CodexAppServer::ready,this,[this,valid]{if(valid())m_harness->readAccount();});
    connect(m_harness,&CodexAppServer::accountChanged,this,[this,valid](bool connected,const QString& plan){
        if(!valid())return;
        if(connected){m_feedback->setText(QString("Connected to your ChatGPT subscription%1. Fetching models…").arg(plan.isEmpty()?QString():" ("+plan+")"));m_harness->discoverModels();}
        else if(m_harnessLogin){m_harnessLogin=false;m_harness->startLogin();}
        else {m_feedback->setText("Codex is not signed in to ChatGPT. Use Connect ChatGPT subscription.");finishHarnessConnection();}
    });
    connect(m_harness,&CodexAppServer::loginUrlReady,this,[this,self,valid](const QUrl& url){
        if(!valid())return;
        const bool opened=QDesktopServices::openUrl(url);if(!self||!valid())return;
        if(!opened){m_harness->cancelLogin();if(!self||!valid())return;m_feedback->setText("The sign-in browser could not be opened. Run codex login, then fetch models here.");finishHarnessConnection();}
        else m_feedback->setText("Complete sign-in in your browser. ADE is waiting for Codex to confirm the account.");
    });
    connect(m_harness,&CodexAppServer::loginFinished,this,[this,valid](bool success,const QString& detail){if(!valid())return;m_feedback->setText(detail);if(!success)finishHarnessConnection();});
    connect(m_harness,&CodexAppServer::failed,this,[this,valid](const QString& error){if(!valid())return;m_feedback->setText(error);finishHarnessConnection();});
    connect(m_harness,&CodexAppServer::modelsReady,this,[this,valid,provider](const QJsonArray& models){
        if(!valid())return;
        m_catalogs[provider.id]=cleanModelCatalog(provider,models);
        QString selected;bool existing=false;for(const auto& value:models){const auto model=value.toObject();if(selected.isEmpty()||model["isDefault"].toBool())selected=model["id"].toString();if(model["id"].toString()==m_model->text())existing=true;}
        if(!existing&&!selected.isEmpty())m_model->setText(selected);
        if(!valid())return;
        m_feedback->setText(models.isEmpty()?"Codex returned no selectable models.":QString("ChatGPT subscription connected. Discovered %1 models; Save to use them in the model picker.").arg(models.size()));finishHarnessConnection();
    });
    updateOperationControls();if(!valid())return;m_feedback->setText("Starting installed Codex…");m_harness->start(provider.harnessExecutable);
}
}
