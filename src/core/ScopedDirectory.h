#pragma once
#include <QDateTime>
#include <QList>
#include <QString>
#include <atomic>
#include <memory>

namespace ade {
struct ScopedDirectoryEntry {
    QString name,absolutePath;
    bool isDirectory=false,isReparsePoint=false;
};
struct ScopedDirectorySnapshot {
    QString canonicalPath;
    QDateTime modified;
    QList<ScopedDirectoryEntry> entries;
    // Keep the opened directory pinned while a caller visits descendants.
    std::shared_ptr<void> pin;
};
// Enumerates the validated handle, never reopens the supplied alias for names.
// Without truncated, a bound failure returns no partial result. Search callers
// may explicitly request a bounded prefix and must disclose *truncated.
bool readScopedDirectory(const QString& path,const QString& root,ScopedDirectorySnapshot* result,
                         QString* error,int maxEntries=10000,const std::atomic_bool* cancelled=nullptr,
                         bool allowRootAlias=true,bool* truncated=nullptr);
}
