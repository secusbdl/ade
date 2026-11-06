#pragma once
#include "TranscriptLinkResolver.h"
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ade {
// This is host-created session state, outside the model's message metadata.
// A saved binding still has to match the current physical mount identity.
inline QJsonObject
encodeLinkProvenance(const QHash<QString, TranscriptLinkProvenance> &rows) {
  QJsonObject result;
  QStringList ids = rows.keys();
  ids.sort();
  for (const auto &id : ids) {
    if (id.isEmpty() || id.size() > 128 || result.size() >= 8192)
      continue;
    const auto &row = rows[id];
    QJsonObject urls, names;
    for (auto it = row.historicalTabUrls.cbegin();
         it != row.historicalTabUrls.cend(); ++it)
      urls[it.key()] = it.value().toString(QUrl::FullyEncoded);
    for (auto it = row.attachmentNames.cbegin();
         it != row.attachmentNames.cend(); ++it)
      names[it.key()] = it.value();
    result[id] = QJsonObject{{"version", 1},
                             {"mountBindings", row.mountBindings},
                             {"browserSessionId", row.browserSessionId},
                             {"historicalTabUrls", urls},
                             {"attachmentNames", names}};
  }
  // Do not silently persist an incomplete authority map under a size cap.
  return QJsonDocument(result).toJson(QJsonDocument::Compact).size() <=
                 2 * 1024 * 1024
             ? result
             : QJsonObject{};
}
inline QHash<QString, TranscriptLinkProvenance>
decodeLinkProvenance(const QJsonObject &encoded) {
  QHash<QString, TranscriptLinkProvenance> result;
  if (encoded.size() > 8192 ||
      QJsonDocument(encoded).toJson(QJsonDocument::Compact).size() >
          2 * 1024 * 1024)
    return result;
  const QRegularExpression prefix("^[A-Za-z0-9_-]{1,64}$"),
      binding("^[a-f0-9]{64}$");
  for (auto it = encoded.begin(); it != encoded.end(); ++it) {
    if (it.key().isEmpty() || it.key().size() > 128 || !it.value().isObject())
      continue;
    const auto value = it.value().toObject();
    if (value["version"] != 1 || !value["mountBindings"].isObject() ||
        !value["historicalTabUrls"].isObject() ||
        !value["attachmentNames"].isObject() ||
        !value["browserSessionId"].isString())
      continue;
    TranscriptLinkProvenance row;
    row.browserSessionId = value["browserSessionId"].toString();
    const auto mounts = value["mountBindings"].toObject(),
               urls = value["historicalTabUrls"].toObject(),
               names = value["attachmentNames"].toObject();
    if (row.browserSessionId.size() > 128 || mounts.size() > 64 ||
        urls.size() > 128 || names.size() > 256)
      continue;
    bool valid = true;
    for (auto m = mounts.begin(); m != mounts.end(); ++m) {
      if (!prefix.match(m.key()).hasMatch() || !m.value().isString() ||
          !binding.match(m.value().toString()).hasMatch()) {
        valid = false;
        break;
      }
      row.mountBindings[m.key()] = m.value();
    }
    for (auto u = urls.begin(); u != urls.end(); ++u) {
      const QUrl url(u.value().toString(), QUrl::StrictMode);
      if (!prefix.match(u.key()).hasMatch() || !u.value().isString() ||
          u.value().toString().size() > 16384 || !url.isValid() ||
          url.host().isEmpty() || !url.userInfo().isEmpty() ||
          (url.scheme() != "http" && url.scheme() != "https")) {
        valid = false;
        break;
      }
      row.historicalTabUrls[u.key()] = url;
    }
    for (auto n = names.begin(); n != names.end(); ++n) {
      if (n.key().size() > 4096 || !n.value().isString() ||
          n.value().toString().size() > 2048) {
        valid = false;
        break;
      }
      row.attachmentNames[n.key()] = n.value().toString();
    }
    if (valid)
      result.insert(it.key(), row);
  }
  return result;
}
} // namespace ade
