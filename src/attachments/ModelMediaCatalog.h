#pragma once
#include "AttachmentPipeline.h"

namespace ade {
struct ProviderConfig;
class ModelMediaCatalog {
public:
    // Exact pinned model IDs/aliases only. Unknown custom models are text-only,
    // matching upstream getModelCapabilities; explicit future catalog metadata can extend this.
    static ModelMediaCapabilities resolve(const QString &modelId);
    static ModelMediaCapabilities resolve(const ProviderConfig& provider);
    static QStringList modelIds();
};
}
