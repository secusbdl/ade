#pragma once
#include <QJsonArray>
#include <QString>
namespace ade {
struct AgentPromptContext {
    QString workspace, localContext, dynamicContext;
    bool apps = false, attachments = false, shells = false;
};
// Capability claims are derived from the same definition array sent on wire.
QString buildNativeSystemPrompt(const AgentPromptContext& context, const QJsonArray& definitions);
}
