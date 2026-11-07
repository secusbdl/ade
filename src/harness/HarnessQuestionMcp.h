#pragma once
#include "QuestionMcpBridge.h"
namespace ade {
// Transport-only, owner-scoped question capability. Its configuration must
// never enter canonical model history or persisted ADE session state.
class HarnessQuestionMcp final : public QObject {
  Q_OBJECT
public:
  explicit HarnessQuestionMcp(QObject *parent = nullptr);
  QJsonObject configure(const QString &executable, QString *error);
  void beginTurn(const QString &turn);
  void endTurn();
  void revoke();
  void setBlocked(bool blocked) { m_blocked = blocked; }
  bool pending() const { return !m_token.isEmpty(); }
  bool owns(const QString &token) const {
    return !token.isEmpty() && token == m_token;
  }
  QString turn() const { return m_turn; }
  bool complete(const QString &token, const QJsonObject &result,
                const QString &error = {});
  static QString preference();
  // UI-only annotation, used only after an adapter verifies its own configured
  // server/tool identity. Raw tool name, arguments and results remain intact.
  static QJsonObject wrapperPresentation(const QString &rawName);
signals:
  void pendingChanged(bool pending);
  void event(QJsonObject event);
  void required(QString token, QJsonObject form);
  void cancelled(QString token);

private:
  void cancelOwned(const QString &token);
  static QJsonObject presentation(const QString &token, const QString &turn,
                                  const QJsonObject &form,
                                  const QJsonObject &result,
                                  const QString &error = {});
  QuestionMcpBridge m_bridge;
  QString m_executable, m_turn, m_pendingTurn, m_token;
  QJsonObject m_form;
  quint64 m_epoch = 0;
  bool m_blocked = false;
};
} // namespace ade
