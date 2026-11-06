#pragma once
#include <QJsonArray>
#include <QWidget>
#include <QUrl>
#include <memory>

namespace ade {
// Input is the read-only AgentEngine display projection, never raw provider
// replay history. Message and part IDs remain stable across streaming updates.
class ConversationView final : public QWidget {
    Q_OBJECT
public:
    explicit ConversationView(QWidget *parent=nullptr);
    ~ConversationView() override;
    void setConversation(const QString &id,const QJsonArray &messages,bool busy);
    void clear();
    QString toPlainText() const;
    QString conversationId() const;
    bool followsOutput() const;
public slots:
    void scrollToBottom();
signals:
    void linkActivated(QUrl url);
    void scopedLinkActivated(QString conversationId,QString messageId,QString partId,QUrl url);
    void approvalDecided(QString conversationId,QString messageId,QString partId,QString toolCallId,bool allowed);
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
} // namespace ade
