#include "ContextPreferences.h"
#include "core/ToolExecutor.h"
#include <QDir>
#include <QFileInfo>
namespace ade {
QString contextWorkspaceKey(const QString& workspace){
    if(workspace.trimmed().isEmpty())return {};
    ToolExecutor resolver;
    auto path=resolver.setWorkspace(workspace)?resolver.workspace():QFileInfo(workspace).absoluteFilePath();
    path=QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    path=path.toCaseFolded();
#endif
    return path;
}
QJsonObject workspaceContextSettingsFor(const QJsonObject& preferences,const QString& workspace){
    const auto options=preferences["skillOptions"].toObject();
    // Legacy unscoped settings remain effective until an explicit scoped edit
    // moves them to the current workspace. No owner can be inferred safely.
    QJsonObject settings{{"respectAgentsMd",options["respectAgentsMd"].toBool(true)},
                         {"disabledSkills",options["disabledSkills"].toArray()}};
    const auto key=contextWorkspaceKey(workspace);
    if(!key.isEmpty()){
        const auto all=options["workspaceSettings"].toObject();
        const auto scoped=all[key].toObject();
        if(scoped.contains("respectAgentsMd"))settings["respectAgentsMd"]=scoped["respectAgentsMd"];
        if(scoped.contains("disabledSkills"))settings["disabledSkills"]=scoped["disabledSkills"];
    }
    return settings;
}
ContextOptions contextOptionsFor(const QJsonObject& preferences,const QString& workspace,const QString& homeDirectory){
    ContextOptions options;options.workspace=workspace;options.homeDirectory=homeDirectory;
    for(const auto& value:preferences["skills"].toArray()){const auto entry=value.toObject();options.extraSkillDirectories.append({entry["path"].toString(),entry["enabled"].toBool(true)});}
    for(const auto& value:preferences["contextFiles"].toArray()){const auto entry=value.toObject();options.contextFiles.append({entry["path"].toString(),entry["enabled"].toBool(true)});}
    const auto skillOptions=preferences["skillOptions"].toObject();
    for(const auto& value:skillOptions["enabledGlobalDirectories"].toArray())options.enabledGlobalSkillDirs.append(value.toString());
    const auto workspaceSettings=workspaceContextSettingsFor(preferences,workspace);
    for(const auto& value:workspaceSettings["disabledSkills"].toArray())options.disabledSkills.append(value.toString());
    for(const auto& value:skillOptions["disabledGlobalSkills"].toArray())options.disabledGlobalSkills.append(value.toString());
    options.respectAgentsMd=workspaceSettings["respectAgentsMd"].toBool(true);return options;
}
}
