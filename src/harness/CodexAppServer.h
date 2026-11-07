#pragma once
#include "JsonRpcProcess.h"
#include <QJsonArray>
#include <QSet>
#include <QUrl>

namespace ade {
// Owns an unmodified Codex CLI. Authentication remains in the CLI's managed
// store; no API for reading, importing or exporting OAuth credentials exists.
class CodexAppServer final : public QObject {
  Q_OBJECT
public:
  explicit CodexAppServer(QObject *parent = nullptr);
  ~CodexAppServer() override;
  bool start(const QString &executable = {}, const QString &directory = {});
  void stop();
  bool suspendDeadline(const QString &id) { return m_rpc.suspendDeadline(id); }
  bool resumeDeadline(const QString &id) { return m_rpc.resumeDeadline(id); }
  bool isReady() const { return m_ready; }
  bool isAuthenticated() const { return m_authenticated; }
  QString executable() const { return m_executable; }
  static QString findExecutable(const QString &configured = {});
  static bool validLoginUrl(const QUrl &url);
  void readAccount();
  void startLogin();
  void cancelLogin();
  void discoverModels();
  void readRateLimits();
  QString call(const QString &method, const QJsonObject &params,
               int timeoutMs = 30000);
  bool respond(const QJsonValue &id, const QJsonValue &result);
  bool reject(const QJsonValue &id, int code, const QString &message);
signals:
  void ready();
  // Intentionally contains no account email, tokens or raw account object.
  void accountChanged(bool subscriptionConnected, QString plan);
  void loginUrlReady(QUrl url);
  void loginFinished(bool success, QString message);
  void modelsReady(QJsonArray models);
  void rateLimitsReady(QJsonObject limits);
  void reply(QString id, QString method, QJsonValue result, QJsonObject error);
  void notification(QString method, QJsonObject params);
  void serverRequest(QJsonValue id, QString method, QJsonObject params);
  void failed(QString error);

private:
  void receiveReply(const QString &id, const QJsonValue &result,
                    const QJsonObject &error);
  void receiveNotification(const QString &method, const QJsonObject &params);
  void modelPage(const QString &cursor = {});
  JsonRpcProcess m_rpc;
  QMap<QString, QString> m_methods;
  QString m_executable, m_loginId, m_loginRequest;
  QJsonArray m_models;
  QSet<QString> m_modelIds, m_cursors;
  int m_pages = 0;
  bool m_ready = false, m_authenticated = false, m_discovering = false;
  quint64 m_generation = 0;
};
} // namespace ade
