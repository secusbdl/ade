#pragma once
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <array>

// Search filters for the native glob and grepSearch tools. The rules follow
// ripgrep 15.0.0 (the engine upstream Stagewise runs) as observed with real
// rg fixtures: gitignore line parsing, globset patterns, override globs and
// the hidden-file policy. This module performs no file I/O.
namespace ade::search {
enum class Syntax { IgnoreFile, Override };
enum class Match { None, Ignore, Whitelist };
struct Rule {
    QRegularExpression regex;
    // When prunes is set, every descendant of a directory matching
    // descendants also matches regex (rules ending in "/**").
    QRegularExpression descendants;
    bool negated=false,directoryOnly=false,prunes=false;
    int line=0;
};
// Returns true when text produced a rule. Blank lines and comments return
// false with an empty error; malformed globs return false with the reason.
bool compileRule(const QString& text,Syntax syntax,int line,Rule* rule,QString* error);

struct RuleFile {
    QString base;   // mount-relative directory holding the file; empty for the mount root
    QList<Rule> rules;
    Match match(const QString& mountPath,bool isDirectory) const;
};
// Index order is precedence order: .rgignore beats .ignore beats .gitignore
// regardless of depth; within one kind the deepest matching file decides.
enum class IgnoreKind { RgIgnore=0, Ignore=1, GitIgnore=2 };
constexpr int IgnoreKindCount=3;
QString ignoreFileName(IgnoreKind kind);
struct IgnoreLevel {
    std::array<RuleFile,IgnoreKindCount> files;
    std::array<bool,IgnoreKindCount> present{};
    // The directory holds a .git entry (file or directory). Like rg, .gitignore
    // files above the nearest such directory do not apply inside it; .ignore
    // and .rgignore files are not scoped by git repositories.
    bool gitBoundary=false;
};
class IgnoreStack {
public:
    void push(IgnoreLevel level){m_levels.append(std::move(level));}
    void pop(){if(!m_levels.isEmpty())m_levels.removeLast();}
    // A directory named node_modules is a native built-in exclusion with the
    // lowest precedence, so any ignore file may still re-include it.
    Match match(const QString& mountPath,const QString& name,bool isDirectory) const;
private:
    QList<IgnoreLevel> m_levels;
};
// Tool globs in command-line order, like rg -g: plain globs whitelist, a
// leading '!' excludes, and the last matching glob wins.
struct OverrideSet {
    QList<Rule> rules;
    int whitelists=0;
    bool add(const QString& glob,QString* error);
    Match match(const QString& searchPath,bool isDirectory) const;
    bool prunes(const QString& searchDirectory) const;
};
struct Policy {
    bool respectIgnores=true;
    bool skipHidden=false;
};
// Native policy first (.git and links are never listed or entered), then the
// rg order: overrides, ignore files, hidden names. True keeps the entry.
bool admit(const Policy& policy,const OverrideSet& overrides,const IgnoreStack& ignores,
           const QString& name,const QString& searchPath,const QString& mountPath,
           bool isDirectory,bool isReparsePoint,bool isHiddenAttribute);
}
