#include "AcpForm.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>
namespace ade {
namespace {
QJsonArray options(const QJsonObject &property) {
  QJsonArray source;
  if (property["oneOf"].isArray())
    source = property["oneOf"].toArray();
  else {
    const auto items = property["items"].toObject();
    if (items["anyOf"].isArray())
      source = items["anyOf"].toArray();
    else if (items["enum"].isArray())
      source = items["enum"].toArray();
    else
      source = property["enum"].toArray();
  }
  QJsonArray result;
  QSet<QString> seen;
  for (const auto &choice : source) {
    const auto row = choice.toObject();
    const auto value =
        choice.isString() ? choice.toString() : row["const"].toString();
    if (value.isEmpty() || value.size() > 8192 || seen.contains(value))
      return {};
    seen.insert(value);
    result.append(QJsonObject{
        {"value", value}, {"label", row["title"].toString(value).left(8192)}});
    if (result.size() > 100)
      return {};
  }
  return result;
}
bool number(const QJsonObject &o, const char *key) {
  return !o.contains(key) ||
         (o[key].isDouble() && std::isfinite(o[key].toDouble()));
}
} // namespace
QJsonObject acpQuestionForm(const QJsonObject &request, QString *error) {
  if (error)
    error->clear();
  auto reject = [&]() {
    if (error)
      *error = "This ACP question uses an unsupported or invalid form.";
    return QJsonObject{};
  };
  if (request["mode"] != "form" ||
      QJsonDocument(request).toJson(QJsonDocument::Compact).size() > 256 * 1024)
    return reject();
  const auto schema = request["requestedSchema"].toObject(),
             properties = schema["properties"].toObject();
  if (schema["type"] != "object" || properties.isEmpty() ||
      properties.size() > 50)
    return reject();
  if (schema.contains("required") && !schema["required"].isArray())
    return reject();
  QSet<QString> required;
  for (const auto &key : schema["required"].toArray()) {
    if (!key.isString() || !properties.contains(key.toString()) ||
        required.contains(key.toString()))
      return reject();
    required.insert(key.toString());
  }
  QJsonArray fields, steps;
  for (auto it = properties.begin(); it != properties.end(); ++it) {
    const auto p = it.value().toObject();
    const auto type = p["type"].toString("string");
    if (it.key().isEmpty() || it.key().size() > 128 || !it.value().isObject() ||
        !QStringList{"string", "number", "integer", "boolean", "array"}
             .contains(type))
      return reject();
    for (const auto *unsupported :
         {"pattern", "allOf", "not", "if", "then", "else", "$ref"})
      if (p.contains(unsupported))
        return reject();
    const auto opts = options(p);
    if ((p.contains("enum") || p.contains("oneOf") || type == "array") &&
        opts.isEmpty())
      return reject();
    if ((type == "number" || type == "integer" || type == "boolean") &&
        !opts.isEmpty())
      return reject();
    QJsonObject field{{"questionId", it.key()},
                      {"label", p["title"].toString(it.key()).left(8192)},
                      {"description", p["description"].toString().left(8192)},
                      {"required", required.contains(it.key())}};
    if (type == "array") {
      for (const auto *key : {"minItems", "maxItems"})
        if (p.contains(key) &&
            (!number(p, key) || p[key].toDouble() < 0 ||
             p[key].toDouble() > 100 ||
             std::floor(p[key].toDouble()) != p[key].toDouble()))
          return reject();
      if (p.contains("minItems") && p.contains("maxItems") &&
          p["minItems"].toInt() > p["maxItems"].toInt())
        return reject();
      if (p["items"].toObject()["type"].toString("string") != "string")
        return reject();
      field["type"] = "checkbox-group";
      field["options"] = opts;
      field["allowOther"] = false;
      if (p.contains("default"))
        field["defaultValues"] = p["default"];
    } else if (type == "boolean") {
      field["type"] = "checkbox";
      if (p.contains("default"))
        field["defaultValue"] = p["default"];
    } else if (!opts.isEmpty()) {
      field["type"] = "radio-group";
      field["options"] = opts;
      field["allowOther"] = false;
      if (p.contains("default"))
        field["defaultValue"] = p["default"];
    } else {
      field["type"] = "input";
      field["inputType"] = (type == "number" || type == "integer")
                               ? "number"
                               : (p["format"] == "email" ? "email" : "text");
      for (const auto *key : {"minLength", "maxLength", "minimum", "maximum"})
        if (!number(p, key))
          return reject();
      for (const auto *key : {"minLength", "maxLength"})
        if (p.contains(key)) {
          const auto n = p[key].toDouble();
          if (n < 0 || n > 8192 || std::floor(n) != n)
            return reject();
          field[key] = p[key];
        }
      if (p.contains("minimum"))
        field["min"] = p["minimum"];
      if (p.contains("maximum"))
        field["max"] = p["maximum"];
      if (p.contains("default"))
        field["defaultValue"] = p["default"];
    }
    if ((p.contains("minimum") && p.contains("maximum") &&
         p["minimum"].toDouble() > p["maximum"].toDouble()) ||
        (p.contains("minLength") && p.contains("maxLength") &&
         p["minLength"].toInt() > p["maxLength"].toInt()))
      return reject();
    fields.append(field);
    if (fields.size() == 10) {
      steps.append(QJsonObject{{"fields", fields}});
      fields = {};
    }
  }
  if (!fields.isEmpty())
    steps.append(QJsonObject{{"fields", fields}});
  return {{"title", schema["title"].toString("Agent needs input").left(8192)},
          {"description", request["message"].toString().left(8192)},
          {"steps", steps}};
}
bool acpQuestionAnswers(const QJsonObject &request,
                        const QJsonObject &answers) {
  if (acpQuestionForm(request).isEmpty() ||
      QJsonDocument(answers).toJson(QJsonDocument::Compact).size() > 64 * 1024)
    return false;
  const auto schema = request["requestedSchema"].toObject(),
             properties = schema["properties"].toObject();
  for (const auto &required : schema["required"].toArray())
    if (!answers.contains(required.toString()))
      return false;
  for (auto it = answers.begin(); it != answers.end(); ++it) {
    if (!properties.contains(it.key()))
      return false;
    const auto p = properties[it.key()].toObject();
    const auto type = p["type"].toString("string");
    const auto value = it.value();
    const auto choices = options(p);
    auto allowed = [&](const QJsonValue &v) {
      for (const auto &option : choices)
        if (option.toObject()["value"] == v)
          return true;
      return false;
    };
    if (type == "array") {
      if (!value.isArray() || value.toArray().size() > 100)
        return false;
      QSet<QString> seen;
      for (const auto &item : value.toArray()) {
        if (!item.isString() || seen.contains(item.toString()) ||
            !allowed(item))
          return false;
        seen.insert(item.toString());
      }
      if (value.toArray().size() < p["minItems"].toInt(0) ||
          value.toArray().size() > p["maxItems"].toInt(100))
        return false;
    } else if (type == "boolean") {
      if (!value.isBool())
        return false;
    } else if (type == "number" || type == "integer") {
      if (!value.isDouble())
        return false;
      const auto n = value.toDouble();
      if (!std::isfinite(n) || (type == "integer" && std::floor(n) != n) ||
          (p.contains("minimum") && n < p["minimum"].toDouble()) ||
          (p.contains("maximum") && n > p["maximum"].toDouble()))
        return false;
    } else {
      if (!value.isString() ||
          value.toString().size() < p["minLength"].toInt(0) ||
          value.toString().size() > p["maxLength"].toInt(8192) ||
          (!choices.isEmpty() && !allowed(value)))
        return false;
    }
  }
  return true;
}
} // namespace ade
