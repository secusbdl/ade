#pragma once
#include <QFrame>
#include <QJsonObject>
#include <functional>
class QVBoxLayout;
class QLabel;
namespace ade {
class QuestionPanel final : public QFrame {
  Q_OBJECT
public:
  explicit QuestionPanel(QWidget *parent = nullptr);
  void setQuestion(const QString &chatId, const QJsonObject &question);
  void setError(const QString &chatId, const QString &token,
                const QString &error);
  QJsonObject draftAnswers() const;
  QString chatId() const { return m_chatId; }
  QString token() const { return m_question["id"].toString(); }
signals:
  void submitStep(QString chatId, QString token, int step, QJsonObject answers);
  void goBack(QString chatId, QString token, int step);
  void cancelled(QString chatId, QString token, QJsonObject answers);
  void draftChanged(QString chatId, QString token, int step,
                    QJsonObject answers);
  void messageRequested(QString chatId, QString token);

private:
  void rebuild();
  void changed();
  QString m_chatId;
  QJsonObject m_question;
  QVBoxLayout *m_layout;
  QLabel *m_error = nullptr;
  QHash<QString, std::function<QJsonValue()>> m_values;
};
} // namespace ade
