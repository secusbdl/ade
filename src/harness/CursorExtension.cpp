#include "CursorExtension.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
namespace ade {
namespace {
bool text(const QJsonValue &v, int max, bool empty = false) {
  return v.isString() && v.toString().size() <= max &&
         (empty || !v.toString().isEmpty()) &&
         !v.toString().contains(QChar::Null);
}
bool bounded(const QJsonObject &o) {
  return QJsonDocument(o).toJson(QJsonDocument::Compact).size() <= 256 * 1024;
}
} // namespace
QJsonObject cursorQuestionForm(const QJsonObject &request, QString *error) {
  if (error)
    error->clear();
  auto fail = [&] {
    if (error)
      *error = "Cursor supplied an invalid or unsupported question.";
    return QJsonObject{};
  };
  if (!bounded(request) || !text(request["toolCallId"], 512) ||
      !request["questions"].isArray() ||
      (request.contains("title") && !text(request["title"], 8192, true)))
    return fail();
  const auto questions = request["questions"].toArray();
  if (questions.isEmpty() || questions.size() > 50)
    return fail();
  QSet<QString> seen;
  QJsonArray fields, steps;
  for (const auto &value : questions) {
    const auto q = value.toObject();
    const auto id = q["id"].toString();
    if (!value.isObject() || !text(q["id"], 128) || seen.contains(id) ||
        !text(q["prompt"], 8192) || !q["options"].isArray() ||
        (q.contains("allowMultiple") && !q["allowMultiple"].isBool()))
      return fail();
    seen.insert(id);
    const auto choices = q["options"].toArray();
    if (choices.isEmpty() || choices.size() > 100)
      return fail();
    QSet<QString> options;
    QJsonArray mapped;
    for (const auto &item : choices) {
      const auto option = item.toObject();
      const auto key = option["id"].toString();
      if (!item.isObject() || !text(option["id"], 512) ||
          !text(option["label"], 8192) || options.contains(key))
        return fail();
      options.insert(key);
      mapped.append(QJsonObject{{"value", key}, {"label", option["label"]}});
    }
    fields.append(
        QJsonObject{{"questionId", id},
                    {"type", q["allowMultiple"].toBool() ? "checkbox-group"
                                                         : "radio-group"},
                    {"label", q["prompt"]},
                    {"required", true},
                    {"allowOther", false},
                    {"options", mapped}});
    if (fields.size() == 10) {
      steps.append(QJsonObject{{"fields", fields}});
      fields = {};
    }
  }
  if (!fields.isEmpty())
    steps.append(QJsonObject{{"fields", fields}});
  return {{"title", request["title"].toString("Cursor needs input")},
          {"steps", steps}};
}
QJsonObject cursorQuestionResponse(const QJsonObject &request,
                                   const QJsonObject &answers, QString *error) {
  if (error)
    error->clear();
  auto fail = [&] {
    if (error)
      *error = "Choose only the options in the active Cursor question.";
    return QJsonObject{};
  };
  if (cursorQuestionForm(request).isEmpty() || !bounded(answers) ||
      answers.size() != request["questions"].toArray().size())
    return fail();
  QJsonArray result;
  for (const auto &value : request["questions"].toArray()) {
    const auto q = value.toObject();
    const auto answer = answers.value(q["id"].toString());
    QJsonArray selected;
    if (q["allowMultiple"].toBool()) {
      if (!answer.isArray())
        return fail();
      selected = answer.toArray();
    } else {
      if (!answer.isString())
        return fail();
      selected.append(answer);
    }
    if (selected.isEmpty() || selected.size() > q["options"].toArray().size())
      return fail();
    QSet<QString> available, seen;
    for (const auto &o : q["options"].toArray())
      available.insert(o.toObject()["id"].toString());
    for (const auto &s : selected) {
      if (!s.isString() || !available.contains(s.toString()) ||
          seen.contains(s.toString()))
        return fail();
      seen.insert(s.toString());
    }
    result.append(
        QJsonObject{{"questionId", q["id"]}, {"selectedOptionIds", selected}});
  }
  return {
      {"outcome", QJsonObject{{"outcome", "answered"}, {"answers", result}}}};
}
bool validCursorPlan(const QJsonObject &request) {
  if (!bounded(request) || !text(request["toolCallId"], 512) ||
      !text(request["plan"], 128 * 1024) || !request["todos"].isArray())
    return false;
  if (request["todos"].toArray().size() > 500)
    return false;
  QSet<QString> seen;
  for (const auto &v : request["todos"].toArray()) {
    const auto t = v.toObject();
    const auto id = t["id"].toString();
    if (!v.isObject() || !text(t["id"], 512) || seen.contains(id) ||
        !text(t["content"], 8192) ||
        !QStringList{"pending", "in_progress", "completed", "cancelled"}
             .contains(t["status"].toString()))
      return false;
    seen.insert(id);
  }
  return true;
}
} // namespace ade
