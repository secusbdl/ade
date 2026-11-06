#pragma once
#include "attachments/FileTransformer.h"
namespace ade {
struct ArchiveLimits {
    qint64 maxInputBytes = 64 * 1024 * 1024, maxInflatedBytes = 128 * 1024 * 1024;
    int maxEntries = 2000, maxHeaders = 20000, maxTreeNodes = 10000;
    int maxPathDepth = 32, maxNameBytes = 4096, maxOutputChars = 65536;
    int maxInflateCalls = 8192, maxGzipMembers = 16;
};
// Listing only. Member contents are never returned, extracted, mounted, or executed.
// TGZ uses a bounded gzip stream to reach TAR headers and skips member payloads.
class ArchiveTransformer {
public:
    static FileTransformResult fromBytes(const QByteArray &, const FileDescriptor &,
        const FileReadParams & = {}, const ArchiveLimits & = {}, const std::atomic_bool *cancelled = nullptr);
};
}
