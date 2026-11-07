#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace ade {
// Only captured, owner-validated bytes enter the store. A path in model JSON
// cannot construct a lease or become an external runtime filesystem grant.
struct HarnessAttachmentCapture {
  QString reference, name, sha256;
  QByteArray bytes;
};
class HarnessAttachmentStore;
class HarnessAttachmentLease {
public:
  HarnessAttachmentLease() = default;
  bool valid() const;
  bool empty() const;
  QString owner() const;
  QStringList references() const;
  HarnessAttachmentLease excluding(const QStringList &references) const;
  HarnessAttachmentLease excluding(const HarnessAttachmentLease &other) const;
  QJsonArray resourceLinks(QString *error = nullptr) const;
  QString textDescriptors(QString *error = nullptr) const;
  // ACP-only exact original bytes. The index addresses this immutable lease;
  // callers cannot supply a path, MIME, owner, or a larger read allowance.
  static constexpr qint64 MaxAcpImageBytes = 20LL * 1024 * 1024;
  std::optional<QByteArray> originalAcpImage(qsizetype index,
      const std::atomic_bool *cancelled, QString *error = nullptr) const;

private:
  struct Data;
  std::shared_ptr<Data> m_data;
  friend class HarnessAttachmentStore;
};

class HarnessAttachmentStore {
public:
  static constexpr qint64 MaxFileBytes = 64LL * 1024 * 1024;
  static constexpr qint64 MaxTurnBytes = 256LL * 1024 * 1024;
  static constexpr qint64 MaxStoreBytes = 512LL * 1024 * 1024;
  explicit HarnessAttachmentStore(const QString &dataDirectory,
                                  const QString &ownerId);
  QString error() const;
  QString rootDirectory() const;
  QString profileDirectory() const;
  HarnessAttachmentLease prepare(const QList<HarnessAttachmentCapture> &files,
                                 const std::atomic_bool *cancelled,
                                 QString *error = nullptr) const;
  // A complete retained manifest for restart/new provider/fork reconstruction.
  HarnessAttachmentLease retained(QString *error = nullptr) const;
  HarnessAttachmentLease select(const QJsonObject &pathHashes,
                                QString *error = nullptr,
                                const QStringList &order = {}) const;
  HarnessAttachmentLease selectKeys(const QList<QPair<QString, QString>> &keys,
                                    QString *error = nullptr) const;
  std::optional<QByteArray> readCaptured(const QString &reference,
                                         const QString &sha256,
                                         QString *error = nullptr) const;
  bool cloneTo(const QString &targetOwner, QString *error = nullptr) const;
  void revoke();
  bool remove(QString *error = nullptr);
  static bool removeAgent(const QString &dataDirectory, const QString &ownerId,
                          QString *error = nullptr);
#ifdef ADE_HARNESS_ATTACHMENT_TESTING
  // Worker-safe deterministic failure/race seam. Never present in production.
  std::function<void(const QString &, const QString &)> boundary;
#endif
private:
  struct State;
  HarnessAttachmentStore(const QString &dataDirectory, const QString &ownerId,
                         bool requireNew, bool allowCreate);
  std::shared_ptr<State> m_state;
  friend class HarnessAttachmentLease;
};
} // namespace ade
