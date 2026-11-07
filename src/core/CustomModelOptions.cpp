#include "CustomModelOptions.h"
#include "ProviderModelRules.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace ade {
namespace {
QString kind(QString value) {
  if (value == "responses" || value == "openai-responses")
    return "responses";
  if (value == "google" || value == "gemini")
    return "google";
  if (value == "anthropic")
    return value;
  if (QStringList{"openai", "openai-compatible", "chat-completions",
                  "openai-chat-completions"}
          .contains(value))
    return "openai";
  return {};
}
bool fail(QString *error, const QString &field, const QString &issue) {
  // Unknown property names are untrusted private data, too. Never echo a
  // long/encoded value smuggled into an object key as a diagnostic.
  static const QRegularExpression safe("^[A-Za-z][A-Za-z0-9_.]{0,79}$");
  const auto name =
      safe.match(field).hasMatch() ? field : QString("custom configuration");
  if (error)
    *error = name + ": " + issue;
  return false;
}
bool keys(const QJsonObject &object, const QStringList &allowed,
          const QString &path, QString *error) {
  for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    if (!allowed.contains(it.key()))
      return fail(error, path, "contains an unsupported field.");
  return true;
}
bool text(const QJsonValue &v, int maximum = 4096) {
  return v.isString() && v.toString().size() <= maximum &&
         !v.toString().contains(QChar::Null);
}
bool integer(const QJsonValue &v, int minimum, int maximum) {
  return v.isDouble() && std::isfinite(v.toDouble()) &&
         std::floor(v.toDouble()) == v.toDouble() && v.toDouble() >= minimum &&
         v.toDouble() <= maximum;
}
bool choice(const QJsonValue &v, const QStringList &allowed) {
  return v.isString() && allowed.contains(v.toString());
}
bool optionalChoice(const QJsonObject &o, const QString &key,
                    const QStringList &allowed) {
  return !o.contains(key) || choice(o.value(key), allowed);
}
bool bounded(const QJsonValue &value, int depth, int &nodes) {
  if (depth > 12 || ++nodes > 8192)
    return false;
  if (value.isString())
    return text(value, 65536);
  if (value.isDouble())
    return std::isfinite(value.toDouble());
  if (value.isArray())
    for (const auto &item : value.toArray())
      if (!bounded(item, depth + 1, nodes))
        return false;
  if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      if (it.key().size() > 512 || it.key().contains(QChar::Null) ||
          !bounded(it.value(), depth + 1, nodes))
        return false;
  }
  return !value.isUndefined();
}
bool headerMap(const QJsonObject &headers, QString *error) {
  if (headers.size() > 64)
    return fail(error, "headers", "at most 64 headers are supported.");
  const QRegularExpression token("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$");
  const QSet<QString> forbidden{"host",
                                "content-length",
                                "transfer-encoding",
                                "connection",
                                "keep-alive",
                                "upgrade",
                                "te",
                                "trailer",
                                "proxy-authorization",
                                "proxy-authenticate"};
  QSet<QString> seen;
  int size = 0;
  for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
    const auto name = it.key().toLower();
    if (name.size() > 256 || !token.match(name).hasMatch() ||
        forbidden.contains(name) || name.startsWith("proxy-"))
      return fail(error, "headers",
                  "invalid or unsupported transport-control header name.");
    if (seen.contains(name))
      return fail(error, "headers",
                  "case-insensitive duplicate names are ambiguous.");
    seen.insert(name);
    if (!it.value().isString() || it.value().toString().size() > 8192)
      return fail(error, "headers", "values must be bounded strings.");
    const auto value = it.value().toString();
    for (const auto c : value)
      if (c.unicode() < 32 || c.unicode() == 127 || c.unicode() > 255)
        return fail(error, "headers",
                    "values must be Latin-1 text without control characters.");
    size += name.size() + value.size();
    if (size > 32768)
      return fail(error, "headers", "aggregate size exceeds 32 KiB.");
    if (name == "content-type" &&
        value.trimmed().compare("application/json", Qt::CaseInsensitive))
      return fail(error, "headers.ContentType",
                  "native requests require application/json.");
  }
  return true;
}
bool validOptions(const QString &protocol, const QJsonObject &options,
                  QString *error) {
  int nodes = 0;
  if (QJsonDocument(options).toJson(QJsonDocument::Compact).size() > 65536 ||
      !bounded(options, 0, nodes))
    return fail(error, "providerOptions",
                "size or nesting exceeds native bounds.");
  QStringList allowed;
  if (protocol == "openai" || protocol == "responses") {
    allowed = {"parallelToolCalls",
               "user",
               "store",
               "metadata",
               "reasoningEffort",
               "serviceTier",
               "textVerbosity",
               "promptCacheKey",
               "promptCacheOptions",
               "promptCacheRetention",
               "safetyIdentifier",
               "forceReasoning"};
    allowed += protocol == "openai"
                   ? QStringList{"maxCompletionTokens", "prediction"}
                   : QStringList{"reasoningSummary", "reasoningMode",
                                 "reasoningContext", "include",
                                 "truncation",       "maxToolCalls"};
  } else if (protocol == "anthropic")
    allowed = {"thinking",
               "effort",
               "metadata",
               "cacheControl",
               "disableParallelToolUse",
               "anthropicBeta",
               "speed",
               "inferenceGeo",
               "toolStreaming"};
  else if (protocol == "google")
    allowed = {"thinkingConfig", "mediaResolution", "safetySettings",
               "serviceTier", "responseModalities"};
  else
    return fail(error, "protocol",
                "custom HTTP options are not supported for this protocol.");
  if (!keys(options, allowed, "providerOptions", error))
    return false;
  for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
    const auto key = it.key();
    const auto value = it.value();
    bool valid = true;
    // The Responses SDK declares these options nullish. Preserve omission.
    if (protocol == "responses" && value.isNull() && key != "forceReasoning")
      continue;
    if (QStringList{"parallelToolCalls", "store", "forceReasoning",
                    "disableParallelToolUse", "toolStreaming"}
            .contains(key))
      valid = value.isBool();
    else if (QStringList{"user", "promptCacheKey", "safetyIdentifier",
                         "reasoningSummary"}
                 .contains(key))
      valid = text(value);
    else if (key == "reasoningEffort")
      valid = protocol == "responses"
                  ? text(value, 64)
                  : choice(value, {"none", "minimal", "low", "medium", "high",
                                   "xhigh", "max"});
    else if (key == "effort")
      valid = choice(value, {"low", "medium", "high", "xhigh", "max"});
    else if (key == "maxCompletionTokens")
      valid = integer(value, 1, 2000000);
    else if (key == "maxToolCalls")
      valid = integer(value, 1, 10000);
    else if (key == "serviceTier")
      valid = choice(value,
                     protocol == "google"
                         ? QStringList{"standard", "flex", "priority"}
                         : QStringList{"auto", "default", "flex", "priority"});
    else if (key == "textVerbosity")
      valid = choice(value, {"low", "medium", "high"});
    else if (key == "promptCacheRetention")
      valid = choice(value, {"in_memory", "24h"});
    else if (key == "reasoningMode")
      valid = choice(value, {"standard", "pro"});
    else if (key == "reasoningContext")
      valid = choice(value, {"auto", "current_turn", "all_turns"});
    else if (key == "truncation")
      valid = choice(value, {"auto", "disabled"});
    else if (key == "speed")
      valid = choice(value, {"fast", "standard"});
    else if (key == "inferenceGeo")
      valid = choice(value, {"us", "global"});
    else if (key == "mediaResolution")
      valid =
          choice(value, {"MEDIA_RESOLUTION_UNSPECIFIED", "MEDIA_RESOLUTION_LOW",
                         "MEDIA_RESOLUTION_MEDIUM", "MEDIA_RESOLUTION_HIGH"});
    else if (key == "metadata") {
      valid = value.isObject();
      const auto object = value.toObject();
      if (protocol == "anthropic")
        valid &= keys(object, {"userId"}, "providerOptions.metadata", error) &&
                 (!object.contains("userId") || text(object.value("userId")));
      else if (protocol == "openai")
        for (auto m = object.constBegin(); m != object.constEnd(); ++m)
          valid &= m.key().size() <= 64 && text(m.value(), 512);
      // Responses accepts arbitrary bounded metadata; it remains data.
    } else if (key == "promptCacheOptions") {
      const auto o = value.toObject();
      valid = value.isObject() &&
              keys(o, {"mode", "ttl"}, "providerOptions.promptCacheOptions",
                   error) &&
              optionalChoice(o, "mode", {"implicit", "explicit"}) &&
              optionalChoice(o, "ttl", {"30m"});
    } else if (key == "prediction") {
      const auto o = value.toObject();
      valid =
          value.isObject() &&
          keys(o, {"type", "content"}, "providerOptions.prediction", error) &&
          o.value("type") == "content" && text(o.value("content"), 60000);
    } else if (key == "thinking") {
      const auto o = value.toObject();
      const auto type = o.value("type").toString();
      const QStringList fields =
          type == "enabled"    ? QStringList{"type", "budgetTokens"}
          : type == "adaptive" ? QStringList{"type", "display"}
                               : QStringList{"type"};
      valid = value.isObject() &&
              choice(o.value("type"), {"enabled", "adaptive", "disabled"}) &&
              keys(o, fields, "providerOptions.thinking", error) &&
              (!o.contains("budgetTokens") ||
               integer(o.value("budgetTokens"), 1024, 2000000)) &&
              optionalChoice(o, "display", {"omitted", "summarized"});
    } else if (key == "cacheControl") {
      const auto o = value.toObject();
      valid = value.isObject() &&
              keys(o, {"type", "ttl"}, "providerOptions.cacheControl", error) &&
              o.value("type") == "ephemeral" &&
              optionalChoice(o, "ttl", {"5m", "1h"});
    } else if (key == "thinkingConfig") {
      const auto o = value.toObject();
      valid = value.isObject() &&
              keys(o, {"thinkingBudget", "includeThoughts", "thinkingLevel"},
                   "providerOptions.thinkingConfig", error) &&
              (!o.contains("thinkingBudget") ||
               integer(o.value("thinkingBudget"), -1, 2000000)) &&
              (!o.contains("includeThoughts") ||
               o.value("includeThoughts").isBool()) &&
              optionalChoice(o, "thinkingLevel",
                             {"minimal", "low", "medium", "high"});
    } else if (key == "include" || key == "responseModalities" ||
               key == "anthropicBeta") {
      valid = value.isArray() && value.toArray().size() <= 64;
      for (const auto &item : value.toArray()) {
        if (key == "include")
          valid &= item == "reasoning.encrypted_content";
        else if (key == "responseModalities")
          valid &= item == "TEXT";
        else
          valid &= text(item, 256) && QRegularExpression("^[A-Za-z0-9._-]+$")
                                          .match(item.toString())
                                          .hasMatch();
      }
    } else if (key == "safetySettings") {
      valid = value.isArray() && value.toArray().size() <= 6;
      QSet<QString> seen;
      for (const auto &item : value.toArray()) {
        const auto o = item.toObject();
        const auto category = o.value("category").toString();
        valid &=
            item.isObject() &&
            keys(o, {"category", "threshold"}, "providerOptions.safetySettings",
                 error) &&
            choice(o.value("category"),
                   {"HARM_CATEGORY_UNSPECIFIED", "HARM_CATEGORY_HATE_SPEECH",
                    "HARM_CATEGORY_DANGEROUS_CONTENT",
                    "HARM_CATEGORY_HARASSMENT",
                    "HARM_CATEGORY_SEXUALLY_EXPLICIT",
                    "HARM_CATEGORY_CIVIC_INTEGRITY"}) &&
            choice(o.value("threshold"),
                   {"HARM_BLOCK_THRESHOLD_UNSPECIFIED", "BLOCK_LOW_AND_ABOVE",
                    "BLOCK_MEDIUM_AND_ABOVE", "BLOCK_ONLY_HIGH", "BLOCK_NONE",
                    "OFF"}) &&
            !seen.contains(category);
        seen.insert(category);
      }
    }
    if (!valid) {
      if (error && !error->isEmpty())
        return false;
      return fail(error, "providerOptions." + key,
                  "invalid value or unsupported native variant.");
    }
  }
  return true;
}
bool boundRequest(const ProviderConfig &p, QString *error) {
  const auto c = p.customRequest;
  if (p.isExternalHarness() || p.id.isEmpty() || p.model.isEmpty() ||
      c.value("providerInstanceId") != p.id || c.value("modelId") != p.model)
    return fail(
        error, "customRequest",
        "definition does not match this HTTP provider instance and model.");
  if (!keys(c,
            {"providerInstanceId", "modelId", "providerOptions", "headers",
             "capabilities"},
            "customRequest", error) ||
      !c.value("providerOptions").isObject() ||
      !c.value("headers").isObject() || !c.value("capabilities").isObject())
    return fail(error, "customRequest", "invalid typed runtime binding.");
  const auto caps = c.value("capabilities").toObject();
  if (!validCustomModelCapabilities(caps, error))
    return false;
  const auto in = caps.value("inputModalities").toObject(),
             out = caps.value("outputModalities").toObject();
  if (!in.value("text").toBool() || !out.value("text").toBool())
    return fail(error, "capabilities",
                "native chat requires text input and output.");
  for (const auto &k : {"audio", "video", "file"})
    if (in.value(k).toBool())
      return fail(error, "capabilities.inputModalities." + QString(k),
                  "not supported by native HTTP chat.");
  for (const auto &k : {"image", "audio", "video", "file"})
    if (out.value(k).toBool())
      return fail(error, "capabilities.outputModalities." + QString(k),
                  "not supported by native HTTP chat.");
  return true;
}
void map(const QJsonObject &from, QJsonObject &to, const char *source,
         const char *destination) {
  if (from.contains(source) && !from.value(source).isNull())
    to[destination] = from.value(source);
}
void setNested(QJsonObject &body, const QString &field, const QString &key,
               const QJsonValue &value) {
  auto object = body.value(field).toObject();
  object[key] = value;
  body[field] = object;
}
bool overridesValid(const QJsonObject &overrides, QString *error) {
  if (!keys(overrides,
            {"maxOutputTokens", "temperature", "reasoningEffort",
             "thinkingMode", "thinkingBudget"},
            "requestOverrides", error))
    return false;
  for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
    bool ok = false;
    if (it.key() == "maxOutputTokens")
      ok = integer(it.value(), 0, 2000000);
    if (it.key() == "temperature")
      ok = it.value().isDouble() && std::isfinite(it.value().toDouble()) &&
           it.value().toDouble() >= 0 && it.value().toDouble() <= 2;
    if (it.key() == "reasoningEffort")
      ok = text(it.value(), 64);
    if (it.key() == "thinkingMode")
      ok = choice(it.value(), {"", "enabled", "disabled", "adaptive"});
    if (it.key() == "thinkingBudget")
      ok = integer(it.value(), 0, 2000000);
    if (!ok)
      return fail(error, "requestOverrides." + it.key(), "invalid value.");
  }
  return true;
}
void collectStrings(const QJsonValue &value, QSet<QString> &strings,
                    int &remaining, int depth = 0) {
  if (depth > 12 || remaining-- <= 0)
    return;
  if (value.isString() && !value.toString().isEmpty() &&
      value.toString().size() <= 65536)
    strings.insert(value.toString());
  else if (value.isArray()) {
    for (const auto &item : value.toArray()) {
      if (remaining <= 0)
        break;
      collectStrings(item, strings, remaining, depth + 1);
    }
  } else if (value.isObject()) {
    const auto object = value.toObject();
    for (auto it = object.constBegin();
         it != object.constEnd() && remaining > 0; ++it) {
      if (!it.key().isEmpty() && it.key().size() <= 512)
        strings.insert(it.key());
      collectStrings(it.value(), strings, remaining, depth + 1);
    }
  }
}

} // namespace

