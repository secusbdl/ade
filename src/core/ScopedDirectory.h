#pragma once
#include <QDateTime>
#include <QList>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>

namespace ade {
struct ScopedDirectoryEntry {
    QString name,absolutePath;
    bool isDirectory=false,isReparsePoint=false;
    // Windows FILE_ATTRIBUTE_HIDDEN; always false elsewhere.
    bool isHidden=false;
};
struct ScopedDirectorySnapshot {
    QString canonicalPath;
    QDateTime modified;
    QList<ScopedDirectoryEntry> entries;
    // Keep the opened directory pinned while a caller visits descendants.
    std::shared_ptr<void> pin;
};
// Optional search filter. Names that admit rejects are dropped before they
// count toward maxEntries; every enumerated name counts toward *scanned, and
// passing scanLimit behaves like reaching maxEntries.
struct ScopedDirectoryAdmission {
    std::function<bool(const ScopedDirectoryEntry&)> admit;
    qint64* scanned=nullptr;
    qint64 scanLimit=0;
};
// Enumerates the validated handle, never reopens the supplied alias for names.
// Without truncated, a bound failure returns no partial result. Search callers
// may explicitly request a bounded prefix and must disclose *truncated.
bool readScopedDirectory(const QString& path,const QString& root,ScopedDirectorySnapshot* result,
                         QString* error,int maxEntries=10000,const std::atomic_bool* cancelled=nullptr,
                         bool allowRootAlias=true,bool* truncated=nullptr,
                         const ScopedDirectoryAdmission* admission=nullptr);
}
