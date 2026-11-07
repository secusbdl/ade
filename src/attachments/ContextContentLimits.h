#pragma once
#include "AttachmentPipeline.h"
#include "core/ProviderConfig.h"
namespace ade {
// Pure per-request limits. Context tokens are used only to derive a soft
// UTF-16 character budget; neither that estimate nor the output is tokenized.
class ContextContentLimits {
public:
    static int pinnedContextWindow(const QString& exactModelId);
    static int contextWindow(const ProviderConfig& activeProvider);
    static AttachmentLimits forProvider(const ProviderConfig& activeProvider);
};
}
