#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QJsonArray>

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
    // A chooser preference only; saved conversations/presets remain runnable.
    QStringList disabledModelIds{};
    // Versioned inventory and visibility defaults share the provider CAS revision.
    QJsonObject discoverySnapshot{};
    bool isClaudeHarness() const { return kind == QStringLiteral("claude-code"); }
    bool isOpenCodeHarness() const { return kind == QStringLiteral("opencode"); }
    bool isExternalHarness() const { return isCodexHarness() || isOpenCodeHarness() || isClaudeHarness(); }
    bool isCodexHarness() const { return kind == QStringLiteral("codex"); }

    QJsonObject publicJson() const {
        return {{"id", id}, {"name", name}, {"baseUrl", baseUrl},
                {"model", model}, {"protocol", protocol}, {"reasoningEffort", reasoningEffort},
                {"thinkingMode", thinkingMode}, {"thinkingBudget", thinkingBudget},
                {"maxOutputTokens", maxOutputTokens}, {"contextWindowTokens", contextWindowTokens},
                {"kind", kind}, {"harnessExecutable", harnessExecutable}, {"templateId", templateId}, {"disabledModelIds", QJsonArray::fromStringList(disabledModelIds)}, {"discoverySnapshot", discoverySnapshot}};
    }
    static ProviderConfig fromPublicJson(const QJsonObject &object) {
        ProviderConfig provider{object["id"].toString(), object["name"].toString(),
                object["baseUrl"].toString(), {}, object["model"].toString(),
                object["protocol"].toString(QStringLiteral("openai")),
                object["reasoningEffort"].toString(), object["thinkingMode"].toString(),
                object["thinkingBudget"].toInt(), object["maxOutputTokens"].toInt(),
                object["contextWindowTokens"].toInt(), object["kind"].toString(QStringLiteral("api")),
                object["harnessExecutable"].toString(), object["templateId"].toString()};
        for(const auto& value:object.value("disabledModelIds").toArray())provider.disabledModelIds.append(value.toString());
        provider.discoverySnapshot=object.value("discoverySnapshot").toObject();
        return provider;
    }
};

} // namespace ade
