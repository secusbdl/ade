#pragma once
#include "ProviderConfig.h"
#include <QList>

namespace ade {
// A resolved instance/model entry. The host resolves its settings without exposing keys in signals.
struct ModelConfig {
    ProviderConfig provider;
    bool hasThinkingOverride = false;
};
}
