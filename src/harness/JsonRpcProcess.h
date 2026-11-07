#pragma once

#include "AcpPromptPayload.h"
#include <QJsonObject>
#include <QQueue>
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
  QString requestAcpPrompt(const AcpPromptPayload &payload, int timeoutMs);
  enum class PromptWriteCancel { NotPending, RemovedBeforeWrite, StoppedPartial };
  PromptWriteCancel cancelAcpPromptWrite(const QString &requestId);
  qint64 peakQueuedWriteBytes() const { return m_peakQueuedBytes; }
  bool notify(const QString &method, const QJsonObject &parameters = {});
  bool respond(const QJsonValue &id, const QJsonValue &result);
  bool reject(const QJsonValue &id, int code, const QString &message);
  void stop();
  bool suspendDeadline(const QString &requestId);
  bool resumeDeadline(const QString &requestId);
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
  struct OutboundFrame {
    QList<QByteArray> segments;
    AcpPromptPayload prompt;
    qsizetype segment = 0, offset = 0;
    qint64 size = 0, end = 0;
    bool acp = false;
    QString requestId;
  };
  struct SentFrame {
    qint64 end = 0, size = 0;
    bool acp = false;
    // QProcess may still own up to1MiB after the final frame segment was
    // accepted. Retain captured inputs until bytesWritten covers this end.
    AcpPromptPayload prompt;
  };
  bool enqueue(OutboundFrame frame);
  void pumpWrites();
  void schedulePump();
  void written(qint64 bytes);
  QQueue<OutboundFrame> m_outbound;
  QQueue<SentFrame> m_sent;
  QString m_acpWriteId;
  qint64 m_acpStart = 0, m_acpEnd = 0;
  qint64 m_totalPlanned = 0, m_totalQueued = 0, m_totalWritten = 0;
  qint64 m_normalOutstanding = 0, m_peakQueuedBytes = 0;
  bool m_pumpScheduled = false;
  void fail(const QString &error);
  void checkDeadlines();
  QPointer<QProcess> m_process;
  QByteArray m_input;
  QMap<QString, qint64> m_deadlines;
  QMap<QString, qint64> m_pausedDeadlines;
  QTimer m_clock;
  quint64 m_generation = 0;
  void *m_job = nullptr;
};
} // namespace ade
