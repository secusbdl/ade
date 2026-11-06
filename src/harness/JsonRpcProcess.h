#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>

namespace ade {
// Native, owned stdio transport. Protocol and stderr bytes are never logged.
class JsonRpcProcess final : public QObject {
  Q_OBJECT
public:
  explicit JsonRpcProcess(QObject *parent = nullptr);
  ~JsonRpcProcess() override;
  bool start(const QString &executable, const QStringList &arguments,
             const QString &directory, const QProcessEnvironment &environment);
  QString request(const QString &method, const QJsonObject &parameters,
                  int timeoutMs = 30000);
  bool notify(const QString &method, const QJsonObject &parameters = {});
  bool respond(const QJsonValue &id, const QJsonValue &result);
  bool reject(const QJsonValue &id, int code, const QString &message);
  void stop();
  bool isRunning() const;
  static QProcessEnvironment managedEnvironment();
signals:
  void started();
  void response(QString id, QJsonValue result, QJsonObject error);
  void notification(QString method, QJsonObject parameters);
  void serverRequest(QJsonValue id, QString method, QJsonObject parameters);
  void failed(QString message);

private:
  bool send(const QJsonObject &object);
  void readOutput();
  void fail(const QString &error);
  void checkDeadlines();
  QPointer<QProcess> m_process;
  QByteArray m_input;
  QMap<QString, qint64> m_deadlines;
  QTimer m_clock;
  quint64 m_generation = 0;
  void *m_job = nullptr;
};
} // namespace ade
