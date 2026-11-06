#pragma once
#include <QFrame>
#include <QJsonArray>
class QVBoxLayout;
namespace ade {
class MessageQueuePanel final : public QFrame {
  Q_OBJECT
public:
  explicit MessageQueuePanel(QWidget *parent = nullptr);
  void setQueue(const QString &chatId, const QJsonArray &messages, bool busy);
signals:
  void editRequested(QString chatId, QString id, QString text);
  void removeRequested(QString chatId, QString id);
  void moveRequested(QString chatId, QString id, int index);
  void sendNowRequested(QString chatId, QString id);
  void resumeRequested(QString chatId);

private:
  QVBoxLayout *m_layout;
  QString m_chatId;
  QJsonArray m_messages;
  bool m_busy = false;
};
} // namespace ade
