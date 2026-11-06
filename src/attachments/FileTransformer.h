#pragma once
// Native adaptation of Stagewise 1.31.1 file-read-transformer.
// Source attribution, bounds and differences: audit/structured-file-transformer.md.
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <atomic>
#include <optional>

namespace ade {
struct FileReadParams {
    std::optional<int> startLine, endLine, startPage, endPage;
    std::optional<double> depth; // The pinned ls schema permits nonnegative numbers.
    std::optional<bool> preview;
    QJsonObject toJson() const;
    bool operator==(const FileReadParams &) const = default;
};
struct FileDescriptor {
    QString mountedPath, originalFileName;
    qint64 size = -1; // fromBytes uses the captured bytes when omitted.
    QDateTime modified;
};
struct FileTransformLimits {
    int maxReadChars = 40000, maxPreviewLines = 30;
    qint64 maxFileBytes = 64 * 1024 * 1024;
    // Inner text body only; XML envelope adds bounded descriptor and metadata text.
    int maxOutputChars = 65536, maxTextLines = 100000;
    int maxTreeNodes = 10000, maxTreeDepth = 32;
};
struct FileTreeEntry {
    QString name;
    bool isDirectory = false;
    QList<FileTreeEntry> children;
    QJsonObject metadata;
};
struct FileTransformResult {
    QJsonObject metadata;
    QJsonArray parts;
    FileReadParams requested; // Normalized request after any small-preview promotion.
    std::optional<FileReadParams> effective;
    bool truncated = false, previewPromoted = false, supported = true, coverageKnown = true;
    // Never record complete coverage from delivered() when coverageKnown is false.
    QString error;
    FileReadParams delivered() const { return effective.value_or(requested); }
};
// Pure transformations: callers own filesystem scope, captures and cancellation lifetime.
// No filesystem access, tool execution, network, mutable global state or hidden cache.
class FileTransformer {
public:
    static bool readParams(const QJsonObject &arguments, FileReadParams *result, QString *error = nullptr);
    static bool listParams(const QJsonObject &arguments, FileReadParams *result, QString *error = nullptr);
    static FileTransformResult fromBytes(const QByteArray &bytes, const FileDescriptor &file,
        const FileReadParams &params = {}, const FileTransformLimits &limits = {},
        const std::atomic_bool *cancelled = nullptr);
    // Empty internal params use the source directory-transform default depth 2;
    // listParams() supplies the current ls tool's default depth 0.
    static FileTransformResult fromDirectory(const QList<FileTreeEntry> &entries, const FileDescriptor &file,
        const FileReadParams &params = {}, const FileTransformLimits &limits = {},
        const std::atomic_bool *cancelled = nullptr);
    static QJsonArray wrappedParts(const FileDescriptor &file, const FileTransformResult &result);
};
}
