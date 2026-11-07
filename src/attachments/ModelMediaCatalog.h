#pragma once
#include "AttachmentPipeline.h"

namespace ade {
struct ProviderConfig;
class ModelMediaCatalog {
public:
    // Exact pinned model IDs/aliases only. Unknown custom models are text-only,
    // matching upstream getModelCapabilities. The ProviderConfig overload also
    // honors a validated, exact-bound custom image declaration with native bounds.
    static ModelMediaCapabilities resolve(const QString &modelId);
    static ModelMediaCapabilities resolve(const ProviderConfig& provider);
    static QStringList modelIds();
};
}
