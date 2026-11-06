#include "ProviderUsage.h"
#include <cmath>

namespace ade {
namespace {
bool valid(const QJsonValue &value) {
  const double n = value.toDouble(-1);
  return value.isDouble() && std::isfinite(n) && n >= 0 &&
         n <= ProviderUsage::MaximumTokens && std::floor(n) == n;
}
} // namespace
void ProviderUsage::take(const QJsonObject &source, const char *key,
                         const char *target) {
  if (valid(source.value(key)))
    m_values[target] = source.value(key);
}
void ProviderUsage::consume(const QString &protocol, const QJsonObject &event) {
  if (m_protocol != protocol)
    m_values = {};
  m_protocol = protocol;
  if (protocol != "openai" && protocol != "responses" &&
      protocol != "anthropic" && protocol != "google")
    return;
  if (protocol == "anthropic") {
    const auto usage = event["type"] == "message_start"
                           ? event["message"].toObject()["usage"].toObject()
                           : event["usage"].toObject();
    take(usage, "input_tokens", "uncachedInputTokens");
    take(usage, "cache_read_input_tokens", "cachedInputTokens");
    take(usage, "cache_creation_input_tokens", "cacheWriteTokens");
    take(usage, "output_tokens", "outputTokens");
  } else if (protocol == "google") {
    const auto usage = event["usageMetadata"].toObject();
    take(usage, "promptTokenCount", "inputTokens");
    take(usage, "candidatesTokenCount", "candidateTokens");
    take(usage, "thoughtsTokenCount", "reasoningTokens");
    take(usage, "cachedContentTokenCount", "cachedInputTokens");
    take(usage, "totalTokenCount", "totalTokens");
  } else {
    const auto response =
        protocol == "responses" && event["response"].isObject()
            ? event["response"].toObject()
            : event;
    const auto usage = response["usage"].toObject();
    const bool responses = protocol == "responses";
    take(usage, responses ? "input_tokens" : "prompt_tokens", "inputTokens");
    take(usage, responses ? "output_tokens" : "completion_tokens",
         "outputTokens");
    take(usage, "total_tokens", "totalTokens");
    take(usage[responses ? "input_tokens_details" : "prompt_tokens_details"]
             .toObject(),
         "cached_tokens", "cachedInputTokens");
    take(
        usage[responses ? "output_tokens_details" : "completion_tokens_details"]
            .toObject(),
        "reasoning_tokens", "reasoningTokens");
  }
}
QJsonObject ProviderUsage::counters() const {
  auto result = m_values;
  if (m_protocol == "anthropic" && result.contains("uncachedInputTokens")) {
    // Anthropic's input_tokens excludes cache reads and cache creation.
    const qint64 input = result["uncachedInputTokens"].toInteger() +
                         result["cachedInputTokens"].toInteger() +
                         result["cacheWriteTokens"].toInteger();
    if (input <= MaximumTokens)
      result["inputTokens"] = input;
  }
  if (m_protocol == "google" && result.contains("candidateTokens")) {
    const qint64 output = result["candidateTokens"].toInteger() +
                          result["reasoningTokens"].toInteger();
    if (output <= MaximumTokens)
      result["outputTokens"] = output;
  }
  if (!result.contains("totalTokens") && result.contains("inputTokens") &&
      result.contains("outputTokens")) {
    const qint64 total =
        result["inputTokens"].toInteger() + result["outputTokens"].toInteger();
    if (total <= MaximumTokens)
      result["totalTokens"] = total;
  }
  return result;
}
bool ProviderUsage::validCounters(const QJsonObject &counters) {
  if (counters.isEmpty() || counters.size() > 9)
    return false;
  const QStringList allowed{"inputTokens",         "outputTokens",
                            "totalTokens",         "cachedInputTokens",
                            "cacheWriteTokens",    "reasoningTokens",
                            "uncachedInputTokens", "candidateTokens"};
  for (auto i = counters.begin(); i != counters.end(); ++i)
    if (!allowed.contains(i.key()) || !valid(i.value()))
      return false;
  return true;
}
} // namespace ade
