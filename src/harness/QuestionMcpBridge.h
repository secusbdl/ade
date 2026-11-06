#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTcpServer>
#include <memory>
class QTcpSocket;
namespace ade {
class QuestionMcpBridge final : public QObject {
  Q_OBJECT
public:
  explicit QuestionMcpBridge(QObject *parent = nullptr);
  ~QuestionMcpBridge() override;
  QJsonObject start(const QString &executable, QString *error = nullptr);
  void beginTurn(const QString &turn);
  void endTurn();
  void revoke();
  bool complete(const QString &token, const QJsonObject &result,
                const QString &error = {});
signals:
  void questionRequested(QString token, QString turn, QJsonObject form);
  void questionCancelled(QString token);

private:
  struct Connection;
  void accept();
  void receive(const std::shared_ptr<Connection> &connection);
  void reply(const std::shared_ptr<Connection> &connection, int status,
             const QByteArray &body);
  void disconnected(const std::shared_ptr<Connection> &connection);
  void stop(bool notify);
  quint64 m_epoch = 0;
  QTcpServer m_server;
  QHash<QTcpSocket *, std::shared_ptr<Connection>> m_connections;
  QJsonObject m_config;
  QByteArray m_secret;
  QString m_turn, m_pendingToken, m_pendingTurn;
  std::shared_ptr<Connection> m_pending;
};
} // namespace ade
