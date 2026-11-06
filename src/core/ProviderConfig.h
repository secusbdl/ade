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
    // Optional external coding agents are separate from HTTP inference.
    // Subscription credentials remain managed by the selected CLI.
    QString kind = QStringLiteral("api");
    QString harnessExecutable{};
    // Picker provenance only; never authorizes a request or supplies credentials.
    QString templateId{};
    bool isCodexHarness() const { return kind == QStringLiteral("codex"); }

    QJsonObject publicJson() const {
        return {{"id", id}, {"name", name}, {"baseUrl", baseUrl},
                {"model", model}, {"protocol", protocol}, {"reasoningEffort", reasoningEffort},
                {"thinkingMode", thinkingMode}, {"thinkingBudget", thinkingBudget},
                {"maxOutputTokens", maxOutputTokens}, {"contextWindowTokens", contextWindowTokens},
                {"kind", kind}, {"harnessExecutable", harnessExecutable}, {"templateId", templateId}};
    }
    static ProviderConfig fromPublicJson(const QJsonObject &object) {
        return {object["id"].toString(), object["name"].toString(),
                object["baseUrl"].toString(), {}, object["model"].toString(),
                object["protocol"].toString(QStringLiteral("openai")),
                object["reasoningEffort"].toString(), object["thinkingMode"].toString(),
                object["thinkingBudget"].toInt(), object["maxOutputTokens"].toInt(),
                object["contextWindowTokens"].toInt(), object["kind"].toString(QStringLiteral("api")),
                object["harnessExecutable"].toString(), object["templateId"].toString()};
    }
};

} // namespace ade
