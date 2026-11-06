#pragma once
#include <QJsonObject>
namespace ade {
// Source-aligned scalar/choice/checkbox elicitation mapping, not general JSON
// Schema.
QJsonObject acpQuestionForm(const QJsonObject &request,
                            QString *error = nullptr);
bool acpQuestionAnswers(const QJsonObject &request, const QJsonObject &answers);
} // namespace ade
