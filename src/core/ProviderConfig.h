#pragma once

#include <QJsonObject>
#include <QString>

namespace ade {

struct ProviderConfig {
    QString id;
    QString name;
    QString baseUrl;
    QString apiKey;
    QString model;
    QString protocol = QStringLiteral("openai");
    QString reasoningEffort{};
    QString thinkingMode{};
    int thinkingBudget = 0;
    int maxOutputTokens = 0;
    // 0 means unknown. This belongs to the selected model, not the provider.
    int contextWindowTokens = 0;

    QJsonObject publicJson() const {
        return {{"id", id}, {"name", name}, {"baseUrl", baseUrl},
                {"model", model}, {"protocol", protocol}, {"reasoningEffort", reasoningEffort},
                {"thinkingMode", thinkingMode}, {"thinkingBudget", thinkingBudget},
                {"maxOutputTokens", maxOutputTokens}, {"contextWindowTokens", contextWindowTokens}};
    }
    static ProviderConfig fromPublicJson(const QJsonObject &object) {
        return {object["id"].toString(), object["name"].toString(),
                object["baseUrl"].toString(), {}, object["model"].toString(),
                object["protocol"].toString(QStringLiteral("openai")),
                object["reasoningEffort"].toString(), object["thinkingMode"].toString(),
                object["thinkingBudget"].toInt(), object["maxOutputTokens"].toInt(),
                object["contextWindowTokens"].toInt()};
    }
};

} // namespace ade
