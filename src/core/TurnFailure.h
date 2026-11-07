#pragma once
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace ade {
// Durable identity of the latest accepted turn that failed. Header-only so
// SessionStore can validate on write and load without linking AgentEngine; a
// single shared predicate keeps the writer from producing records the strict
// loader would reject (which would refuse every saved chat).
namespace TurnFailure {
constexpr int MaximumErrorCharacters = 2000;
constexpr int MaximumCompactBytes = 4096;
inline bool validRowId(const QString &id) {
  if (id.isEmpty() || id.size() > 128)
    return false;
  for (const auto c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  return true;
}
inline bool validShape(const QJsonObject &record) {
  if (record.size() != 4 || !record.contains("version") ||
      !record.contains("userRowId") || !record.contains("error") ||
      !record.contains("failedAt"))
    return false;
  const auto version = record.value("version"),
             failedAt = record.value("failedAt");
  if (!version.isDouble() || version.toDouble() != 1)
    return false;
  if (!record.value("userRowId").isString() ||
      !validRowId(record.value("userRowId").toString()))
    return false;
  if (!record.value("error").isString() ||
      record.value("error").toString().size() > MaximumErrorCharacters)
    return false;
  // Integral, non-negative and exactly representable as a JSON number.
  if (!failedAt.isDouble() || failedAt.toDouble() < 0 ||
      failedAt.toDouble() > 9007199254740991.0 ||
      failedAt.toDouble() != double(failedAt.toInteger()))
    return false;
  return QJsonDocument(record).toJson(QJsonDocument::Compact).size() <=
         MaximumCompactBytes;
}
// Shrinks the error until the record satisfies validShape. Multibyte and
// escaped control characters can exceed the byte bound well before 2000
// characters. Returns an empty object for an invalid identity or time.
inline QJsonObject make(const QString &userRowId, const QString &error,
                        qint64 failedAt) {
  if (!validRowId(userRowId) || failedAt < 0)
    return {};
  auto text = error.left(MaximumErrorCharacters);
  // The character bound must not split a surrogate pair either.
  if (error.size() > text.size() && !text.isEmpty() &&
      text.back().isHighSurrogate())
    text.chop(1);
  for (;;) {
    const QJsonObject record{{"version", 1},
                             {"userRowId", userRowId},
                             {"error", text},
                             {"failedAt", double(failedAt)}};
    if (validShape(record))
      return record;
    if (text.isEmpty())
      return {};
    int next = text.size() * 3 / 4;
    // Never split a surrogate pair.
    if (next > 0 && text.at(next - 1).isHighSurrogate())
      --next;
    text.truncate(next);
  }
}
} // namespace TurnFailure
} // namespace ade
