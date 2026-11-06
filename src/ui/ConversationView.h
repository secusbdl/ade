#pragma once
#include <QJsonArray>
#include <QWidget>
#include <QUrl>
#include <QImage>
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
    bool provideImage(quint64 requestId,const QImage &image,const QString &error={});
    bool provideTurnFileEdits(quint64 requestId,const QJsonObject& summary,const QString& error={});
    QStringList turnMutationIds(const QString& rowId)const;
public slots:
    void scrollToBottom();
signals:
    void linkActivated(QUrl url);
    void scopedLinkActivated(QString conversationId,QString messageId,QString partId,QUrl url);
    void approvalDecided(QString conversationId,QString messageId,QString partId,QString toolCallId,bool allowed);
    void imageRequested(quint64 requestId,QString conversationId,QString messageId,QString partId,QUrl url);
    void turnFileEditsRequested(quint64 requestId,QString conversationId,QString rowId,QStringList mutationIds);
    void turnFileRequested(QString conversationId,QString rowId,QStringList mutationIds,QString path);
    void turnFilesReviewRequested(QString conversationId,QString rowId,QStringList mutationIds);
    void savedTurnFileRequested(QString conversationId,QString rowId,QStringList mutationIds,QString path);
    void turnUndoReviewRequested(QString conversationId,QString rowId,QStringList mutationIds);
protected:
    bool eventFilter(QObject *object,QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
} // namespace ade
