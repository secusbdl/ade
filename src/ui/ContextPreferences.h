#pragma once
#include "core/ContextLoader.h"
namespace ade {
QString contextWorkspaceKey(const QString& workspace);
QJsonObject workspaceContextSettingsFor(const QJsonObject& preferences,const QString& workspace);
ContextOptions contextOptionsFor(const QJsonObject& preferences,const QString& workspace,const QString& homeDirectory);
}
