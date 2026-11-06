#pragma once
#include "CodexAppServer.h"
#include "HarnessWorkspace.h"
#include <QHash>

namespace ade {
struct CodexHarnessOptions {
  QString executable, providerId, model, reasoningEffort, workspace,
      instructions;
  bool alwaysAllow = false;
  QString historyContext;
};

// One host-owned chat/session. Output is normalized into bounded UI events;
// native CLI thread IDs stay in separate nonsecret session state.
class CodexHarnessSession final : public QObject {
  Q_OBJECT
public:
  explicit CodexHarnessSession(QObject *parent = nullptr);
  ~CodexHarnessSession() override;
  bool startTurn(const CodexHarnessOptions &options, const QJsonArray &input,
                 const QJsonObject &savedState = {});
  void interrupt();
  void shutdown();
  bool answerApproval(const QString &token, bool allow);
  bool answerQuestion(const QString &token, const QJsonObject &answers);
  bool cancelQuestion(const QString &token);
  bool isBusy() const { return m_busy; }
  QJsonObject sessionState() const;
  // Fork is explicitly requested by the owning host. Never resume a source
  // thread for a cloned chat: copy state with fork:true for the first turn.
  static QJsonObject forkState(QJsonObject source);
signals:
  void event(QJsonObject event);
  void sessionStateChanged(QJsonObject state);
  void approvalRequired(QString token, QString name, QJsonObject details);
  void questionRequired(QString token, QJsonArray questions);
  void finished(QJsonObject result);
  void failed(QString error);

private:
  struct PendingInput {
    QJsonValue id;
    QString method, itemId;
    QJsonObject parameters;
  };
  void beginThread();
  void sendTurn();
  void receiveReply(const QString &id, const QString &method,
                    const QJsonValue &result, const QJsonObject &error);
  void receiveNotification(const QString &method, const QJsonObject &params);
  void receiveRequest(const QJsonValue &id, const QString &method,
                      const QJsonObject &params);
  bool belongs(const QJsonObject &params, bool permitNewTurn = false);
  void finish(const QString &status, const QString &error = {});
  void publishItem(const QJsonObject &item, bool completed);
  bool publish(QJsonObject value);
  void settleInputs();
  CodexAppServer m_server;
  CodexHarnessOptions m_options;
  HarnessWorkspace m_workspaceBinding;
  QJsonArray m_input, m_order, m_earlyEvents;
  QJsonObject m_saved, m_usage;
  QHash<QString, QJsonObject> m_items;
  QHash<QString, QString> m_text, m_reasoning;
  QHash<QString, PendingInput> m_pending;
  QHash<QString, QString> m_requests;
  QString m_threadId, m_turnId;
  QTimer m_timeout;
  quint64 m_generation = 0;
  qsizetype m_outputChars = 0, m_itemBytes = 0, m_earlyBytes = 0;
  bool m_busy = false, m_interrupted = false, m_turnSent = false,
       m_sessionCreated = false;
};
} // namespace ade
