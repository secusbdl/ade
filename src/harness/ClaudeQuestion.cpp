#include "ClaudeQuestion.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
namespace ade {
namespace {
bool text(const QJsonValue &v, int maximum, bool optional = false) {
  return (optional && v.isUndefined()) ||
         (v.isString() && v.toString().size() <= maximum &&
          (optional || !v.toString().trimmed().isEmpty()));
}
} // namespace
QJsonObject claudeQuestionForm(const QJsonObject &input) {
  if (QJsonDocument(input).toJson(QJsonDocument::Compact).size() > 256 * 1024 ||
      !input.value("questions").isArray())
    return {};
  const auto questions = input.value("questions").toArray();
  if (questions.isEmpty() || questions.size() > 4)
    return {};
  QJsonArray fields;
  QSet<QString> seen;
  for (qsizetype i = 0; i < questions.size(); ++i) {
    if (!questions[i].isObject())
      return {};
    const auto question = questions[i].toObject();
    const auto label = question.value("question");
    if (!text(label, 8192) || seen.contains(label.toString()) ||
        !text(question.value("header"), 128, true) ||
        !question.value("multiSelect").isBool() ||
        !question.value("options").isArray())
      return {};
    seen.insert(label.toString());
    const auto options = question.value("options").toArray();
    if (options.size() < 2 || options.size() > 4)
      return {};
    QJsonArray mapped;
    QSet<QString> labels;
    for (qsizetype j = 0; j < options.size(); ++j) {
      const auto option = options[j].toObject();
      const auto name = option.value("label");
      if (!text(name, 512) || labels.contains(name.toString()) ||
          !text(option.value("description"), 8192, true))
        return {};
      labels.insert(name.toString());
      mapped.append(
          QJsonObject{{"value", QString("o%1").arg(j)},
                      {"label", name},
                      {"description", option.value("description").toString()}});
    }
    const bool multi = question.value("multiSelect").toBool();
    fields.append(
        QJsonObject{{"questionId", QString("q%1").arg(i)},
                    {"type", multi ? "checkbox-group" : "radio-group"},
                    {"label", label},
                    {"required", true},
                    {"allowOther", !multi},
                    {"options", mapped}});
  }
  return {{"title", "Claude has a question"},
          {"steps", QJsonArray{QJsonObject{{"fields", fields}}}}};
}
bool claudeQuestionResponse(const QJsonObject &input,
                            const QJsonObject &answers,
                            QJsonObject *updatedInput) {
  const auto form = claudeQuestionForm(input);
  if (form.isEmpty() ||
      QJsonDocument(answers).toJson(QJsonDocument::Compact).size() > 64 * 1024)
    return false;
  const auto questions = input.value("questions").toArray();
  if (answers.size() != questions.size())
    return false;
  QJsonObject mapped;
  for (qsizetype i = 0; i < questions.size(); ++i) {
    const auto question = questions[i].toObject();
    const auto value = answers.value(QString("q%1").arg(i));
    const auto choices = question.value("options").toArray();
    auto label = [&](const QJsonValue &selected) -> QString {
      if (!selected.isString())
        return {};
      for (qsizetype j = 0; j < choices.size(); ++j)
        if (selected.toString() == QString("o%1").arg(j))
          return choices[j].toObject().value("label").toString();
      return {};
    };
    if (question.value("multiSelect").toBool()) {
      if (!value.isArray() || value.toArray().isEmpty() ||
          value.toArray().size() > choices.size())
        return false;
      QJsonArray selected;
      QSet<QString> seen;
      for (const auto &item : value.toArray()) {
        const auto name = label(item);
        if (name.isEmpty() || seen.contains(name))
          return false;
        seen.insert(name);
        selected.append(name);
      }
      mapped[question.value("question").toString()] = selected;
    } else {
      QString selected = label(value);
      if (value.isString() && value.toString().startsWith("__other__:"))
        selected = value.toString().mid(10);
      if (selected.trimmed().isEmpty() || selected.size() > 8192)
        return false;
      mapped[question.value("question").toString()] = selected;
    }
  }
  if (updatedInput) {
    *updatedInput = input;
    (*updatedInput)["answers"] = mapped;
  }
  return true;
}
} // namespace ade
