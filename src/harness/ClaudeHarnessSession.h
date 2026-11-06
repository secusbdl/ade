#pragma once
#include "ClaudeControlProcess.h"
#include "ClaudeStream.h"
#include "harness/HarnessWorkspace.h"
#include <QHash>

namespace ade {
struct ClaudeHarnessOptions {
  QString executable, providerId, workspace, model, effort;
  QString instructions, historyContext;
  QString thinkingMode;
  int thinkingBudget = 0;
  bool alwaysAllow = false;
  bool questionsEnabled = false;
  int turnTimeoutMs = 600000;
};
// Private direct-CLI session foundation; unsupported control types fail
// explicitly.
class ClaudeHarnessSession final : public QObject {
  Q_OBJECT
public:
  explicit ClaudeHarnessSession(QObject *parent = nullptr);
  ~ClaudeHarnessSession() override;
  bool startTurn(const ClaudeHarnessOptions &options, const QJsonArray &input,
                 const QJsonObject &saved = {});
  void interrupt();
  void shutdown();
  bool answerApproval(const QString &token, bool allow);
  bool validQuestionAnswer(const QString &token,
                           const QJsonObject &answers) const;
  bool answerQuestion(const QString &token, const QJsonObject &answers);
  bool cancelQuestion(const QString &token);
  bool isBusy() const { return m_busy; }
  bool hasBackgroundTasks() const { return !m_tasks.isEmpty(); }
  QString turnId() const { return m_turnId; }
  QJsonObject sessionState() const;
signals:
  void event(QJsonObject frame);
  void approvalRequired(QString token, QJsonObject request);
  void questionRequired(QString token, QJsonObject request, QJsonObject form);
  void decisionCanceled(QString token);
  void sessionStateChanged(QJsonObject state);
  void finished(QJsonObject result);
  void failed(QString error);

private:
  struct Pending {
    QString requestId, toolId;
    QJsonObject input;
    bool question = false;
  };
  void sendPrompt();
  void consume(const QJsonObject &frame);
  void request(const QString &id, const QString &subtype,
               const QJsonObject &request);
  void finish(const QString &status, const QString &error = {});
  bool publish(const QJsonObject &frame);
  bool publishUpdates(const QJsonArray &updates);
  void pauseForDecision();
  void resumeAfterDecision();
  ClaudeControlProcess m_process;
  HarnessWorkspace m_workspace;
  ClaudeHarnessOptions m_options;
  QJsonArray m_input;
  ClaudeStream m_stream;
  QJsonObject m_usage;
  QHash<QString, Pending> m_pending;
  QSet<QString> m_tasks;
  QString m_sessionId, m_initializeId, m_turnId;
  QTimer m_deadline, m_cancelDeadline;
  qsizetype m_eventCount = 0;
  int m_waitRemaining = -1;
  quint64 m_generation = 0;
  bool m_busy = false, m_initialized = false, m_created = false,
       m_interrupted = false, m_promptSent = false, m_fork = false;
};
} // namespace ade
