#pragma once
#include "harness/HarnessWorkspace.h"
#include "harness/JsonRpcProcess.h"
#include <QJsonArray>
#include <QSet>
#include <QTimer>

namespace ade {
struct AcpHarnessOptions {
  QString executable, providerId, workspace, model, reasoningEffort;
  QString instructions, historyContext;
  bool thinkingDisabled = false;
  bool alwaysAllow = false;
};
// Native OpenCode ACP v1 session. The optional external CLI owns authentication
// and tools; this class never reads its credential files or exports tokens.
class AcpHarnessSession final : public QObject {
  Q_OBJECT
public:
  explicit AcpHarnessSession(QObject *parent = nullptr);
  ~AcpHarnessSession() override;
  bool startTurn(const AcpHarnessOptions &options, const QJsonArray &input,
                 const QJsonObject &savedState = {});
  void interrupt();
  void shutdown();
  bool answerApproval(const QString &token, bool allow);
  bool answerQuestion(const QString &token, const QJsonObject &answers);
  bool validQuestionAnswer(const QString& token,const QJsonObject& answers) const;
  bool cancelQuestion(const QString &token);
  QString turnId() const { return m_turnId; }
  bool isBusy() const { return m_busy; }
  QJsonObject sessionState() const;
  static QProcessEnvironment environment(bool alwaysAllow);
signals:
  void event(QJsonObject event);
  void sessionStateChanged(QJsonObject state);
  void approvalRequired(QString token, QJsonObject request);
  void questionRequired(QString token, QJsonObject request);
  void authenticationRequired(QJsonArray methods);
  void finished(QJsonObject result);
  void failed(QString message);

private:
  struct Pending {
    QJsonValue id;
    QJsonObject parameters;
    bool question = false;
  };
  void initialize();
  void beginSession();
  void configure();
  void prompt();
  bool call(const QString &method, const QJsonObject &params,
            int timeout = 30000);
  void response(const QString &id, const QJsonValue &result,
                const QJsonObject &error);
  void notification(const QString &method, const QJsonObject &params);
  void request(const QJsonValue &id, const QString &method,
               const QJsonObject &params);
  void finish(const QString &status, const QString &error = {});
  bool publish(QJsonObject event);
  bool active(quint64 generation) const;
  JsonRpcProcess m_process;
  AcpHarnessOptions m_options;
  HarnessWorkspace m_workspace;
  QJsonObject m_saved, m_capabilities, m_usage;
  QHash<QString, QJsonObject> m_toolStates;
  QJsonArray m_input, m_config, m_authMethods, m_parts;
  QHash<QString, QString> m_calls;
  QHash<QString, Pending> m_pending;
  QSet<QString> m_configured;
  QString m_sessionId, m_currentPart, m_currentType, m_turnId;
  QTimer m_cancelTimer;
  quint64 m_generation = 0;
  qsizetype m_outputSize = 0;
  int m_partSequence = 0;
  bool m_busy = false, m_initialized = false, m_created = false;
  bool m_promptSent = false, m_interrupted = false;
};
} // namespace ade
