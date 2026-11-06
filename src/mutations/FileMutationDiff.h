#pragma once
#include "FileMutationExecutor.h"
#include <QJsonArray>
#include <QStringDecoder>
namespace ade {
// Host-only display data. Provider tool content never includes these snapshots.
inline QJsonObject
mutationDiffMetadata(const QList<FileMutationChange> &changes) {
  QJsonArray files;
  qsizetype used = 0;
  int omitted = 0;
  for (const auto &change : changes) {
    if (change.before == change.after &&
        change.existedBefore == change.existsAfter)
      continue;
    if (files.size() >= 8 ||
        change.before.size() + change.after.size() > 512 * 1024) {
      ++omitted;
      continue;
    }
    QStringDecoder beforeDecoder(QStringDecoder::Utf8,
                                 QStringConverter::Flag::Stateless),
        afterDecoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString before = beforeDecoder.decode(change.before),
                  after = afterDecoder.decode(change.after);
    if (files.size() >= 8 || used + before.size() + after.size() > 128 * 1024 ||
        beforeDecoder.hasError() || afterDecoder.hasError() ||
        before.contains(QChar::Null) || after.contains(QChar::Null) ||
        change.path.size() > 32768) {
      ++omitted;
      continue;
    }
    used += before.size() + after.size();
    files.append(QJsonObject{
        {"path", change.path},
        {"before", change.existedBefore ? QJsonValue(before)
                                        : QJsonValue(QJsonValue::Null)},
        {"after", change.existsAfter ? QJsonValue(after)
                                     : QJsonValue(QJsonValue::Null)}});
  }
  return {{"version", 1}, {"files", files}, {"omitted", omitted}};
}
inline bool validMutationDiff(const QJsonObject &value) {
  if (value["version"] != 1 || !value["files"].isArray() ||
      value["files"].toArray().size() > 8 || !value["omitted"].isDouble() ||
      value["omitted"].toInt(-1) < 0)
    return false;
  qsizetype total = 0;
  for (const auto &item : value["files"].toArray()) {
    if (!item.isObject())
      return false;
    const auto file = item.toObject();
    if (!file["path"].isString() || file["path"].toString().size() > 32768)
      return false;
    for (const auto *key : {"before", "after"}) {
      if (!file[key].isString() && !file[key].isNull())
        return false;
      const auto text = file[key].toString();
      if (text.contains(QChar::Null))
        return false;
      total += text.size();
    }
    if (total > 128 * 1024)
      return false;
  }
  return true;
}
} // namespace ade
