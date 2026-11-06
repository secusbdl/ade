#pragma once
#include "AttachmentPipeline.h"

namespace ade {
class ModelMediaCatalog {
public:
    // Exact pinned model IDs/aliases only. Unknown custom models are text-only,
    // matching upstream getModelCapabilities; explicit future catalog metadata can extend this.
    static ModelMediaCapabilities resolve(const QString &modelId);
    static QStringList modelIds();
};
}
