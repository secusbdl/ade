#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <memory>
namespace ade {
class SessionLogStore;
class TerminalModel;
class ConPtyProcess;
class ShellService final : public QObject {
  Q_OBJECT
public:
  ShellService(QString agentId, std::shared_ptr<SessionLogStore> logs,
               QObject *parent = nullptr);
  ~ShellService() override;
  static QJsonArray definitions();
  static bool handles(const QString &tool);
  bool setMounts(const QMap<QString, QString> &mounts,
                 QString *error = nullptr);
  QJsonObject snapshot() const;
  QString manifest() const;
  bool ownsSession(const QString &sessionId) const;
  TerminalModel *sessionModel(const QString &sessionId) const;
  ConPtyProcess *sessionProcess(const QString &sessionId) const;
  void execute(const QString &requestId, const QString &tool,
               const QJsonObject &arguments);
  void cancel(const QString &requestId);
  void cancelAll();
  void closeAll();
signals:
  void completed(QString requestId, QString output, bool error);
  void output(QString requestId, QString text);
  void outputSnapshot(QString requestId, QString text);
  void manifestChanged();
  void watcherCompleted(QJsonObject result);

private:
  struct Session;
  using SessionPtr = std::shared_ptr<Session>;
  QString resolveCwd(const QString &mounted, QString *error) const;
  void create(const QString &requestId, const QJsonObject &args,
              bool watcher = false);
  void command(const QString &requestId, const QJsonObject &args);
  void dispatch(const SessionPtr &session);
  void pumpInput(const SessionPtr &session);
  void receive(const SessionPtr &session, const QByteArray &bytes);
  void processExited(const SessionPtr &session, int code);
  void resolve(const SessionPtr &session, const QString &reason,
               int exitCode = -1);
  void kill(const SessionPtr &session, const QString &reason);
  void notifyWatcher(const SessionPtr &session, const QString &outcome,
                     int code);
  bool owns(const SessionPtr &session) const;
  bool pruneExited();
  bool retire(const SessionPtr &session);
  QString m_agentId;
  std::shared_ptr<SessionLogStore> m_logs;
  QMap<QString, QString> m_mounts;
  QMap<QString, std::shared_ptr<void>> m_mountPins;
  QMap<QString, SessionPtr> m_sessions;
  QMap<QString, QString> m_requests;
  quint64 m_generation = 0;
  quint64 m_exitSerial = 0;
  bool m_closing = false;
};
} // namespace ade
