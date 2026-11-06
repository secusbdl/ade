#include "ProviderDialog.h"
#include "ModelSelection.h"
#include "core/ProviderStore.h"
#include "core/ProviderService.h"
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
    auto* description=new QLabel("Configure API keys and custom endpoints for your agents."); outer->addWidget(description);
    auto* columns=new QHBoxLayout; outer->addLayout(columns,1);
    auto* left=new QVBoxLayout; columns->addLayout(left);m_list=new QListWidget; m_list->setMinimumWidth(210);left->addWidget(m_list);
    auto* add=new QPushButton("+ Add Provider");left->addWidget(add);
    auto* import=new QPushButton("Import API key file…"); left->addWidget(import);
    auto* right=new QVBoxLayout; columns->addLayout(right,1); auto* form=new QFormLayout;right->addLayout(form);
    m_name=new QLineEdit;m_url=new QLineEdit;m_key=new QLineEdit;m_model=new QLineEdit;m_protocol=new QComboBox;
    m_name->setObjectName("providerName");m_url->setObjectName("providerBaseUrl");m_key->setObjectName("providerApiKey");m_model->setObjectName("providerModel");
    m_key->setEchoMode(QLineEdit::Password);m_key->setPlaceholderText("API key (encrypted when saved)");
    m_url->setPlaceholderText("https://api.example.com/v1");m_model->setPlaceholderText("Model ID from your provider");
    m_protocol->addItem("OpenAI compatible","openai");
    m_protocol->addItem("OpenAI Responses","openai-responses");
    m_protocol->addItem("Anthropic Messages","anthropic");m_protocol->addItem("Google Gemini","google");
    m_reasoning=new QComboBox;m_reasoning->addItem("Provider default",QString());for(const auto& effort:QStringList{"low","medium","high","xhigh","max","minimal","none"})m_reasoning->addItem(effort,effort);
    m_thinking=new QComboBox;m_thinking->addItem("Provider default",QString());m_thinking->addItem("Enabled","enabled");m_thinking->addItem("Disabled","disabled");
    m_thinkingBudget=new QSpinBox;m_thinkingBudget->setRange(0,200000);m_thinkingBudget->setSpecialValueText("Provider default");
    m_maxTokens=new QSpinBox;m_maxTokens->setRange(0,200000);m_maxTokens->setSpecialValueText("Provider default");
    m_contextTokens=new QSpinBox;m_contextTokens->setObjectName("providerContextWindow");m_contextTokens->setRange(0,100000000);m_contextTokens->setSpecialValueText("Unknown / use discovered model limit");
    m_contextTokens->setToolTip("Context tokens for this default model. Automatic compression uses half this limit, capped at 100,000 tokens. Unknown models keep manual /compact.");
    form->addRow("Name",m_name);form->addRow("Base URL",m_url);form->addRow("API key",m_key);form->addRow("Model",m_model);form->addRow("Protocol",m_protocol);
    form->addRow("Reasoning effort",m_reasoning);form->addRow("Thinking",m_thinking);form->addRow("Thinking budget",m_thinkingBudget);form->addRow("Output token limit",m_maxTokens);
    form->addRow("Model context limit",m_contextTokens);
    connect(m_model,&QLineEdit::textChanged,this,[this]{m_contextTokens->setValue(0);});
    connect(m_url,&QLineEdit::textChanged,this,[this]{m_contextTokens->setValue(0);});
    connect(m_protocol,&QComboBox::currentIndexChanged,this,[this]{m_contextTokens->setValue(0);});
    m_feedback=new QLabel;m_feedback->setWordWrap(true);right->addWidget(m_feedback);right->addStretch();
    auto* apply=new QPushButton("Apply provider");right->addWidget(apply,0,Qt::AlignRight);
    auto* probeRow=new QHBoxLayout;auto* discover=new QPushButton("Fetch models");auto* validate=new QPushButton("Test connection");probeRow->addWidget(discover);probeRow->addWidget(validate);right->addLayout(probeRow);
    auto* service=new ProviderService(this);
    auto liveConfig=[this]{ProviderConfig p;if(m_selected>=0)p=m_providers[m_selected];p.name=m_name->text().trimmed();p.baseUrl=m_url->text().trimmed();p.apiKey=m_key->text().trimmed();p.model=m_model->text().trimmed();p.protocol=m_protocol->currentData().toString();p.reasoningEffort=m_reasoning->currentData().toString();p.thinkingMode=m_thinking->currentData().toString();p.thinkingBudget=m_thinkingBudget->value();p.maxOutputTokens=m_maxTokens->value();p.contextWindowTokens=m_contextTokens->value();return p;};
    connect(discover,&QPushButton::clicked,this,[this,service,liveConfig]{m_discoveryProvider=liveConfig();service->discover(m_discoveryProvider);});
    connect(validate,&QPushButton::clicked,this,[service,liveConfig]{service->test(liveConfig());});
    connect(service,&ProviderService::busyChanged,this,[this,discover,validate,add,import,apply](bool busy){discover->setEnabled(!busy);validate->setEnabled(!busy);m_list->setEnabled(!busy);add->setEnabled(!busy);import->setEnabled(!busy);apply->setEnabled(!busy);for(auto* edit:{m_name,m_url,m_key,m_model})edit->setEnabled(!busy);m_protocol->setEnabled(!busy);});
    connect(service,&ProviderService::discoveryFailed,this,[this](const QString& error){m_feedback->setText(error);});
    connect(service,&ProviderService::validationFinished,this,[this](bool ok,const QString& detail){m_feedback->setText((ok?"Connection verified: ":"Connection failed: ")+detail);});
    connect(service,&ProviderService::modelsReady,this,[this](const QJsonArray& models){m_catalogs[m_discoveryProvider.id]=cleanModelCatalog(m_discoveryProvider,models);QStringList names,ids;for(const auto& value:models){const auto model=value.toObject();const auto id=model["id"].toString();if(id.isEmpty())continue;ids.append(id);names.append(model["name"].toString(id)+"  ["+id+"]");}if(ids.isEmpty()){m_feedback->setText("The provider returned no selectable models.");return;}bool ok=false;auto selected=QInputDialog::getItem(this,"Available models","Select a default model",names,qMax(0,ids.indexOf(m_model->text())),false,&ok);if(ok)m_model->setText(ids.value(names.indexOf(selected)));m_feedback->setText(QString("Discovered %1 models. Save to include them in the model picker.").arg(ids.size()));});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);outer->addWidget(buttons);
    connect(service,&ProviderService::busyChanged,this,[buttons](bool busy){buttons->button(QDialogButtonBox::Save)->setEnabled(!busy);});
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{applyProvider();if(m_feedback->property("error").toBool())return;accept();});
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(apply,&QPushButton::clicked,this,&ProviderDialog::applyProvider);
    connect(import,&QPushButton::clicked,this,&ProviderDialog::importKeys);
    connect(add,&QPushButton::clicked,this,[this]{ProviderConfig p;p.id=QUuid::createUuid().toString(QUuid::WithoutBraces);p.name="Custom provider";p.protocol="openai";m_providers.append(p);m_list->addItem(p.name);m_list->setCurrentRow(m_providers.size()-1);});
    connect(m_list,&QListWidget::currentRowChanged,this,&ProviderDialog::selectProvider);
    for(const auto& p:m_providers)m_list->addItem(p.name);
    if(!m_providers.isEmpty())m_list->setCurrentRow(0);
}
void ProviderDialog::selectProvider(int row) {
    m_selected=row;if(row<0||row>=m_providers.size())return;
    const auto& p=m_providers[row];m_name->setText(p.name);m_url->setText(p.baseUrl);m_key->setText(p.apiKey);m_model->setText(p.model);
    auto i=m_protocol->findData(p.protocol);m_protocol->setCurrentIndex(qMax(0,i));m_feedback->clear();m_feedback->setProperty("error",false);
    m_reasoning->setCurrentIndex(qMax(0,m_reasoning->findData(p.reasoningEffort)));m_thinking->setCurrentIndex(qMax(0,m_thinking->findData(p.thinkingMode)));m_thinkingBudget->setValue(p.thinkingBudget);m_maxTokens->setValue(p.maxOutputTokens);
    m_contextTokens->setValue(p.contextWindowTokens);
}
void ProviderDialog::applyProvider() {
    m_feedback->setProperty("error",false);if(m_selected<0)return;
    const QUrl url(m_url->text().trimmed());
    if(m_name->text().trimmed().isEmpty()||!url.isValid()||url.host().isEmpty()||m_model->text().trimmed().isEmpty()||
        (url.scheme()!="https" && !(url.scheme()=="http"&&(url.host()=="127.0.0.1"||url.host()=="localhost"||url.host()=="::1")))) {
        m_feedback->setText("Enter a name, HTTPS endpoint (or local HTTP server), and a model ID.");m_feedback->setProperty("error",true);return;
    }
    auto& p=m_providers[m_selected];p.name=m_name->text().trimmed();p.baseUrl=url.toString();p.apiKey=m_key->text().trimmed();p.model=m_model->text().trimmed();p.protocol=m_protocol->currentData().toString();
    p.reasoningEffort=m_reasoning->currentData().toString();p.thinkingMode=m_thinking->currentData().toString();p.thinkingBudget=m_thinkingBudget->value();p.maxOutputTokens=m_maxTokens->value();
    p.contextWindowTokens=m_contextTokens->value();
    m_list->item(m_selected)->setText(p.name);m_feedback->setText("Provider configured. Save to store it on this computer.");
}
void ProviderDialog::importKeys() {
    const auto path=QFileDialog::getOpenFileName(this,"Import API keys",{},"Key files (*.md *.env *.txt);;All files (*)");if(path.isEmpty())return;
    QString error;auto imported=ProviderStore::importKeyFile(path,&error);
    for(auto& p:imported){bool found=false;for(auto& existing:m_providers){if(existing.id==p.id){existing=p;found=true;break;}}if(!found)m_providers.append(p);}
    m_list->clear();for(const auto& p:m_providers)m_list->addItem(p.name);if(!m_providers.isEmpty())m_list->setCurrentRow(0);
    m_feedback->setText(QString("Imported %1 provider configurations. %2").arg(imported.size()).arg(error));
}
}
