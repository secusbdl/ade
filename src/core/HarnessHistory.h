#pragma once
#include <QJsonArray>
#include <QJsonObject>
namespace ade {
// Host-only continuity for mutable external sessions. This is consistency
// metadata, not a credential or an authorization grant.
class HarnessHistory final {
public:
  static QByteArray canonicalDigest(const QJsonArray &messages);
  static QJsonArray withoutBindings(const QJsonArray &messages);
  static QJsonObject bind(QJsonObject state, const QJsonArray &messages);
  static QJsonObject resumable(const QJsonArray &prefix,
                               const QString &providerId, const QString &kind);
};
} // namespace ade
