#pragma once
#include <QJsonArray>
#include <QJsonObject>
namespace ade {
// Persistent queue entries are user messages plus a host-owned id. The id is
// removed before provider serialization; attachment metadata remains unchanged.
class QueuedMessages {
public:
  static constexpr int MaximumCount = 100;
  static constexpr int MaximumBytes = 16 * 1024 * 1024;
  static bool validate(const QJsonArray &queue, QString *error = nullptr);
  static QJsonObject create(const QString &text,
                            const QJsonArray &attachments = {},
                            const QJsonObject &references = {},
                            const QJsonObject &pathMetadata = {});
  static int indexOf(const QJsonArray &queue, const QString &id);
};
} // namespace ade
