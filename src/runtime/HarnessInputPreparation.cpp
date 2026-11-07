#include "HarnessInputPreparation.h"
#include "attachments/FileTransformer.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>
namespace ade {
HarnessPreparedTurn prepareHarnessInput(const HarnessAttachmentStore &handoff,
                                        const AttachmentStore &store,
                                        const QString &owner,
                                        const QJsonArray &history,
                                        const AttachmentMounts &mounts,
                                        const ModelMediaCapabilities &media,
                                        const std::atomic_bool *cancelled) {
  HarnessPreparedTurn out;
  auto fail = [](const QString &error) {
    HarnessPreparedTurn result;
    result.error = error;
    return result;
  };
  const auto stopped = [&] { return cancelled && cancelled->load(); };
  if (stopped())
    return fail("Attachment preparation was cancelled.");
  if (history.isEmpty() || history.last().toObject()["role"] != "user" ||
      !history.last().toObject()["content"].isString())
    return fail(
        "External attachment input requires a current user text message.");
  out.input.append(QJsonObject{{"type", "text"},
                               {"text", history.last().toObject()["content"]}});
  QList<QPair<QString, QString>> currentKeys, historyKeys;
  QSet<QString> captured;
  qint64 aggregate = 0;
  int referencesCount = 0;
  int boundary = 0;
  for (int index = history.size() - 1; index >= 0; --index)
    if (!history[index]
             .toObject()["ade_compressedHistory"]
             .toString()
             .isEmpty()) {
      boundary = index;
      break;
    }
  // Match the retained context boundary used by fresh CLI text replay.
  // Earlier rows remain saved for review, but no longer grant replay inputs.
  for (int sequence = 0; sequence < history.size() - boundary; ++sequence) {
    const int index =
        sequence == 0 ? history.size() - 1 : boundary + sequence - 1;
    const auto row = history[index].toObject();
    const bool current = index == history.size() - 1;
    const auto metadata = row["metadata"].toObject();
    if (metadata.contains("attachments") && !metadata["attachments"].isArray())
      return fail("Attachment references are invalid.");
    const auto references = metadata["attachments"].toArray();
    const auto hashes = metadata["pathReferences"].toObject();
    if (current && references.size() > 32)
      return fail("At most 32 attachments can be handed off per turn.");
    if ((referencesCount += references.size()) > 500)
      return fail("Canonical attachment references exceed 500 entries.");
    for (const auto &value : references) {
      if (stopped())
        return fail("Attachment preparation was cancelled.");
      QString error;
      const auto reference = AttachmentRef::fromJson(value.toObject(), &error);
      if (!error.isEmpty())
        return fail(error);
      const auto expected = hashes[reference.path].toString();
      if (expected.isEmpty())
        return fail("This attachment has no captured original; attach it again "
                    "before using an external runtime.");
      (current ? currentKeys : historyKeys).append({reference.path, expected});
      const auto key = reference.path + QChar::Null + expected;
      if (captured.contains(key))
        continue;
      captured.insert(key);
      const auto durable =
          handoff.readCaptured(reference.path, expected, &error);
      if (!error.isEmpty())
        return fail(error);
      QByteArray bytes;
      if (durable)
        bytes = *durable;
      else {
        bytes = store.readSnapshot(owner, reference, expected, &error);
        if (!error.isEmpty())
          bytes = store.read(owner, reference, mounts, &error,
                             HarnessAttachmentStore::MaxFileBytes);
        if (!error.isEmpty())
          return fail("The captured attachment is unavailable: " + error);
      }
      if (QString::fromLatin1(
              QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                  .toHex()) != expected)
        return fail("The original attachment changed and its captured bytes "
                    "are unavailable.");
      if ((aggregate += bytes.size()) > HarnessAttachmentStore::MaxTurnBytes)
        return fail(
            "Attachment preparation exceeds 256 MiB across canonical history.");
      auto name = reference.originalFileName;
      if (name.isEmpty())
        name = QFileInfo(reference.path).fileName();
      if (!durable && !handoff
                           .prepare({{reference.path, name, expected, bytes}},
                                    cancelled, &error)
                           .valid())
        return fail(error);
      if (!current)
        continue;
      QBuffer buffer(&bytes);
      buffer.open(QIODevice::ReadOnly);
      QImageReader reader(&buffer);
      const bool raster =
          QStringList{"png", "jpg", "jpeg", "gif", "webp", "bmp", "avif", "ico"}
              .contains(QFileInfo(name).suffix().toLower());
      if (raster && reader.canRead()) {
        const auto prepared =
            AttachmentPipeline::prepareImage(bytes, reference, media, &error);
        if (!error.isEmpty())
          return fail(error);
        QJsonArray parts;
        bool inlineImage = false;
        for (const auto &part : prepared) {
          const auto p = part.toObject();
          if (p["type"] == "image") {
            const auto mime = p["mediaType"].toString(),
                       data = p["dataBase64"].toString();
            parts.append(
                QJsonObject{{"type", "image"},
                            {"url", "data:" + mime + ";base64," + data}});
            inlineImage = true;
          } else if (p["type"] == "text")
            parts.append(p);
        }
        auto candidate = out.input;
        for (const auto &part : parts)
          candidate.append(part);
        if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() <=
            3 * 1024 * 1024) {
          out.input = candidate;
          if (inlineImage)
            out.attachments.inlineReferences.append(reference.path);
        }
        // Otherwise the adapter receives the original file descriptor instead.
      } else {
        FileDescriptor descriptor{reference.path, name, bytes.size(), {}};
        FileReadParams params;
        if (!reference.path.startsWith("att/"))
          params.preview = true;
        const auto transformed = FileTransformer::fromBytes(
            bytes, descriptor, params, {}, cancelled);
        if (stopped())
          return fail("Attachment preparation was cancelled.");
        QStringDecoder decoder(QStringDecoder::Utf8,
                               QStringConverter::Flag::Stateless |
                                   QStringConverter::Flag::ConvertInitialBom);
        const QString decoded = decoder(bytes);
        Q_UNUSED(decoded)
        const bool binary = bytes.contains('\0') || decoder.hasError();
        const auto format = transformed.metadata["format"].toString();
        // Preserve supported structured/text previews. Binary fallback
        // instructions describe native sandbox APIs and must never be offered
        // to this runtime.
        if (transformed.error.isEmpty() && format != "binary" &&
            (!binary || !format.isEmpty()))
          for (const auto &part :
               FileTransformer::wrappedParts(descriptor, transformed))
            out.input.append(part);
      }
    }
  }
  if (stopped())
    return fail("Attachment preparation was cancelled.");
  QString error;
  out.attachments.current = handoff.selectKeys(currentKeys, &error);
  if (!out.attachments.current.valid())
    return fail(error);
  out.attachments.history = handoff.selectKeys(historyKeys, &error);
  if (!out.attachments.history.valid())
    return fail(error);
  return out;
}
} // namespace ade
