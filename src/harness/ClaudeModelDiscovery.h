#pragma once
#include "ClaudeControlProcess.h"
#include <QTemporaryDir>
#include <memory>
namespace ade {
// Reads only an allowlisted public catalog from CLI initialization. Account
// responses are discarded; initialization never establishes model entitlement.
class ClaudeModelDiscovery final : public QObject {
  Q_OBJECT
public:
  explicit ClaudeModelDiscovery(QObject *parent = nullptr);
  ~ClaudeModelDiscovery() override;
  bool start(const QString &executable = {});
  void cancel();
  bool isBusy() const { return m_busy; }
  QString executable() const { return m_executable; }
  static QJsonArray parse(const QJsonObject &response,
                          QString *error = nullptr);
signals:
  void completed(QJsonArray models, QString error);

private:
  void finish(const QJsonArray &models, const QString &error);
  ClaudeControlProcess m_process;
  std::unique_ptr<QTemporaryDir> m_scratch;
  QString m_executable, m_initializeId;
  bool m_busy = false;
};
} // namespace ade
