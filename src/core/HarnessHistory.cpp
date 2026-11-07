#include "HarnessHistory.h"
#include <QCryptographicHash>
#include <QJsonDocument>
namespace ade {
QByteArray HarnessHistory::canonicalDigest(const QJsonArray &messages) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayLiteral("ade-external-history-v1\n"));
  for (const auto &value : messages) {
    const auto row = value.toObject();
    const auto role = row.value("role").toString();
    if (role != "user" && role != "assistant")
      continue;
    QJsonObject canonical{{"role", role}, {"content", row.value("content")}};
    if (row.value("ade_compressedHistory").isString())
      canonical["summary"] = row.value("ade_compressedHistory");
    if (role == "user") {
      const auto metadata = row.value("metadata").toObject();
      // Accepted logical references are context; transient prepared bytes,
      // physical delivery paths and UI/tool presentation metadata are not.
      for (const auto *key : {"attachments", "pathReferences", "pathMetadata"})
        if (metadata.contains(key))
          canonical[key] = metadata.value(key);
    }
    const auto bytes = QJsonDocument(canonical).toJson(QJsonDocument::Compact);
    hash.addData(QByteArray::number(bytes.size()));
    hash.addData(QByteArrayLiteral(":"));
    hash.addData(bytes);
  }
  return hash.result().toHex();
}
QJsonArray HarnessHistory::withoutBindings(const QJsonArray &messages) {
  auto result = messages;
  for (int i = 0; i < result.size(); ++i) {
    auto row = result[i].toObject();
    auto metadata = row.value("metadata").toObject();
    auto state = metadata.value("harness").toObject();
    if (!state.contains("continuity"))
      continue;
    state.remove("continuity");
    metadata["harness"] = state;
    row["metadata"] = metadata;
    result[i] = row;
  }
  return result;
}
QJsonObject HarnessHistory::bind(QJsonObject state,
                                 const QJsonArray &messages) {
  state.remove("continuity");
  const auto owner = state.value("ownerId").toString();
  if (state.isEmpty() || owner.isEmpty() || owner.size() > 512 ||
      state.value("providerId").toString().isEmpty() ||
      state.value("kind").toString().isEmpty())
    return state;
  state["continuity"] =
      QJsonObject{{"version", 1},
                  {"ownerId", owner},
                  {"providerId", state.value("providerId")},
                  {"kind", state.value("kind")},
                  {"digest", QString::fromLatin1(canonicalDigest(messages))}};
  return state;
}
QJsonObject HarnessHistory::resumable(const QJsonArray &prefix,
                                      const QString &providerId,
                                      const QString &kind) {
  if (prefix.isEmpty())
    return {};
  const auto last = prefix.last().toObject();
  // Never jump over a new HTTP/external turn, interrupted user, or tool result
  // to find an older mutable external session.
  if (last.value("role") != "assistant")
    return {};
  const auto state =
      last.value("metadata").toObject().value("harness").toObject();
  const auto proof = state.value("continuity").toObject();
  if (state.value("providerId") != providerId || state.value("kind") != kind ||
      state.value("ownerId").toString().isEmpty() ||
      proof.value("version") != 1 ||
      proof.value("ownerId") != state.value("ownerId") ||
      proof.value("providerId") != providerId || proof.value("kind") != kind ||
      proof.value("digest").toString() !=
          QString::fromLatin1(canonicalDigest(prefix)))
    return {};
  return state;
}
} // namespace ade
