#pragma once
#include <QJsonObject>
#include <QString>

namespace ade::providerModelRules {
// Transport rules from the pinned Stagewise 1.31.1 bundled AI SDK. These are
// serialization defaults, not a live model catalog or account entitlement.
struct OpenAiTraits {
  bool reasoning, nonReasoning, flex, priority;
};
inline OpenAiTraits openAi(const QString &m, const QJsonObject &options) {
  const bool gpt5 = m.startsWith("gpt-5") && !m.startsWith("gpt-5-chat");
  const bool o = m.startsWith("o3") || m.startsWith("o4-mini");
  bool nonReasoning = false;
  for (const auto &v :
       {"gpt-5.1", "gpt-5.2", "gpt-5.3", "gpt-5.4", "gpt-5.5", "gpt-5.6"})
    nonReasoning |= m.startsWith(v);
  return {
      options.value("forceReasoning").toBool(m.startsWith("o1") || o || gpt5),
      nonReasoning, o || gpt5,
      m.startsWith("gpt-4") || o ||
          (gpt5 && !m.startsWith("gpt-5-nano") &&
           !m.startsWith("gpt-5.4-nano"))};
}
inline int anthropicLimit(const QString &m) {
  if (m.contains("claude-opus-4-7") || m.contains("claude-opus-4-6") ||
      m.contains("claude-sonnet-4-6"))
    return 128000;
  if (m.contains("claude-sonnet-4-5") || m.contains("claude-opus-4-5") ||
      m.contains("claude-haiku-4-5"))
    return 64000;
  if (m.contains("claude-opus-4-1"))
    return 32000;
  if (m.contains("claude-sonnet-4-"))
    return 64000;
  if (m.contains("claude-opus-4-"))
    return 32000;
  if (m.contains("claude-3-haiku"))
    return 4096;
  return 0; // No guessed limit for an unknown model ID.
}

inline qint64 anthropicOutputTokens(const QString &wireModel, int output,
                                    int budget) {
  const int limit = anthropicLimit(wireModel);
  const qint64 requested = output > 0 ? output : limit > 0 ? limit : 4096;
  const qint64 total = requested + qint64(budget);
  return limit > 0 ? qMin(qint64(limit), total) : total;
}
inline bool permitsTemperature(const QString &protocol, const QString &model,
                               const QString &thinking, const QString &effort) {
  if (protocol == "anthropic") {
    auto wireModel = model;
    wireModel.replace('.', '-');
    return thinking != "enabled" && thinking != "adaptive" &&
           !wireModel.contains("claude-opus-4-7");
  }
  if (protocol == "openai" || protocol == "responses") {
    const auto traits = openAi(model, {});
    if (traits.reasoning)
      return traits.nonReasoning && effort == "none";
    if (protocol == "openai" &&
        (model.startsWith("gpt-4o-search-preview") ||
         model.startsWith("gpt-4o-mini-search-preview")))
      return false;
  }
  return true;
}
} // namespace ade::providerModelRules
