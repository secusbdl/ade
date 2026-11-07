#pragma once
#include <QJsonObject>
namespace ade {
// Typed projections/replies only; no filesystem or command authority.
QJsonObject cursorQuestionForm(const QJsonObject &request,
                               QString *error = nullptr);
QJsonObject cursorQuestionResponse(const QJsonObject &request,
                                   const QJsonObject &answers,
                                   QString *error = nullptr);
bool validCursorPlan(const QJsonObject &request);
} // namespace ade
