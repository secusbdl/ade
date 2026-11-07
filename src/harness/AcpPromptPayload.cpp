#include "AcpPromptPayload.h"
#include <QJsonDocument>
namespace ade {
namespace {
QByteArray json(const QJsonObject &value) {
  return QJsonDocument(value).toJson(QJsonDocument::Compact);
}
}
AcpPromptPayload prepareAcpPrompt(const QString &sessionId,
    const QJsonArray &prefix, const QJsonArray &input,
    const HarnessAttachments &attachments, bool images, bool newSession,
    const std::atomic_bool *cancelled, QString *error) {
  if (error) error->clear();
  const auto fail = [&](const QString &message) {
    if (error) *error = message;
    return AcpPromptPayload{};
  };
  const auto stopped = [&] { return cancelled && cancelled->load(); };
  if (stopped()) return fail("ACP image preparation was cancelled.");
  if (sessionId.isEmpty() || sessionId.size() > 4096 || !attachments.acpOriginalImages)
    return fail("ACP original-image input requires a bound native policy and session.");
  QString failure;
  // Baseline includes every fallback descriptor and all non-media content.
  // This is checked before allocating or encoding any original image.
  auto parts = prefix;
  const auto fallback = mapHarnessAttachments(input, attachments, true,
                                              images, newSession, &failure);
  if (!failure.isEmpty()) return fail(failure);
  for (const auto &part : fallback) parts.append(part);
  const auto empty = json({{"sessionId",sessionId},{"prompt",QJsonArray{}}});
  const auto marker = QByteArray("\"prompt\":[]");
  const auto position = empty.indexOf(marker);
  if (position < 0) return fail("ACP prompt serialization failed.");
  const auto head = empty.left(position) + "\"prompt\":[";
  const auto tail = QByteArray("]") + empty.mid(position + marker.size());
  QList<QByteArray> encoded;
  qsizetype total = head.size() + tail.size();
  for (const auto &part : parts) {
    if (!part.isObject()) return fail("ACP prompt part is invalid.");
    auto bytes = json(part.toObject());
    total += bytes.size() + (encoded.isEmpty() ? 0 : 1);
    if (total > AcpPromptPayload::MaxFallbackBytes)
      return fail("ACP non-image content and fallback descriptors exceed 4 MiB.");
    encoded.append(std::move(bytes));
  }
  // Fixed framing reserve covers the UUID and exact JSON-RPC envelope. The
  // transport independently checks the actual final complete frame.
  constexpr qsizetype maximum = AcpPromptPayload::MaxFrameBytes - 512;
  const auto links = attachments.current.empty() ? QJsonArray{} :
      attachments.current.resourceLinks(&failure);
  if (!failure.isEmpty()) return fail(failure);
  const qsizetype currentOffset = prefix.size() + input.size();
  if (!attachments.inlineReferences.isEmpty())
    return fail("Original ACP inputs cannot contain converted image references.");
  struct Original { qsizetype leaseIndex; QByteArray header; qsizetype encodedSize; };
  QHash<qsizetype, Original> originals;
  for (qsizetype i = 0; i < links.size(); ++i) {
    if (stopped()) return fail("ACP image preparation was cancelled.");
    const auto link = links[i].toObject();
    const auto mime = link["mimeType"].toString();
    const qint64 size = link["size"].toInteger(-1);
    if (!images || mime.isEmpty() || size < 0 ||
        size > HarnessAttachmentLease::MaxAcpImageBytes) continue;
    const auto slot = currentOffset + i;
    if (slot >= encoded.size()) return fail("ACP attachment mapping is inconsistent.");
    const auto imageHead = QByteArray("{\"type\":\"image\",\"mimeType\":") +
        QJsonDocument(QJsonArray{mime}).toJson(QJsonDocument::Compact).mid(1).chopped(1) +
        ",\"data\":\"";
    const qsizetype expanded = imageHead.size() + ((size + 2) / 3) * 4 + 2;
    const qsizetype delta = expanded - encoded[slot].size();
    if (delta > maximum - total) continue; // Explicit ordered file fallback.
    originals.insert(slot,{i,imageHead,expanded});
    total += delta;
  }
  if (stopped()) return fail("ACP image preparation was cancelled.");
  if (total > maximum) return fail("ACP prompt exceeds its 64 MiB envelope.");
  AcpPromptPayload result;
  result.m_attachments = attachments;
  result.m_parameters.reserve(total);
  result.m_parameters += head;
  for (qsizetype i=0;i<encoded.size();++i) {
    if (stopped()) return fail("ACP image preparation was cancelled.");
    if (i) result.m_parameters += ',';
    const auto original = originals.constFind(i);
    if (original == originals.cend()) result.m_parameters += encoded[i];
    else {
      auto bytes = attachments.current.originalAcpImage(original->leaseIndex,cancelled,&failure);
      if (!failure.isEmpty()) return fail(failure);
      if (!bytes) return fail("The captured ACP image became unavailable.");
      const auto start = result.m_parameters.size();
      result.m_parameters += original->header;
      // One <=20 MiB raw file and one small base64 block coexist with the
      // <=64 MiB final payload. No full-size UTF-16/base64 intermediate exists.
      constexpr qsizetype block = 3 * 65536;
      for (qsizetype at=0;at<bytes->size();at+=block) {
        if (stopped()) return fail("ACP image preparation was cancelled.");
        result.m_parameters += QByteArrayView(*bytes).sliced(at,qMin(block,bytes->size()-at)).toByteArray().toBase64();
      }
      result.m_parameters += "\"}";
      if (result.m_parameters.size()-start != original->encodedSize)
        return fail("ACP image encoding size mismatch.");
    }
    encoded[i].clear();
  }
  result.m_parameters += tail;
  if (result.m_parameters.size() != total) return fail("ACP final envelope size mismatch.");
  for (const auto &lease : {attachments.current, attachments.history})
    if (!lease.empty() && !lease.valid()) return fail("Captured attachment lease was revoked.");
  return result;
}
} // namespace ade
