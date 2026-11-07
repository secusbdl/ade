#include "QuestionForm.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
namespace ade {
namespace {
bool text(const QJsonObject &o, const char *key, bool required = false) {
  const auto v = o[key];
  return (!required && v.isUndefined()) ||
         (v.isString() && v.toString().size() <= 8192);
}
bool boolean(const QJsonObject &o, const char *key) {
  return !o.contains(key) || o[key].isBool();
}
bool number(const QJsonObject &o, const char *key) {
  return !o.contains(key) ||
         (o[key].isDouble() && std::isfinite(o[key].toDouble()));
}
QJsonObject scalar(const char *type) { return {{"type", type}}; }
} // namespace
QJsonObject QuestionForm::toolDefinition() {
  QJsonObject field{
      {"type", scalar("string")},
      {"questionId", scalar("string")},
      {"label", scalar("string")},
      {"description", scalar("string")},
      {"inputType", QJsonObject{{"type", "string"},
                                {"enum", QJsonArray{"text", "email", "number",
                                                    "password"}}}},
      {"placeholder", scalar("string")},
      {"defaultValue",
       QJsonObject{{"anyOf", QJsonArray{scalar("string"), scalar("number"),
                                        scalar("boolean")}}}},
      {"required", scalar("boolean")},
      {"allowOther", scalar("boolean")},
      {"minLength", scalar("number")},
      {"maxLength", scalar("number")},
      {"min", scalar("number")},
      {"max", scalar("number")},
      {"options",
       QJsonObject{
           {"type", "array"},
           {"minItems", 1},
           {"maxItems", 100},
           {"items", QJsonObject{{"type", "object"},
                                 {"properties",
                                  QJsonObject{{"value", scalar("string")},
                                              {"label", scalar("string")}}},
                                 {"required", QJsonArray{"value", "label"}}}}}},
      {"defaultValues",
       QJsonObject{{"type", "array"}, {"items", scalar("string")}}}};
  field["type"] =
      QJsonObject{{"type", "string"},
                  {"enum", QJsonArray{"input", "radio-group", "checkbox",
                                      "checkbox-group"}}};
  const QJsonObject step{
      {"type", "object"},
      {"properties",
       QJsonObject{
           {"title", scalar("string")},
           {"description", scalar("string")},
           {"fields",
            QJsonObject{{"type", "array"},
                        {"minItems", 1},
                        {"maxItems", 10},
                        {"items", QJsonObject{{"type", "object"},
                                              {"properties", field},
                                              {"required",
                                               QJsonArray{"type", "questionId",
                                                          "label"}}}}}}}},
      {"required", QJsonArray{"fields"}}};
  return {
      {"type", "function"},
      {"function",
       QJsonObject{
           {"name", "askUserQuestions"},
           {"description",
            "Ask the user a structured form and wait for explicit answers. Use "
            "one call with 1-5 steps, 1-10 fields each; never make concurrent "
            "question calls. Radio groups include Other unless allowOther is "
            "false. Sending a user message interrupts the form and returns "
            "partial answers with a notice; read the following user message. "
            "Do not use this for ordinary action approval."},
           {"parameters",
            QJsonObject{{"type", "object"},
                        {"properties",
                         QJsonObject{{"title", scalar("string")},
                                     {"description", scalar("string")},
                                     {"steps", QJsonObject{{"type", "array"},
                                                           {"minItems", 1},
                                                           {"maxItems", 5},
                                                           {"items", step}}}}},
                        {"required", QJsonArray{"title", "steps"}}}}}}};
}
bool QuestionForm::validate(const QJsonObject &definition, QString *error) {
  if (error)
    error->clear();
  auto reject = [&](const QString &reason) {
    if (error)
      *error = reason;
    return false;
  };
  if (QJsonDocument(definition).toJson(QJsonDocument::Compact).size() >
          256 * 1024 ||
      !text(definition, "title", true) || !text(definition, "description") ||
      !definition["steps"].isArray() ||
      definition["steps"].toArray().isEmpty() ||
      definition["steps"].toArray().size() > 5)
    return reject("Question form must have a title and 1-5 bounded steps.");
  QSet<QString> ids;
  for (const auto &value : definition["steps"].toArray()) {
    const auto step = value.toObject();
    const auto fields = step["fields"].toArray();
    if (!value.isObject() || !text(step, "title") ||
        !text(step, "description") || !step["fields"].isArray() ||
        fields.isEmpty() || fields.size() > 10)
      return reject("Each question step must contain 1-10 fields.");
    for (const auto &item : fields) {
      const auto f = item.toObject();
      const auto id = f["questionId"].toString(), type = f["type"].toString();
      if (!item.isObject() || !text(f, "questionId", true) || id.isEmpty() ||
          id.size() > 128 || ids.contains(id) || !text(f, "label", true) ||
          !text(f, "description") || !text(f, "placeholder") ||
          !boolean(f, "required") || !boolean(f, "allowOther") ||
          !QStringList{"input", "radio-group", "checkbox", "checkbox-group"}
               .contains(type))
        return reject(
            "Question fields require unique IDs and a supported type.");
      ids.insert(id);
      if (type == "input") {
        if ((f.contains("inputType") &&
             !QStringList{"text", "email", "number", "password"}.contains(
                 f["inputType"].toString())) ||
            !number(f, "min") || !number(f, "max") || !number(f, "minLength") ||
            !number(f, "maxLength") ||
            (f.contains("defaultValue") && !f["defaultValue"].isString() &&
             !f["defaultValue"].isDouble()))
          return reject("Question input configuration is invalid.");
        for (const auto *key : {"minLength", "maxLength"})
          if (f.contains(key) &&
              (f[key].toDouble() < 0 || f[key].toDouble() > 8192 ||
               std::floor(f[key].toDouble()) != f[key].toDouble()))
            return reject(
                "Question length constraints must be integers within 0-8192.");
        if ((f.contains("min") && f.contains("max") &&
             f["min"].toDouble() > f["max"].toDouble()) ||
            (f.contains("minLength") && f.contains("maxLength") &&
             f["minLength"].toInt() > f["maxLength"].toInt()))
          return reject("Question limits are reversed.");
      } else if (type == "checkbox") {
        if (f.contains("defaultValue") && !f["defaultValue"].isBool())
          return reject("Checkbox default must be boolean.");
      } else {
        const auto options = f["options"].toArray();
        QSet<QString> values;
        if (!f["options"].isArray() || options.isEmpty() ||
            options.size() > 100)
          return reject("Choice fields require 1-100 options.");
        for (const auto &option : options) {
          const auto o = option.toObject();
          const auto v = o["value"].toString();
          if (!option.isObject() || !text(o, "label", true) ||
              !text(o, "value", true) || v.isEmpty() ||
              v.startsWith("__other__") || values.contains(v))
            return reject("Choice option values must be nonempty and unique.");
          values.insert(v);
        }
        if (type == "radio-group" && f.contains("defaultValue") &&
            (!f["defaultValue"].isString() ||
             !values.contains(f["defaultValue"].toString())))
          return reject("Radio default is not an available choice.");
        if (type == "checkbox-group" && f.contains("defaultValues")) {
          if (!f["defaultValues"].isArray())
            return reject("Checkbox defaults must be a list.");
          QSet<QString> selected;
          for (const auto &v : f["defaultValues"].toArray()) {
            if (!v.isString() || !values.contains(v.toString()) ||
                selected.contains(v.toString()))
              return reject(
                  "Checkbox default is not an available unique choice.");
            selected.insert(v.toString());
          }
        }
      }
    }
  }
  return true;
}
QJsonObject QuestionForm::defaults(const QJsonArray &fields,
                                   const QJsonObject &saved) {
  QJsonObject out;
  for (const auto &value : fields) {
    const auto f = value.toObject();
    const auto id = f["questionId"].toString();
    if (saved.contains(id)) {
      out[id] = saved[id];
      continue;
    }
    const auto type = f["type"].toString();
    out[id] = type == "checkbox-group" ? f.value("defaultValues").toArray()
              : type == "checkbox"     ? QJsonValue(f["defaultValue"].toBool())
              : f.value("defaultValue").isUndefined() ? QJsonValue("")
                                                      : f["defaultValue"];
  }
  return out;
}
bool QuestionForm::validateAnswers(const QJsonArray &fields,
                                   const QJsonObject &answers,
                                   bool requireComplete, QString *error) {
  if (error)
    error->clear();
  auto reject = [&](const QString &id, const QString &reason) {
    if (error)
      *error = id + ": " + reason;
    return false;
  };
  if (QJsonDocument(answers).toJson(QJsonDocument::Compact).size() > 128 * 1024)
    return reject("Answers", "Response exceeds 128 KiB.");
  QSet<QString> ids;
  for (const auto &value : fields)
    ids.insert(value.toObject()["questionId"].toString());
  for (auto it = answers.begin(); it != answers.end(); ++it)
    if (!ids.contains(it.key()))
      return reject(it.key(),
                    "This field does not belong to the current step.");
  for (const auto &entry : fields) {
    const auto f = entry.toObject();
    const auto id = f["questionId"].toString(), type = f["type"].toString();
    const auto v = answers[id];
    if (v.isUndefined()) {
      if (requireComplete && f["required"].toBool())
        return reject(id, "This field is required.");
      continue;
    }
    if (type == "input") {
      if ((!v.isString() && !v.isDouble()) ||
          (v.isString() && v.toString().size() > 8192))
        return reject(id, "Enter a bounded text or number value.");
      const auto s = v.isString() ? v.toString().trimmed()
                                  : QString::number(v.toDouble(), 'g', 16);
      if (!requireComplete)
        continue;
      if (f["required"].toBool() && s.isEmpty())
        return reject(id, "This field is required.");
      if (f.contains("minLength") && s.size() < f["minLength"].toInt())
        return reject(id, "The answer is too short.");
      if (f.contains("maxLength") && s.size() > f["maxLength"].toInt())
        return reject(id, "The answer is too long.");
      if (f["inputType"] == "email" && !s.isEmpty() &&
          !QRegularExpression("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$")
               .match(s)
               .hasMatch())
        return reject(id, "Enter a valid email address.");
      if (f["inputType"] == "number" && !s.isEmpty()) {
        bool ok = false;
        const double n = s.toDouble(&ok);
        if (!ok || !std::isfinite(n) ||
            (f.contains("min") && n < f["min"].toDouble()) ||
            (f.contains("max") && n > f["max"].toDouble()))
          return reject(id, "Enter a number within the allowed range.");
      }
    } else if (type == "checkbox") {
      if (!v.isBool())
        return reject(id, "Checkbox answer must be boolean.");
    } else {
      QSet<QString> choices;
      for (const auto &option : f["options"].toArray())
        choices.insert(option.toObject()["value"].toString());
      if (type == "radio-group") {
        if (!v.isString() || v.toString().size() > 8192)
          return reject(id, "Select an option.");
        const auto s = v.toString();
        if (s.startsWith("__other__:") && f["allowOther"] != false) {
          if (requireComplete && s.mid(10).trimmed().isEmpty())
            return reject(id, "Enter an Other value.");
        } else if (!s.isEmpty() && !choices.contains(s))
          return reject(id, "This option is unavailable.");
        if (requireComplete && f["required"].toBool() && s.isEmpty())
          return reject(id, "Select an option.");
      } else {
        if (!v.isArray() || v.toArray().size() > 100)
          return reject(id, "Select a bounded list of options.");
        QSet<QString> seen;
        for (const auto &item : v.toArray()) {
          if (!item.isString() || !choices.contains(item.toString()) ||
              seen.contains(item.toString()))
            return reject(id,
                          "A selected option is unavailable or duplicated.");
          seen.insert(item.toString());
        }
        if (requireComplete && f["required"].toBool() && seen.isEmpty())
          return reject(id, "Select at least one option.");
      }
    }
  }
  return true;
}
QJsonArray QuestionForm::fields() const {
  const auto steps = m_definition["steps"].toArray();
  return m_step >= 0 && m_step < steps.size()
             ? steps[m_step].toObject()["fields"].toArray()
             : QJsonArray{};
}
bool QuestionForm::begin(const QJsonObject &definition, QString *error) {
  if (!validate(definition, error))
    return false;
  m_definition = definition;
  m_step = 0;
  m_answers = {};
  m_draft = {};
  return true;
}
bool QuestionForm::submit(int expectedStep, const QJsonObject &answers,
                          QString *error) {
  if (expectedStep != m_step || completed()) {
    if (error)
      *error = "This question step is no longer current.";
    return false;
  }
  if (!validateAnswers(fields(), answers, true, error))
    return false;
  for (auto it = answers.begin(); it != answers.end(); ++it)
    m_answers[it.key()] = it.value();
  m_draft = {};
  ++m_step;
  return true;
}
bool QuestionForm::updateDraft(int expectedStep, const QJsonObject &answers,
                               QString *error) {
  if (expectedStep != m_step || completed())
    return false;
  if (!validateAnswers(fields(), answers, false, error))
    return false;
  m_draft = answers;
  return true;
}
bool QuestionForm::back(int expectedStep) {
  if (expectedStep != m_step || m_step == 0 || completed())
    return false;
  m_draft = {};
  --m_step;
  return true;
}
QJsonObject QuestionForm::snapshot(const QString &token) const {
  auto out = m_definition;
  out["id"] = token;
  out["currentStep"] = m_step;
  auto answers = m_answers;
  for (auto it = m_draft.begin(); it != m_draft.end(); ++it)
    answers[it.key()] = it.value();
  out["answers"] = answers;
  return out;
}
QJsonObject QuestionForm::result(const QString &reason) const {
  auto answers = m_answers;
  // Explicit Cancel and agent Stop preserve only submitted steps. The
  // message-interrupt path deliberately carries the current draft upstream.
  if (reason.isEmpty() || reason == "user_sent_message")
    for (auto it = m_draft.begin(); it != m_draft.end(); ++it)
      answers[it.key()] = it.value();
  QJsonObject out{
      {"completed", reason.isEmpty() && completed()},
      {"cancelled", !reason.isEmpty() && reason != "user_sent_message"},
      {"answers", answers},
      {"completedSteps", m_step}};
  if (reason == "user_sent_message")
    out["notice"] = "User sent a message while responding to questions. Read "
                    "the user message that follows this tool result.";
  else if (!reason.isEmpty())
    out["cancelReason"] = reason;
  return out;
}
} // namespace ade
