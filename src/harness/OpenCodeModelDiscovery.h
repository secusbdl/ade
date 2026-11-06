#pragma once
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
namespace ade {
// Optional installed OpenCode CLI discovery. Credentials remain CLI-managed.
class OpenCodeModelDiscovery final : public QObject {
  Q_OBJECT
public:
  explicit OpenCodeModelDiscovery(QObject *parent = nullptr);
  ~OpenCodeModelDiscovery() override;
  bool start(const QString &executable = {});
  void cancel();
  bool isBusy() const { return !m_process.isNull(); }
  QString executable() const { return m_executable; }
  static QJsonArray parse(const QByteArray &output, QString *error = nullptr);
signals:
  void completed(QJsonArray models, QString error);

private:
  bool read();
  void finish(const QString &error = {});
  QPointer<QProcess> m_process;
  QTimer m_deadline;
  QByteArray m_output;
  QString m_executable;
  quint64 m_generation = 0;
  void *m_job = nullptr;
};
} // namespace ade
