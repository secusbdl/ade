#pragma once
#include "attachments/AttachmentStore.h"
#include "harness/HarnessWorkspace.h"
#include <atomic>
namespace ade {
enum class HarnessAttachmentAvailability { Available, Missing, Invalid };
struct HarnessAttachmentRead {
  HarnessAttachmentAvailability status = HarnessAttachmentAvailability::Invalid;
  QByteArray bytes;
  QString error;
};
// Host-captured physical roots remain pinned throughout preparation. Missing is
// returned only for a missing descendant of a validated existing directory.
class HarnessAttachmentSource {
public:
  HarnessAttachmentSource(const QString &physicalProfile, const QString &owner,
                          const AttachmentMounts &mounts);
  HarnessAttachmentRead snapshot(const AttachmentRef &, const QString &hash,
                                 const std::atomic_bool *) const;
  HarnessAttachmentRead original(const AttachmentRef &,
                                 const std::atomic_bool *) const;

private:
  HarnessWorkspace m_profile;
  QMap<QString, HarnessWorkspace> m_mounts;
  QString m_owner;
};
} // namespace ade
