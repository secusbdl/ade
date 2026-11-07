#include "CustomModelDialog.h"
#include "core/CustomModelOptions.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QToolButton>
#include <QStandardItemModel>
#include <QVBoxLayout>
namespace ade {
namespace {
QString jsonText(const QJsonObject& value){return value.isEmpty()?QString{}:QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Indented));}
std::optional<QJsonObject> parseObject(const QPlainTextEdit* field,int maximum,const QString& label,QString* error){
 const auto text=field->toPlainText().trimmed();if(text.isEmpty())return QJsonObject{};
 if(text.size()>maximum){*error=label+" exceeds its supported size.";return std::nullopt;}
 const auto document=QJsonDocument::fromJson(text.toUtf8());if(!document.isObject()){*error=label+" must contain a JSON object.";return std::nullopt;}return document.object();
}
}
CustomModelDialog::CustomModelDialog(QList<ProviderConfig> providers,std::optional<CustomModelConfig> existing,
 QString initialProviderId,Committer committer,QWidget* parent):QDialog(parent),m_providers(std::move(providers)),m_commit(std::move(committer)){
 if(parent){setPalette(parent->palette());parent->installEventFilter(this);}
 setObjectName("customModelDialog");setWindowTitle(existing?"Edit custom model":"Add custom model");resize(620,720);setMinimumSize(320,360);
 const auto model=existing.value_or(CustomModelConfig{});if(existing)initialProviderId=model.providerInstanceId;
 auto* outer=new QVBoxLayout(this);auto* scroll=new QScrollArea;scroll->setObjectName("customModelScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto* body=new QWidget;body->setObjectName("customModelBody");m_formWidget=body;auto* layout=new QVBoxLayout(body);layout->setContentsMargins(4,4,4,4);layout->setSpacing(12);
 auto* form=new QFormLayout;form->setRowWrapPolicy(QFormLayout::WrapLongRows);form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
 m_modelId=new QLineEdit(model.modelId);m_modelId->setObjectName("customModelId");m_modelId->setMaxLength(512);form->addRow("Model ID",m_modelId);
 m_displayName=new QLineEdit(model.displayName);m_displayName->setObjectName("customModelDisplayName");m_displayName->setMaxLength(512);form->addRow("Display name",m_displayName);
 m_description=new QPlainTextEdit(model.description);m_description->setObjectName("customModelDescription");m_description->setMaximumHeight(72);form->addRow("Description",m_description);
 m_provider=new QComboBox;m_provider->setObjectName("customModelProvider");m_provider->setMinimumWidth(0);m_provider->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_provider->setMinimumContentsLength(8);int selected=-1;
 for(const auto& provider:m_providers)if(provider.kind=="api"){QString error;if(!CustomModelOptions::validate(provider.protocol,{}, {},&error))continue;m_provider->addItem(provider.name,provider.id);if(provider.id==initialProviderId)selected=m_provider->count()-1;}
 if(selected<0&&!initialProviderId.isEmpty()){m_provider->addItem("Saved provider unavailable",initialProviderId);selected=m_provider->count()-1;if(auto* items=qobject_cast<QStandardItemModel*>(m_provider->model()))items->item(selected)->setEnabled(false);}
 m_provider->setCurrentIndex(selected>=0?selected:(m_provider->count()?0:-1));form->addRow("Provider instance",m_provider);
 m_context=new QSpinBox;m_context->setObjectName("customModelContext");m_context->setRange(1,100000000);m_context->setValue(model.contextWindowSize);form->addRow("Context window",m_context);
 m_thinking=new QCheckBox("Supports thinking");m_thinking->setObjectName("customModelThinking");m_thinking->setChecked(model.thinkingEnabled);m_thinking->setToolTip("Describes the model. Configure the actual request through provider options or the model selection override.");form->addRow(m_thinking);layout->addLayout(form);
 auto* capabilities=new QGroupBox("Capabilities");auto* grid=new QGridLayout(capabilities);grid->addWidget(new QLabel("Input"),0,1);grid->addWidget(new QLabel("Output"),0,2);int row=1;
 for(const auto& modality:{QString("text"),QString("image"),QString("audio"),QString("video"),QString("file")}){
  grid->addWidget(new QLabel(modality),row,0);int col=1;
  for(const auto& direction:{QString("inputModalities"),QString("outputModalities")}){
   const auto key=direction+"."+modality;auto* flag=new QCheckBox;flag->setObjectName("customModelCapability_"+key);flag->setChecked(model.capabilities.value(direction).toObject().value(modality).toBool());
   const bool supported=modality=="text"||(direction=="inputModalities"&&modality=="image");
   if(!supported){flag->setToolTip("This native transport does not support this modality yet. Clear an imported flag before saving.");flag->setEnabled(flag->isChecked());connect(flag,&QCheckBox::toggled,flag,[flag](bool checked){if(!checked)flag->setEnabled(false);});}
   else if(modality=="text"){flag->setEnabled(!flag->isChecked());connect(flag,&QCheckBox::toggled,flag,[flag](bool checked){if(checked)flag->setEnabled(false);});}
   m_capabilities.insert(key,flag);grid->addWidget(flag,row,col++);
  }++row;
 }
 m_tools=new QCheckBox("Tool calling");m_tools->setObjectName("customModelToolCalling");m_tools->setChecked(model.capabilities.value("toolCalling").toBool(true));grid->addWidget(m_tools,row,0,1,3);layout->addWidget(capabilities);
 auto* advancedToggle=new QToolButton;advancedToggle->setObjectName("customModelAdvancedToggle");advancedToggle->setText("Advanced options");advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advancedToggle->setCheckable(true);advancedToggle->setArrowType(Qt::RightArrow);layout->addWidget(advancedToggle);
 auto* advanced=new QGroupBox;advanced->setObjectName("customModelAdvanced");auto* advancedLayout=new QVBoxLayout(advanced);auto* hint=new QLabel("Provider options use this API's supported option names. Header values are stored encrypted and sent only to this model's configured endpoint.");hint->setWordWrap(true);advancedLayout->addWidget(hint);
 advancedLayout->addWidget(new QLabel("Provider options (JSON)"));m_options=new QPlainTextEdit(jsonText(model.providerOptions));m_options->setObjectName("customModelOptions");m_options->setLineWrapMode(QPlainTextEdit::NoWrap);m_options->setMaximumHeight(110);advancedLayout->addWidget(m_options);
 advancedLayout->addWidget(new QLabel("Headers (JSON)"));m_headers=new QPlainTextEdit(jsonText(model.headers));m_headers->setObjectName("customModelHeaders");m_headers->setLineWrapMode(QPlainTextEdit::NoWrap);m_headers->setMaximumHeight(110);advancedLayout->addWidget(m_headers);if(QFontDatabase::families().contains("Roboto Mono")){QFont code("Roboto Mono");code.setPixelSize(12);m_options->setFont(code);m_headers->setFont(code);}layout->addWidget(advanced);advanced->hide();
 connect(advancedToggle,&QToolButton::toggled,this,[button=QPointer<QToolButton>(advancedToggle),contents=QPointer<QWidget>(advanced)](bool checked){if(!button||!contents)return;button->setArrowType(checked?Qt::DownArrow:Qt::RightArrow);if(button&&contents&&button->isChecked()==checked)contents->setVisible(checked);});
 layout->addStretch();scroll->setWidget(body);outer->addWidget(scroll,1);
 m_status=new QLabel;m_status->setObjectName("customModelStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);outer->addWidget(m_status);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);m_buttons=buttons;m_save=buttons->button(QDialogButtonBox::Save);m_save->setObjectName("saveCustomModel");outer->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,this,&CustomModelDialog::save);connect(buttons,&QDialogButtonBox::rejected,this,&CustomModelDialog::reject);
 if(m_provider->count()==0){m_status->setText("Configure a supported HTTP provider before adding a custom model.");m_save->setEnabled(false);}
 updateColors();
}
std::optional<CustomModelConfig> CustomModelDialog::candidate(QString* error)const{
 error->clear();CustomModelConfig model;model.modelId=m_modelId->text().trimmed();model.displayName=m_displayName->text().trimmed();model.description=m_description->toPlainText().trimmed();model.providerInstanceId=m_provider->currentData().toString();model.contextWindowSize=m_context->value();model.thinkingEnabled=m_thinking->isChecked();
 for(const auto& direction:{QString("inputModalities"),QString("outputModalities")}){QJsonObject flags;for(const auto& modality:{QString("text"),QString("image"),QString("audio"),QString("video"),QString("file")})flags[modality]=m_capabilities.value(direction+"."+modality)->isChecked();model.capabilities[direction]=flags;}
 model.capabilities["toolCalling"]=m_tools->isChecked();
 const auto options=parseObject(m_options,65536,"Provider options",error);if(!options)return std::nullopt;const auto headers=parseObject(m_headers,32768,"Headers",error);if(!headers)return std::nullopt;model.providerOptions=*options;model.headers=*headers;
 if(!model.validate(model.providerInstanceId,error))return std::nullopt;
 const ProviderConfig* provider=nullptr;for(const auto& item:m_providers)if(item.id==model.providerInstanceId&&item.kind=="api"){provider=&item;break;}
 if(!provider){*error="The selected HTTP provider is unavailable.";return std::nullopt;}
 if(!CustomModelOptions::validate(provider->protocol,model.providerOptions,model.headers,error))return std::nullopt;
 for(const auto& direction:{QString("inputModalities"),QString("outputModalities")}){const auto flags=model.capabilities.value(direction).toObject();if(!flags.value("text").toBool()){*error="This native transport requires text input and output.";return std::nullopt;}for(const auto& modality:{QString("image"),QString("audio"),QString("video"),QString("file")})if(flags.value(modality).toBool()&&!(direction=="inputModalities"&&modality=="image")){*error="The selected modality is not supported by this native transport.";return std::nullopt;}}
 return model;
}
void CustomModelDialog::save(){
 if(m_saving||m_closed)return;
 QString error;const auto model=candidate(&error);if(!model){m_status->setText(error);return;}
 const QPointer<CustomModelDialog> self(this);m_saving=true;m_status->clear();if(!self)return;m_formWidget->setEnabled(false);if(!self)return;m_buttons->setEnabled(false);if(!self)return;
 const auto commit=m_commit;const bool saved=commit&&commit(*model,&error);if(!self)return;m_saving=false;
 if(saved){accept();return;}m_formWidget->setEnabled(true);if(!self)return;m_buttons->setEnabled(true);if(self)m_status->setText(error.isEmpty()?"The custom model could not be saved.":error);
}
void CustomModelDialog::done(int result){if(m_saving)return;m_closed=true;QDialog::done(result);}
void CustomModelDialog::updateColors(){
 if(m_coloring)return;
 m_coloring=true;const QPointer<CustomModelDialog> self(this);
 const bool dark=palette().color(QPalette::Base).lightness()<128;
 const auto base=dark?QString("#21242b"):QString("#fdfdfd"),text=dark?QString("#e8e9ec"):QString("#292a2d"),border=dark?QString("#3b4049"):QString("#e4e6e8"),button=dark?QString("#2c3039"):QString("#f2f2f2");
 setStyleSheet(QString("QDialog#customModelDialog,QWidget#customModelBody,QScrollArea#customModelScroll {background:%1;} QDialog#customModelDialog QWidget {color:%2;} QDialog#customModelDialog QLineEdit,QDialog#customModelDialog QPlainTextEdit,QDialog#customModelDialog QComboBox,QDialog#customModelDialog QSpinBox {background:%1;border:1px solid %3;border-radius:4px;padding:4px;} QDialog#customModelDialog QGroupBox {border:1px solid %3;border-radius:4px;margin-top:10px;padding-top:8px;} QDialog#customModelDialog QGroupBox::title {subcontrol-origin:margin;left:8px;} QDialog#customModelDialog QPushButton,QDialog#customModelDialog QToolButton {background:%4;border:1px solid %3;border-radius:4px;padding:5px 12px;} QDialog#customModelDialog QWidget:disabled {color:#80858d;} QDialog#customModelDialog QComboBox QAbstractItemView {background:%1;}").arg(base,text,border,button));
 if(self)m_coloring=false;
}
void CustomModelDialog::changeEvent(QEvent* event){const QPointer<CustomModelDialog> self(this);QDialog::changeEvent(event);if(self&&event->type()==QEvent::PaletteChange&&!m_coloring)updateColors();}
bool CustomModelDialog::eventFilter(QObject* watched,QEvent* event){
 const QPointer<CustomModelDialog> self(this);const QPointer<QObject> receiver(watched);
 if(watched==parentWidget()&&event->type()==QEvent::PaletteChange){setPalette(parentWidget()->palette());if(!self)return !receiver;updateColors();}
 return self?QDialog::eventFilter(watched,event):!receiver;
}
}
