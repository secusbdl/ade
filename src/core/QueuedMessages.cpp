#include "QueuedMessages.h"
#include "AgentPresentation.h"
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
namespace ade {
bool QueuedMessages::validate(const QJsonArray &queue, QString *error) {
  if (error)
    error->clear();
  auto reject = [&](const QString &text) {
    if (error)
      *error = text;
    return false;
  };
  if (queue.size() > MaximumCount ||
      QJsonDocument(queue).toJson(QJsonDocument::Compact).size() > MaximumBytes)
    return reject("The message queue exceeds its 100 message or 16 MiB limit.");
  QSet<QString> ids;
  for (const auto &value : queue) {
    const auto row = value.toObject();
    const auto id = row["id"].toString();
    if (!value.isObject() || id.isEmpty() || id.size() > 128 ||
        ids.contains(id) || row["role"] != "user" ||
        !row["content"].isString() ||
        (row.contains("metadata") && !row["metadata"].isObject()))
      return reject("A queued user message is invalid or duplicated.");
    for (auto it = row.begin(); it != row.end(); ++it)
      if (it.key() != "id" && it.key() != "role" && it.key() != "content" &&
          it.key() != "metadata")
        return reject("A queued message contains an unsupported replay field.");
    const auto metadata = row["metadata"].toObject(),
               refs = metadata["pathReferences"].toObject();
    Q_UNUSED(refs);
    const auto attachments = metadata["attachments"];
    if ((!attachments.isUndefined() &&
         (!attachments.isArray() || attachments.toArray().size() > 32)) ||
        (metadata.contains("pathReferences") &&
         !metadata["pathReferences"].isObject()) ||
        (metadata.contains("pathMetadata") &&
         !metadata["pathMetadata"].isObject()))
      return reject("Queued attachment metadata is invalid.");
    for (const auto &entry : attachments.toArray())
      if (!entry.isObject() || !entry.toObject()["path"].isString() ||
          entry.toObject()["path"].toString().isEmpty() ||
          entry.toObject()["path"].toString().size() > 32768)
        return reject("Queued attachment reference is invalid.");
    if (row["content"].toString().trimmed().isEmpty() &&
        attachments.toArray().isEmpty())
      return reject("A queued message cannot be empty.");
    ids.insert(id);
  }
  return true;
}
int QueuedMessages::indexOf(const QJsonArray &queue, const QString &id) {
  for (int i = 0; i < queue.size(); ++i)
    if (queue[i].toObject()["id"].toString() == id)
      return i;
  return -1;
}
QJsonObject QueuedMessages::create(const QString &text,
                                   const QJsonArray &attachments,
                                   const QJsonObject &references,
                                   const QJsonObject &pathMetadata) {
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonObject row{{"id", id}, {"role", "user"}, {"content", text}};
  if (!attachments.isEmpty()) {
    QJsonObject metadata{{"attachments", attachments},
                         {"pathReferences", references}};
    if (!pathMetadata.isEmpty())
      metadata["pathMetadata"] = pathMetadata;
    row["metadata"] = metadata;
  }
  const QJsonArray parts{QJsonObject{
      {"id", id + "/0"}, {"type", "text"}, {"text", text}, {"state", "done"}}};
  return AgentPresentation::withMetadata(
      row, {{"version", 1}, {"id", id}, {"parts", parts}});
}
} // namespace ade
