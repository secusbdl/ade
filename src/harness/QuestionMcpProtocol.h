#pragma once
#include <QJsonObject>
#include <QObject>
namespace ade {
// One-tool stdio MCP protocol. Form content is validated by the owning host.
class QuestionMcpProtocol final : public QObject {
  Q_OBJECT
public:
  explicit QuestionMcpProtocol(QJsonObject inputSchema,
                               QObject *parent = nullptr);
  void receive(const QJsonObject &message);
  bool complete(const QJsonValue &id, const QJsonObject &result,
                const QString &error = {});
  void cancelPending();
  bool hasPending() const { return !m_pending.isUndefined(); }
  static QJsonObject cancelledResult();
signals:
  void output(QJsonObject message);
  void questionRequested(QJsonValue id, QJsonObject form);
  void questionCancelled(QJsonValue id);

private:
  void result(const QJsonValue &id, const QJsonValue &value);
  void error(const QJsonValue &id, int code, const QString &message);
  QJsonObject m_schema;
  QJsonValue m_pending = QJsonValue::Undefined;
  bool m_initialized = false, m_ready = false;
};
} // namespace ade
