#pragma once
#include "attachments/AttachmentPipeline.h"
#include "HarnessAttachmentSource.h"
#include "harness/HarnessPreparedTurn.h"
namespace ade {
// Value-only worker entry; all owner/mount bindings are copied by the host
// before dispatch. No QObject or external CLI state is accessed here.
HarnessPreparedTurn prepareHarnessInput(const HarnessAttachmentStore &handoff,
                                        const HarnessAttachmentSource &source,
                                        const QJsonArray &history,
                                        const ModelMediaCapabilities &media,
                                        const std::atomic_bool *cancelled,
                                        bool acpOriginalImages = false);
} // namespace ade
