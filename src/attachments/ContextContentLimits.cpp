#include "ContextContentLimits.h"
#include "core/ModelDiscoveryPolicy.h"
namespace ade {
namespace {
#include "ModelContextEntries.inc"
constexpr int MaximumContext = 100000000;
}
int ContextContentLimits::pinnedContextWindow(const QString& exactModelId) {
    QString id = exactModelId;
    for (const auto& alias : ContextAliases)
        if (id == QLatin1String(alias.id)) { id = QLatin1String(alias.target); break; }
    for (const auto& entry : ContextEntries)
        if (id == QLatin1String(entry.id)) return entry.tokens;
    return 0;
}
int ContextContentLimits::contextWindow(const ProviderConfig& provider) {
    // External coding agents own their own context and original-file handling.
    if (provider.kind != "api") return 0;
    if (provider.contextWindowTokens > 0 && provider.contextWindowTokens <= MaximumContext)
        return provider.contextWindowTokens;
    if (provider.contextWindowTokens != 0) return 0;
    // A custom route must carry its own resolved context. Never substitute a
    // catalog context for an unresolved or malformed custom runtime binding.
    if (!provider.customRequest.isEmpty()) return 0;
    const auto& snapshot = provider.discoverySnapshot;
    if (!snapshot.isEmpty() && modelDiscoveryMatches(provider, snapshot)
        && snapshot.value("complete").toBool() && validModelDiscoverySnapshot(provider, snapshot)) {
        for (const auto& value : snapshot.value("models").toArray()) {
            const auto model = value.toObject();
            if (model.value("id").toString() != provider.model) continue;
            const auto limit = model.value("inputTokenLimit");
            if (limit.isDouble() && limit.toDouble() == limit.toInt()
                && limit.toInt() > 0 && limit.toInt() <= MaximumContext)
                return limit.toInt();
        }
    }
    return pinnedContextWindow(provider.model);
}
AttachmentLimits ContextContentLimits::forProvider(const ProviderConfig& provider) {
    AttachmentLimits limits;
    const int context = contextWindow(provider);
    if (context == 0) return limits; // Preserve existing unknown-model behavior.
    // Source: max(4000, floor(tokens * 0.1 * 4)). Integer arithmetic avoids
    // overflow and floating rounding without claiming measured token usage.
    limits.maxReadChars = int(qMin<qint64>(4 * 1024 * 1024,
        qMax<qint64>(4000, qint64(context) * 2 / 5)));
    // Line numbers, the mandatory first long line and notices exceed the soft
    // content budget. Keep a separate existing native hard ceiling; truncation
    // already marks partialContent/unknown coverage in FileTransformer.
    limits.maxTextChars = 1024 * 1024;
    return limits;
}
}
