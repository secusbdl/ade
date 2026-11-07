#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QStringList>

namespace ade {
struct ContextLocation { QString path; bool enabled = true; };
struct ContextOptions {
    QString workspace;
    QString mountPrefix = "ws1";
    QString homeDirectory; // Empty disables global discovery; the host supplies QDir::homePath().
    QString builtinSkillsDirectory;
    QString pluginsDirectory;
    QStringList enabledGlobalSkillDirs;
    QStringList disabledSkills, disabledGlobalSkills, disabledPluginIds;
    QList<ContextLocation> extraSkillDirectories, contextFiles;
    QStringList relevantPaths; // Optional scoped AGENTS extension; empty means upstream root-only behavior.
    QStringList invokedSkillIds; // Explicit user invocations; enabled/user-invocable skills only.
    bool respectAgentsMd = true;
};
struct ContextInstruction {
    QString path, scope, content;
    bool respected = true, truncated = false;
};
struct ContextSkill {
    QString id, name, description, source, path, contentPath, pluginId;
    bool userInvocable = true, agentInvocable = true;
};
struct ContextInvokedSkill { QString id, path, content; };
struct ContextSnapshot {
    QList<ContextInstruction> instructions;
    QList<ContextSkill> skills;
    QStringList warnings;
    QString prompt;
    QString fingerprint;
    QList<ContextInvokedSkill> invokedSkills;
    QJsonObject manifest() const;
};
// Synchronous, bounded local-file discovery. This class executes no scripts or remote actions.
class ContextLoader {
public:
    ContextSnapshot load(const ContextOptions &options);
    ContextSnapshot snapshot() const { return m_snapshot; }
    QString readFile(const QString &virtualPath, QString *error = nullptr) const;
    static QJsonArray toolDefinitions();
    // Transient HTTP user-message conversion; canonical history is unchanged.
    QJsonArray prepareMessages(const QJsonArray& history, QString* error = nullptr) const;
    QString nativeSystemContext() const;
    // For an external CLI's own tool runtime. No native mount/tool roster is
    // advertised; explicit instructions/invoked bodies are preserved as data.
    static QString externalHarnessContext(const ContextSnapshot& snapshot);
private:
    struct Mount { QString virtualRoot, physicalRoot; };
    struct ExactFile { QString path, root; };
    ContextSnapshot m_snapshot;
    QList<Mount> m_mounts;
    QMap<QString, ExactFile> m_exactFiles;
};
}