bool CustomModelOptions::validate(const QString &protocol,
                                  const QJsonObject &options,
                                  const QJsonObject &headers, QString *error) {
  if (error)
    error->clear();
  return headerMap(headers, error) &&
         validOptions(kind(protocol), options, error);
}
bool CustomModelOptions::allowsTools(const ProviderConfig &p) {
  return p.customRequest.isEmpty() ||
         (boundRequest(p, nullptr) && p.customRequest.value("capabilities")
                                          .toObject()
                                          .value("toolCalling")
                                          .toBool());
}
QString CustomModelOptions::redactDiagnostic(const ProviderConfig &p,
                                             QString value) {
  QSet<QString> secrets;
  int optionsBudget = 8192, headersBudget = 129;
  collectStrings(p.customRequest.value("providerOptions"), secrets,
                 optionsBudget);
  collectStrings(p.customRequest.value("headers"), secrets, headersBudget);
  if (!p.apiKey.isEmpty())
    secrets.insert(p.apiKey);
  auto sorted = secrets.values();
  std::sort(sorted.begin(), sorted.end(),
            [](const auto &a, const auto &b) { return a.size() > b.size(); });
  for (const auto &secret : sorted)
    value.replace(secret, "[REDACTED]");
  return value;
}
bool CustomModelOptions::apply(const ProviderConfig &p, QJsonObject *targetBody,
                               QNetworkRequest *targetRequest, QString *error) {
  if (error)
    error->clear();
  if (p.customRequest.isEmpty())
    return true;
  if (!targetBody || !targetRequest)
    return fail(error, "customRequest", "missing request destination.");
  if (!boundRequest(p, error) || !overridesValid(p.requestOverrides, error))
    return false;
  const auto protocol = kind(p.protocol);
  if ((protocol == "openai" || protocol == "responses") &&
      (p.requestOverrides.value("thinkingMode") == "enabled" ||
       p.requestOverrides.value("thinkingMode") == "adaptive" ||
       p.requestOverrides.value("thinkingBudget").toInt() > 0))
    return fail(error, "requestOverrides.thinkingMode",
                "this custom protocol requires reasoningEffort instead of "
                "enabled/adaptive thinking or a budget.");
  if (protocol == "google" &&
      p.requestOverrides.value("thinkingMode") == "adaptive")
    return fail(error, "requestOverrides.thinkingMode",
                "adaptive is not a Google SDK thinking mode; use "
                "thinkingConfig or reasoningEffort.");
  auto options = p.customRequest.value("providerOptions").toObject();
  const auto headers = p.customRequest.value("headers").toObject();
  if (!validate(p.protocol, options, headers, error))
    return false;
  for (auto it = options.begin(); it != options.end();)
    if (it.value().isNull())
      it = options.erase(it);
    else
      ++it;
  auto body = *targetBody;
  auto request = *targetRequest;
  const auto overrides = p.requestOverrides;
  if (overrides.contains("reasoningEffort")) {
    const auto key = protocol == "anthropic" ? "effort" : "reasoningEffort";
    if (overrides.value("reasoningEffort").toString().isEmpty())
      options.remove(key);
    else
      options[key] = overrides.value("reasoningEffort");
  }
  if (protocol == "anthropic" && (overrides.contains("thinkingMode") ||
                                  overrides.contains("thinkingBudget"))) {
    auto thinking = options.value("thinking").toObject();
    if (overrides.contains("thinkingMode"))
      thinking = QJsonObject{{"type", overrides.value("thinkingMode")}};
    if (thinking.value("type") == "enabled" &&
        overrides.value("thinkingBudget").toInt() > 0)
      thinking["budgetTokens"] = overrides.value("thinkingBudget");
    if (thinking.value("type").toString().isEmpty())
      options.remove("thinking");
    else
      options["thinking"] = thinking;
  } else if (protocol == "google") {
    auto thinking = options.value("thinkingConfig").toObject();
    if (overrides.value("thinkingMode") == "disabled") {
      thinking.remove("thinkingLevel");
      thinking["thinkingBudget"] = 0;
    } else if (overrides.value("thinkingBudget").toInt() > 0) {
      thinking.remove("thinkingLevel");
      thinking["thinkingBudget"] = overrides.value("thinkingBudget");
    }
    if (overrides.contains("reasoningEffort")) {
      thinking.remove("thinkingLevel");
      if (!overrides.value("reasoningEffort").toString().isEmpty()) {
        thinking.remove("thinkingBudget");
        thinking["thinkingLevel"] = overrides.value("reasoningEffort");
      }
      options.remove("reasoningEffort");
    }
    if (!thinking.isEmpty())
      options["thinkingConfig"] = thinking;
  }
  if ((protocol == "openai" || protocol == "responses") &&
      overrides.value("thinkingMode") == "disabled")
    options["reasoningEffort"] = "none";
  if (!validOptions(protocol, options, error))
    return false;
  const int output =
      overrides.value("maxOutputTokens").toInt(p.maxOutputTokens);
  if (overrides.contains("temperature")) {
    if (protocol == "google")
      setNested(body, "generationConfig", "temperature",
                overrides.value("temperature"));
    else
      body["temperature"] = overrides.value("temperature");
  }
  if (!allowsTools(p)) {
    body.remove("tools");
    body.remove("tool_choice");
    body.remove("toolConfig");
  }
  if (protocol == "openai" || protocol == "responses") {
    const auto traits = providerModelRules::openAi(p.model, options);
    for (const auto &pair :
         {qMakePair("user", "user"), qMakePair("store", "store"),
          qMakePair("metadata", "metadata"),
          qMakePair("parallelToolCalls", "parallel_tool_calls"),
          qMakePair("promptCacheKey", "prompt_cache_key"),
          qMakePair("promptCacheOptions", "prompt_cache_options"),
          qMakePair("promptCacheRetention", "prompt_cache_retention"),
          qMakePair("safetyIdentifier", "safety_identifier")})
      map(options, body, pair.first, pair.second);
    const auto tier = options.value("serviceTier").toString();
    if ((tier == "flex" && !traits.flex) ||
        (tier == "priority" && !traits.priority))
      return fail(error, "providerOptions.serviceTier",
                  "not supported by the pinned SDK model capability rules.");
    map(options, body, "serviceTier", "service_tier");
    body.remove("thinking");
    body.remove("reasoning_effort");
    body.remove("reasoning");
    if (protocol == "openai") {
      map(options, body, "reasoningEffort", "reasoning_effort");
      map(options, body, "prediction", "prediction");
      map(options, body, "textVerbosity", "verbosity");
      body.remove("max_tokens");
      body.remove("max_completion_tokens");
      if (output > 0)
        body[traits.reasoning ? "max_completion_tokens" : "max_tokens"] =
            output;
      if (options.contains("maxCompletionTokens")) {
        body.remove("max_tokens");
        body["max_completion_tokens"] = options.value("maxCompletionTokens");
      }
      if (overrides.contains("maxOutputTokens")) {
        body.remove("max_tokens");
        body.remove("max_completion_tokens");
        if (output > 0)
          body[traits.reasoning || options.contains("maxCompletionTokens")
                   ? "max_completion_tokens"
                   : "max_tokens"] = output;
      }
      if (traits.reasoning || providerModelRules::openAi(p.model, {}).reasoning) {
        auto messages = body.value("messages").toArray();
        for (int i = 0; i < messages.size(); ++i) {
          auto message = messages[i].toObject();
          if (message.value("role") == "system") {
            message["role"] = "developer";
            messages[i] = message;
          }
        }
        body["messages"] = messages;
      }
    } else {
      if (output > 0)
        body["max_output_tokens"] = output;
      else if (overrides.contains("maxOutputTokens"))
        body.remove("max_output_tokens");
      for (const auto &pair : {qMakePair("maxToolCalls", "max_tool_calls"),
                               qMakePair("truncation", "truncation")})
        map(options, body, pair.first, pair.second);
      if (options.contains("textVerbosity"))
        setNested(body, "text", "verbosity", options.value("textVerbosity"));
      QJsonObject reasoning;
      for (const auto &pair : {qMakePair("reasoningEffort", "effort"),
                               qMakePair("reasoningSummary", "summary"),
                               qMakePair("reasoningMode", "mode"),
                               qMakePair("reasoningContext", "context")})
        map(options, reasoning, pair.first, pair.second);
      if (!reasoning.isEmpty() && !traits.reasoning)
        return fail(error, "providerOptions.reasoning",
                    "unknown reasoning model; set forceReasoning explicitly or "
                    "remove reasoning options.");
      if (!reasoning.isEmpty())
        body["reasoning"] = reasoning;
      auto include = options.value("include").toArray();
      if (body.value("store") == false && traits.reasoning &&
          !include.contains("reasoning.encrypted_content"))
        include.append("reasoning.encrypted_content");
      if (include.isEmpty())
        body.remove("include");
      else
        body["include"] = include;
    }
    if (traits.reasoning &&
        !(options.value("reasoningEffort") == "none" && traits.nonReasoning)) {
      body.remove("temperature");
      body.remove("top_p");
      body.remove("logprobs");
    }
    if (traits.reasoning)
      for (const auto &key : {"frequency_penalty", "presence_penalty",
                              "logit_bias", "top_logprobs"})
        body.remove(key);
  } else if (protocol == "anthropic") {
    auto wireModel = p.model;
    wireModel.replace('.', '-');
    body["model"] = wireModel;
    body.remove("thinking");
    body.remove("output_config");
    const auto thinking = options.value("thinking").toObject();
    const auto type = thinking.value("type").toString();
    const int budget =
        type == "enabled" ? thinking.value("budgetTokens").toInt(1024) : 0;
    body["max_tokens"] = providerModelRules::anthropicOutputTokens(wireModel, output, budget);
    if (type == "enabled")
      body["thinking"] = QJsonObject{{"type", type}, {"budget_tokens", budget}};
    else if (type == "adaptive")
      body["thinking"] = thinking;
    if (type == "enabled" || type == "adaptive" ||
        wireModel.contains("claude-opus-4-7"))
      for (const auto &key : {"temperature", "top_p", "top_k"})
        body.remove(key);
    if (options.contains("effort"))
      setNested(body, "output_config", "effort", options.value("effort"));
    if (options.contains("metadata")) {
      QJsonObject metadata;
      map(options.value("metadata").toObject(), metadata, "userId", "user_id");
      body["metadata"] = metadata;
    }
    map(options, body, "cacheControl", "cache_control");
    map(options, body, "speed", "speed");
    map(options, body, "inferenceGeo", "inference_geo");
    if (!body.value("tools").toArray().isEmpty()) {
      if (options.contains("disableParallelToolUse"))
        body["tool_choice"] =
            QJsonObject{{"type", "auto"},
                        {"disable_parallel_tool_use",
                         options.value("disableParallelToolUse")}};
      if (body.value("stream").toBool() &&
          options.value("toolStreaming").toBool(true)) {
        auto tools = body.value("tools").toArray();
        for (int i = 0; i < tools.size(); ++i) {
          auto tool = tools[i].toObject();
          tool["eager_input_streaming"] = true;
          tools[i] = tool;
        }
        body["tools"] = tools;
      }
    }
  } else {
    auto generation = body.value("generationConfig").toObject();
    generation.remove("thinkingConfig");
    if (output > 0)
      generation["maxOutputTokens"] = output;
    else if (overrides.contains("maxOutputTokens"))
      generation.remove("maxOutputTokens");
    for (const auto &key :
         {"thinkingConfig", "mediaResolution", "responseModalities"})
      map(options, generation, key, key);
    if (!generation.isEmpty())
      body["generationConfig"] = generation;
    else
      body.remove("generationConfig");
    map(options, body, "safetySettings", "safetySettings");
    map(options, body, "serviceTier", "serviceTier");
  }
  for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
    request.setRawHeader(it.key().toLower().toLatin1(),
                         it.value().toString().toLatin1());
  if (protocol == "anthropic") {
    QStringList beta;
    for (const auto &part :
         QString::fromLatin1(request.rawHeader("anthropic-beta")).split(','))
      if (!part.trimmed().isEmpty() && !beta.contains(part.trimmed().toLower()))
        beta.append(part.trimmed().toLower());
    for (const auto &v : options.value("anthropicBeta").toArray())
      if (!beta.contains(v.toString()))
        beta.append(v.toString());
    if (options.value("speed") == "fast" &&
        !beta.contains("fast-mode-2026-02-01"))
      beta.append("fast-mode-2026-02-01");
    if (!beta.isEmpty())
      request.setRawHeader("anthropic-beta", beta.join(',').toLatin1());
  }
  *targetBody = body;
  *targetRequest = request;
  return true;
}
} // namespace ade
