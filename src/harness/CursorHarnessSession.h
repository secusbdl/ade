#pragma once
#include "CursorExecutable.h"
#include "HarnessWorkspace.h"
#include "JsonRpcProcess.h"
#include <QFutureWatcher>
#include <QSet>
namespace ade {
struct CursorHarnessOptions {
  QString executable, providerId, ownerId, workspace, model, mode = "agent";
  QString instructions, historyContext;
  HarnessAttachments attachments;
  int promptTimeoutMs = 600000;
  bool alwaysAllow = false;
};
// Optional vendor CLI. ADE never reads its credential files or launches a
// shell. authenticate(token) is an explicit UI decision: vendor OAuth may open
// a browser.
class CursorHarnessSession final : public QObject {
  Q_OBJECT
public:
  explicit CursorHarnessSession(QObject *parent = nullptr);
  ~CursorHarnessSession() override;
  bool startTurn(const CursorHarnessOptions &, const QJsonArray &input,
                 const QJsonObject &saved = {});
  bool authenticate(const QString &token);
  bool rejectAuthentication(const QString &token);
  bool answerApproval(const QString &token, bool allow);
  bool validQuestionAnswer(const QString &token,
                           const QJsonObject &answers) const;
  bool answerQuestion(const QString &token, const QJsonObject &answers);
  bool cancelQuestion(const QString &token);
  bool answerPlan(const QString &token, bool accepted);
  void interrupt();
  void shutdown();
  bool isBusy() const { return m_busy; }
  QString turnId() const { return m_turn; }
  QJsonObject sessionState() const;
signals:
  void authenticationRequested(QString token, QJsonArray methods);
  void approvalRequired(QString token, QJsonObject request);
  void questionRequired(QString token, QJsonObject form);
  void planRequired(QString token, QJsonObject plan);
  void sessionStateChanged(QJsonObject state);
  void event(QJsonObject event);
  void failed(QString error);
  void finished(QJsonObject result);

private:
  struct Pending {
    QJsonValue id;
    QString method;
    QJsonObject params;
  };
  struct Prepared {
    AcpPromptPayload payload;
    QString error;
  };
  bool current(quint64 epoch) const { return m_busy && epoch == m_epoch; }
  bool call(const QString &, const QJsonObject &, int timeout = 30000);
  bool publish(QJsonObject);
  void beginSession();
  void configure();
  void prompt();
  void sendPrompt(const AcpPromptPayload &);
  void response(const QString &, const QJsonValue &, const QJsonObject &);
  void notification(const QString &, const QJsonObject &);
  void request(const QJsonValue &, const QString &, const QJsonObject &);
  bool reply(const QString &, const QString &, const QJsonObject &);
  void refreshDeadline();
  void settle();
  void finish(const QString &, const QString &error = {});
  void cancelPreparation();
  JsonRpcProcess m_process;
  CursorHarnessOptions m_options;
  CursorExecutable m_executable;
  HarnessWorkspace m_workspace;
  QJsonObject m_saved, m_capabilities, m_usage;
  QJsonArray m_input, m_events, m_config;
  QHash<QString, QString> m_calls;
  QHash<QString, Pending> m_pending;
  QHash<QString, QJsonObject> m_tools;
  QSet<QString> m_configured;
  QString m_session, m_turn, m_promptId, m_authToken, m_part, m_partType;
  QPointer<QFutureWatcher<Prepared>> m_preparing;
  std::shared_ptr<std::atomic_bool> m_cancelPreparation;
  QTimer m_deadline, m_cancel, m_settle;
  int m_remaining = 0, m_partNumber = 0;
  quint64 m_epoch = 0;
  qsizetype m_bytes = 0;
  bool m_busy = false, m_authenticated = false, m_promptSent = false,
       m_promptReturned = false, m_new = false, m_interrupted = false;
};
} // namespace ade
