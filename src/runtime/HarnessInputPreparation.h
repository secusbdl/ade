#pragma once
#include "attachments/AttachmentPipeline.h"
#include "attachments/AttachmentStore.h"
#include "harness/HarnessPreparedTurn.h"
namespace ade {
// Value-only worker entry; all owner/mount bindings are copied by the host
// before dispatch. No QObject or external CLI state is accessed here.
HarnessPreparedTurn prepareHarnessInput(const HarnessAttachmentStore &handoff,
                                        const AttachmentStore &store,
                                        const QString &owner,
                                        const QJsonArray &history,
                                        const AttachmentMounts &mounts,
                                        const ModelMediaCapabilities &media,
                                        const std::atomic_bool *cancelled);
} // namespace ade
