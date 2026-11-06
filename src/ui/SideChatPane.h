#pragma once
#include "ui/SessionStore.h"
#include <QWidget>
#include <QPointer>
#include <QImage>
class QComboBox;class QToolButton;class QLabel;class QScrollArea;
namespace ade {
class Composer;class ConversationView;class QuestionPanel;class MessageQueuePanel;class AttachmentStrip;
class SideChatPane final:public QWidget {
    Q_OBJECT
public:
    explicit SideChatPane(QString chatId,QWidget* parent=nullptr);
    QString chatId()const{return m_chatId;}
    Composer* composer()const{return m_composer;}
    ConversationView* transcript()const{return m_transcript;}
    QuestionPanel* questions()const{return m_questions;}
    MessageQueuePanel* queue()const{return m_queue;}
    void setState(const Conversation& chat,const QJsonArray& rows,bool busy,const QJsonArray& models,const QJsonObject& question,const QJsonArray& queue);
    void setError(const QString& message);
    void setUiScale(double scale);
signals:
    void submitted(QString chatId);void stopRequested(QString chatId);void promoteRequested(QString chatId);
    void changesRequested(QString chatId);
    void draftChanged(QString chatId,QString text);void modelChanged(QString chatId,QJsonObject selection);void permissionChanged(QString chatId,bool allow);
    void filesAttached(QString chatId,QStringList paths);void imageAttached(QString chatId,QImage image);void removeAttachmentRequested(QString chatId,int index);
protected:
    void resizeEvent(QResizeEvent*)override;
private:
    void fit();void updateActions();void applyQuestionVisibility();
    QString m_chatId,m_questionToken,m_messageToken;bool m_busy=false,m_available=true,m_hasAttachments=false;
    ConversationView* m_transcript=nullptr;Composer* m_composer=nullptr;QuestionPanel* m_questions=nullptr;MessageQueuePanel* m_queue=nullptr;
    AttachmentStrip* m_attachments=nullptr;QComboBox *m_models=nullptr,*m_permissions=nullptr;
    QToolButton *m_send=nullptr,*m_stop=nullptr,*m_promote=nullptr,*m_back=nullptr;
    QWidget* m_footer=nullptr;QScrollArea* m_controls=nullptr;QLabel *m_title=nullptr,*m_error=nullptr;
};
}
