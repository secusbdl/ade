#include "ModelMediaCatalog.h"
namespace ade {
namespace {
#include "ModelMediaEntries.inc"
}
ModelMediaCapabilities ModelMediaCatalog::resolve(const QString &requestedId) {
    auto id = requestedId;
    if (id.startsWith("models/")) id.remove(0, 7);
    if (id == "default") id = "deepseek-v4-pro";
    else if (id == "quick") id = "gemini-3.5-flash";
    else if (id == "smart") id = "glm-5.2";
    ModelMediaCapabilities result;
    for (const auto &entry : Entries) if (id == QLatin1String(entry.id)) {
        result.image = entry.image;
        if (!entry.image) return result;
        result.imageMimeTypes = {"image/jpeg", "image/png", "image/gif", "image/webp"};
        switch (entry.family) {
        case Family::Anthropic: result.maxWidth = result.maxHeight = 8000; result.maxPixels = 1200000; break;
        case Family::OpenAi: result.maxWidth = result.maxHeight = 2048; result.maxPixels = 1572864; break;
        case Family::Gpt54: result.maxWidth = result.maxHeight = 2048; result.maxPixels = 2560000; break;
        case Family::Google:
            // Upstream permits 100 MiB; this native slice applies a declared 20 MiB request ceiling.
            result.maxImageBytes = 20 * 1024 * 1024; result.maxWidth = result.maxHeight = 4096; result.maxPixels = 2500000; break;
        case Family::TenMiB: result.maxImageBytes = 10 * 1024 * 1024; break;
        case Family::Native: break; // Source declares image input without numeric constraints; use native bounds.
        }
        return result;
    }
    return result;
}
QStringList ModelMediaCatalog::modelIds() { QStringList ids; for (const auto &entry : Entries) ids.append(QLatin1String(entry.id)); return ids; }
}
