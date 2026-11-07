#include "SideChatPane.h"
#include <QStandardItemModel>
#include "ContentControlsScrollArea.h"
#include "ui/Composer.h"
#include "ui/ConversationView.h"
#include "ui/QuestionPanel.h"
#include "ui/MessageQueuePanel.h"
#include "ui/AttachmentStrip.h"
#include "ui/Icons.h"
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
namespace ade {
SideChatPane::SideChatPane(QString chatId,QWidget* parent):QWidget(parent),m_chatId(std::move(chatId)){
    setObjectName("sideChatPane");setProperty("sideChatAgentId",m_chatId);setMinimumWidth(0);
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(8,8,8,8);layout->setSpacing(6);
    auto* header=new QHBoxLayout;m_title=new QLabel("Side chat");m_title->setTextFormat(Qt::PlainText);m_title->setMinimumWidth(0);m_title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);header->addWidget(m_title,1);
    auto* changes=new QToolButton;changes->setObjectName("sideChatChangesButton");changes->setIcon(icon("document"));changes->setToolTip("File changes and undo");changes->setAccessibleName("File changes and undo");changes->setFixedSize(26,26);header->addWidget(changes);connect(changes,&QToolButton::clicked,this,[this]{emit changesRequested(m_chatId);});
    m_promote=new QToolButton;m_promote->setObjectName("promoteSideChatButton");m_promote->setText("Move to sidebar");m_promote->setToolTip("Promote this side chat to an independent chat");header->addWidget(m_promote);layout->addLayout(header);connect(m_promote,&QToolButton::clicked,this,[this]{emit promoteRequested(m_chatId);});
    m_transcript=new ConversationView(this);m_transcript->setObjectName("sideChatTranscript");layout->addWidget(m_transcript,1);
    m_controls=new ContentControlsScrollArea;m_controls->setObjectName("sideChatControls");m_controls->setWidgetResizable(true);m_controls->setFrameShape(QFrame::NoFrame);m_controls->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);auto* body=new QWidget;auto* controlsLayout=new QVBoxLayout(body);controlsLayout->setContentsMargins(0,0,0,0);controlsLayout->setSpacing(4);
    m_back=new QToolButton;m_back->setObjectName("sideChatBackToQuestions");m_back->setText("Back to questions");controlsLayout->addWidget(m_back);connect(m_back,&QToolButton::clicked,this,[this]{m_messageToken.clear();applyQuestionVisibility();});
    m_questions=new QuestionPanel;controlsLayout->addWidget(m_questions);m_queue=new MessageQueuePanel;controlsLayout->addWidget(m_queue);m_controls->setWidget(body);layout->addWidget(m_controls);
    connect(m_questions,&QuestionPanel::messageRequested,this,[this](const QString& chat,const QString& token){if(chat!=m_chatId||token!=m_questionToken)return;m_messageToken=token;const QPointer<SideChatPane> self(this);applyQuestionVisibility();if(self)m_composer->setFocus();});
    auto* center=new QHBoxLayout;center->setContentsMargins(0,0,0,0);center->addStretch();m_footer=new QWidget;m_footer->setObjectName("composerBox");m_footer->setMaximumWidth(768);m_footer->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);auto* footer=new QVBoxLayout(m_footer);footer->setContentsMargins(8,8,8,8);footer->setSpacing(4);m_attachments=new AttachmentStrip;footer->addWidget(m_attachments);m_composer=new Composer;m_composer->setObjectName("sideChatComposer");m_composer->setCompletionContext(m_chatId);footer->addWidget(m_composer);
    auto* bar=new QHBoxLayout;bar->setSpacing(4);auto* attach=new QToolButton;attach->setObjectName("sideChatAttachButton");attach->setIcon(icon("attach"));attach->setToolTip("Attach files");bar->addWidget(attach);m_models=new QComboBox;m_models->setObjectName("sideChatModel");m_models->setMinimumWidth(0);m_models->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_models->setMinimumContentsLength(1);bar->addWidget(m_models,1);
    m_permissions=new QComboBox;m_permissions->setObjectName("sideChatPermission");m_permissions->addItem("Ask",false);m_permissions->addItem("Always allow",true);m_permissions->setMaximumWidth(105);bar->addWidget(m_permissions);
    m_stop=new QToolButton;m_stop->setObjectName("sideChatStopButton");m_stop->setIcon(icon("stop"));m_stop->setToolTip("Stop this side chat");bar->addWidget(m_stop);m_send=new QToolButton;m_send->setObjectName("sideChatSendButton");m_send->setIcon(icon("send"));m_send->setToolTip("Send message");bar->addWidget(m_send);footer->addLayout(bar);center->addWidget(m_footer,1);center->addStretch();layout->addLayout(center);
    m_error=new QLabel;m_error->setObjectName("sideChatStatus");m_error->setTextFormat(Qt::PlainText);m_error->setWordWrap(true);m_error->hide();layout->addWidget(m_error);
    connect(m_composer,&Composer::textChanged,this,[this]{updateActions();emit draftChanged(m_chatId,m_composer->toPlainText());});
    connect(m_composer,&Composer::submitted,this,[this]{if(m_send->isEnabled())emit submitted(m_chatId);});connect(m_send,&QToolButton::clicked,this,[this]{emit submitted(m_chatId);});
    connect(m_composer,&Composer::stopRequested,this,[this]{emit stopRequested(m_chatId);});connect(m_composer,&Composer::stopShortcutRequested,m_stop,&QToolButton::click);connect(m_stop,&QToolButton::clicked,this,[this]{emit stopRequested(m_chatId);});
    connect(m_composer,&Composer::filesAttached,this,[this](const QStringList& paths){emit filesAttached(m_chatId,paths);});connect(m_composer,&Composer::imageAttached,this,[this](const QImage& image){emit imageAttached(m_chatId,image);});
    connect(attach,&QToolButton::clicked,this,[this]{const QPointer<SideChatPane> self(this);const auto paths=QFileDialog::getOpenFileNames(this,"Attach files");if(self&&!paths.isEmpty())emit filesAttached(m_chatId,paths);});
    connect(m_attachments,&AttachmentStrip::removeRequested,this,[this](int index){emit removeAttachmentRequested(m_chatId,index);});
    connect(m_models,&QComboBox::currentIndexChanged,this,[this]{emit modelChanged(m_chatId,m_models->currentData().toJsonObject());});connect(m_permissions,&QComboBox::currentIndexChanged,this,[this]{emit permissionChanged(m_chatId,m_permissions->currentData().toBool());});updateActions();fit();
}
void SideChatPane::setState(const Conversation& chat,const QJsonArray& rows,bool busy,const QJsonArray& models,const QJsonObject& question,const QJsonArray& queue){
    if(chat.id!=m_chatId)return;
    const QPointer<SideChatPane> self(this);m_busy=busy;m_available=!chat.archived;m_hasAttachments=!chat.draftAttachments.isEmpty();m_title->setText(chat.title);m_title->setToolTip(chat.title);m_transcript->setConversation(m_chatId,rows,busy);if(!self)return;
    if(m_composer->toPlainText()!=chat.draft){QSignalBlocker blocker(m_composer);m_composer->setPlainText(chat.draft);m_composer->moveCursor(QTextCursor::End);}m_attachments->setAttachments(chat.draftAttachments);if(!self)return;
    {QSignalBlocker blocker(m_models);m_models->clear();int chosen=-1;for(const auto& value:models){const auto item=value.toObject();m_models->addItem(item["label"].toString(),item);if(!item.value("selectable").toBool(true))if(auto* model=qobject_cast<QStandardItemModel*>(m_models->model()))model->item(m_models->count()-1)->setEnabled(false);if(item["providerInstanceId"]==chat.providerId&&item["modelId"]==chat.model&&item["presetId"].toString()==chat.presetId)chosen=m_models->count()-1;}if(chosen<0&&!chat.model.isEmpty()){m_models->addItem(chat.model+" (unavailable)",QJsonObject{{"providerInstanceId",chat.providerId},{"modelId",chat.model},{"presetId",chat.presetId}});chosen=m_models->count()-1;}m_models->setCurrentIndex(chosen);}
    {QSignalBlocker blocker(m_permissions);m_permissions->setCurrentIndex(chat.alwaysAllow?1:0);}m_models->setEnabled(!busy&&m_available);m_permissions->setEnabled(!busy&&m_available);m_promote->setEnabled(!busy&&m_available);
    m_questionToken=question["id"].toString();if(m_questionToken!=m_messageToken)m_messageToken.clear();m_questions->setQuestion(m_chatId,question);if(!self)return;m_queue->setQueue(m_chatId,queue,busy);if(!self)return;m_controls->setProperty("hasQueue",!queue.isEmpty());applyQuestionVisibility();if(self)updateActions();
}
void SideChatPane::applyQuestionVisibility(){const bool answering=!m_questionToken.isEmpty(),writing=answering&&m_messageToken==m_questionToken;m_questions->setVisible(answering&&!writing);m_back->setVisible(writing);m_controls->setVisible(answering||m_controls->property("hasQueue").toBool());m_footer->setVisible(!answering||writing);}
void SideChatPane::updateActions(){m_send->setEnabled(m_available&&(!m_composer->toPlainText().trimmed().isEmpty()||m_hasAttachments));m_send->setToolTip(m_busy?"Add message to queue":"Send message");m_stop->setVisible(m_busy);m_stop->setEnabled(m_busy);}
void SideChatPane::setError(const QString& message){m_error->setText(message);m_error->setVisible(!message.isEmpty());}
void SideChatPane::setUiScale(double scale){
    m_composer->setUiScale(scale);auto font=m_transcript->font();font.setPixelSize(qRound(14*scale));m_transcript->setFont(font);fit();
    // Theme refresh calls this after updating the application palette. Scroll
    // bodies paint the inherited Window role even when their surrounding pane
    // is styled, so keep the whole chat on the same Base content surface.
    auto contentPalette=palette();const auto applicationPalette=QApplication::palette();
    for(const auto group:{QPalette::Active,QPalette::Inactive,QPalette::Disabled})
        contentPalette.setBrush(group,QPalette::Window,applicationPalette.brush(group,QPalette::Base));
    // QScrollArea enables auto-fill on setWidget(); nested question bodies can
    // retain an application Window fill instead of the surrounding chat color.
    // As in ConversationView, let these container layers show their parent.
    for(auto* scroll:findChildren<QScrollArea*>()){
        scroll->setAutoFillBackground(false);scroll->viewport()->setAutoFillBackground(false);
        if(scroll->widget())scroll->widget()->setAutoFillBackground(false);
    }
    setAutoFillBackground(true);setPalette(contentPalette);
}
void SideChatPane::fit(){if(!m_composer)return;m_composer->setAvailableHeightLimit(qMax(28,height()/3));m_controls->setMaximumHeight(qBound(48,height()-130,440));}
void SideChatPane::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);fit();}
}
