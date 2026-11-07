#include "HarnessPreparedTurn.h"
#include <QJsonDocument>
#include <QJsonObject>
namespace ade {
QJsonArray mapHarnessAttachments(const QJsonArray &input,
                                 const HarnessAttachments &attachments,
                                 bool acp, bool images, bool newSession,
                                 QString *error) {
  if (error)
    error->clear();
  auto fail = [&](const QString &s) {
    if (error)
      *error = s;
    return QJsonArray{};
  };
  for (const auto &lease : {attachments.current, attachments.history})
    if (!lease.empty() && !lease.valid())
      return fail("Captured attachment lease was revoked.");
  if (!attachments.current.empty() && !attachments.history.empty() &&
      attachments.current.owner() != attachments.history.owner())
    return fail("Attachment manifests belong to different chats.");
  QJsonArray output;
  for (const auto &part : input) {
    const auto object = part.toObject();
    if (object["type"] == "image" && !images) {
      if (attachments.inlineReferences.isEmpty() || attachments.current.empty())
        return fail("This runtime cannot receive the prepared inline image.");
      continue;
    }
    output.append(part);
  }
  auto append = [&](const HarnessAttachmentLease &lease) -> bool {
    if (lease.empty())
      return true;
    QString failure;
    if (acp)
      for (const auto &part : lease.resourceLinks(&failure))
        output.append(part);
    else {
      const auto text = lease.textDescriptors(&failure);
      if (!text.isEmpty())
        output.append(QJsonObject{{"type", "text"}, {"text", text}});
    }
    if (!failure.isEmpty()) {
      if (error)
        *error = failure;
      return false;
    }
    return true;
  };
  const auto current =
      images ? attachments.current.excluding(attachments.inlineReferences)
             : attachments.current;
  if (!append(current))
    return {};
  if (newSession) {
    const auto previous = attachments.history.excluding(attachments.current);
    if (!append(previous))
      return {};
  }
  if (QJsonDocument(output).toJson(QJsonDocument::Compact).size() >
      4 * 1024 * 1024)
    return fail("Prepared external input exceeds its4 MiB transport bound.");
  return output;
}
} // namespace ade
