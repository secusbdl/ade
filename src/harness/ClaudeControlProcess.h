#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>

namespace ade {
// Native Claude CLI stream-json transport, using the official SDK control-frame
// contract. No SDK sidecar is loaded and no account credentials are read.
class ClaudeControlProcess final : public QObject {
  Q_OBJECT
public:
  explicit ClaudeControlProcess(QObject *parent = nullptr);
  ~ClaudeControlProcess() override;
  bool start(const QString &executable, const QStringList &arguments,
             const QString &directory, const QProcessEnvironment &environment);
  QString control(const QString &subtype, const QJsonObject &parameters = {},
                  int timeoutMs = 30000);
  bool sendUser(const QJsonArray &content, const QString &sessionId = {});
  bool respond(const QString &requestId, const QJsonObject &response);
  bool reject(const QString &requestId, const QString &message);
  void stop();
  bool isRunning() const;
  static QProcessEnvironment managedEnvironment();
signals:
  void started();
  void controlResponse(QString requestId, QJsonObject response, QString error);
  void controlRequest(QString requestId, QString subtype, QJsonObject request);
  void controlCanceled(QString requestId);
  void frame(QJsonObject value);
  void failed(QString message);

private:
  bool send(const QJsonObject &object);
  void readOutput();
  void fail(const QString &message);
  void checkDeadlines();
  QPointer<QProcess> m_process;
  QByteArray m_input;
  QMap<QString, qint64> m_deadlines;
  QSet<QString> m_inbound;
  QTimer m_clock;
  quint64 m_generation = 0;
  void *m_job = nullptr;
};
} // namespace ade
