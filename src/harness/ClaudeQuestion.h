#pragma once
#include <QJsonObject>
namespace ade {
QJsonObject claudeQuestionForm(const QJsonObject &input);
bool claudeQuestionResponse(const QJsonObject &input,
                            const QJsonObject &answers,
                            QJsonObject *updatedInput);
} // namespace ade
