// Native adaptation of the pinned Stagewise archive listing contract.
// Source attribution, bounds and differences: audit/archive-transformer-evidence.md.
#include "ArchiveTransformer.h"
#include <QtZlib/zlib.h>
#include <QCollator>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace ade {
namespace {
struct Failure { QString kind, message; };
[[noreturn]] void fail(const QString &message, const QString &kind = "parse-failed") { throw Failure{kind, message}; }
bool span(quint64 offset, quint64 count, quint64 size) { return offset <= size && count <= size - offset; }
quint16 u16(const QByteArray &data, quint64 offset) { if (!span(offset, 2, data.size())) fail("Truncated 16-bit archive field."); return qFromLittleEndian<quint16>(data.constData() + offset); }
quint32 u32(const QByteArray &data, quint64 offset) { if (!span(offset, 4, data.size())) fail("Truncated 32-bit archive field."); return qFromLittleEndian<quint32>(data.constData() + offset); }
quint64 u64(const QByteArray &data, quint64 offset) { if (!span(offset, 8, data.size())) fail("Truncated 64-bit archive field."); return qFromLittleEndian<quint64>(data.constData() + offset); }
QString formatBytes(quint64 bytes) {
    if (!bytes) return "0B";
    int unit = 0; double value = double(bytes); while (unit < 3 && value >= 1024) { value /= 1024; ++unit; }
    return (value < 10 ? QString::number(value, 'f', 1) : QString::number(qRound64(value))) + QStringList{"B", "KB", "MB", "GB"}[unit];
}
QString textName(QString name) {
    name.replace('\\', "\\\\"); name.replace('\n', "\\n"); name.replace('\r', "\\r"); name.replace('\t', "\\t");
    return name;
}
QString utf8(const QByteArray &bytes) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    const QString result = decoder(bytes); if (decoder.hasError()) fail("Invalid UTF-8 archive name."); return result;
}
QString cp437(const QByteArray &bytes) {
    static constexpr char16_t extended[]{
#include "Cp437.inc"
    };
    static_assert(std::size(extended) == 128);
    QString result; result.reserve(bytes.size());
    for (const auto value : bytes) { const auto ch = quint8(value); result += QChar(ch < 128 ? char16_t(ch) : extended[ch - 128]); }
    return result;
}
struct Entry { QString path; bool directory = false; quint64 size = 0; };
struct Work {
    const ArchiveLimits &limits;
    const std::atomic_bool *cancelled;
    QList<Entry> entries;
    QSet<int> encodedMethods;
    bool encrypted = false, entryLimit = false, gzipPartial = false;
    void check() const { if (cancelled && cancelled->load(std::memory_order_relaxed)) fail("Archive listing cancelled.", "cancelled"); }
    void name(const QString &value) const {
        if (value.size() > limits.maxNameBytes || value.contains(QChar::Null)) fail("Archive name exceeds bounds or contains NUL.");
        if (value.count('/') + 1 > limits.maxPathDepth) fail("Archive member path exceeds the depth bound.", "limit");
    }
    void add(Entry entry) { name(entry.path); entries.append(std::move(entry)); entryLimit = entries.size() >= limits.maxEntries; }
};
void listZip(const QByteArray &data, Work &work) {
    work.check();
    qint64 end = -1;
    for (qint64 index = data.size() - 22; index >= qMax<qint64>(0, data.size() - 22 - 65535); --index) {
        if (u32(data, index) != 0x06054b50) continue;
        // A signature can occur inside the comment. Prefer the candidate whose
        // declared comment ends at EOF; yauzl 3.3.0 rejects the first false hit.
        if (quint64(index) + 22 + u16(data, index + 20) != quint64(data.size())) continue;
        end = index; break;
    }
    if (end < 0) fail("ZIP end-of-directory record is missing.");
    if (u16(data, end + 4) || u16(data, end + 6)) fail("Split ZIP archives are not supported.", "unsupported");
    quint64 count = u16(data, end + 10), directorySize = u32(data, end + 12), offset = u32(data, end + 16), directoryEndLimit = end;
    if (u16(data, end + 8) != count) fail("Split ZIP entry counts are not supported.", "unsupported");
    if (count == 0xffff || directorySize == 0xffffffffULL || offset == 0xffffffffULL) {
        if (end < 20 || u32(data, end - 20) != 0x07064b50) fail("ZIP64 locator is missing.");
        if (u32(data, end - 16) || u32(data, end - 4) != 1) fail("Split ZIP64 archives are not supported.", "unsupported");
        const auto zip64 = u64(data, end - 12);
        if (!span(zip64, 56, quint64(end - 20)) || u32(data, zip64) != 0x06064b50) fail("Invalid ZIP64 end record offset.");
        const auto recordSize = u64(data, zip64 + 4);
        if (recordSize < 44 || !span(zip64 + 12, recordSize, quint64(end - 20))) fail("Invalid ZIP64 end record size.");
        if (u32(data, zip64 + 16) || u32(data, zip64 + 20)) fail("Split ZIP64 archives are not supported.", "unsupported");
        count = u64(data, zip64 + 32); if (u64(data, zip64 + 24) != count) fail("Inconsistent ZIP64 entry counts.");
        directorySize = u64(data, zip64 + 40); offset = u64(data, zip64 + 48); directoryEndLimit = zip64;
    }
    if (!span(offset, directorySize, directoryEndLimit)) fail("ZIP central directory is outside the captured archive.");
    const auto directoryStart = offset;
    const auto directoryEnd = offset + directorySize;
    if (count > directorySize / 46) fail("ZIP entry count exceeds its central directory.");
    const auto desired = qMin<quint64>(count, work.limits.maxEntries);
    for (quint64 index = 0; index < desired; ++index) {
        work.check();
        if (!span(offset, 46, directoryEnd) || u32(data, offset) != 0x02014b50) fail("Invalid ZIP central entry.");
        const auto flags = u16(data, offset + 8), method = u16(data, offset + 10);
        if (flags & (1 << 6)) fail("Strong-encrypted ZIP entries are not supported.", "unsupported");
        quint64 compressed = u32(data, offset + 20), size = u32(data, offset + 24), localOffset = u32(data, offset + 42);
        quint32 disk = u16(data, offset + 34);
        const auto nameSize = u16(data, offset + 28), extraSize = u16(data, offset + 30), commentSize = u16(data, offset + 32);
        const quint64 recordSize = 46ULL + nameSize + extraSize + commentSize;
        if (!span(offset, recordSize, directoryEnd)) fail("Truncated ZIP central entry.");
        if (nameSize > work.limits.maxNameBytes) fail("ZIP name exceeds its byte bound.", "limit");
        const auto rawName = data.mid(offset + 46, nameSize), extra = data.mid(offset + 46 + nameSize, extraSize);
        QString unicodeName; bool hasUnicodeName = false, hasZip64 = false;
        for (quint64 p = 0; p < quint64(extra.size());) {
            if (!span(p, 4, extra.size())) fail("Truncated ZIP extra field.");
            const auto type = u16(extra, p), length = u16(extra, p + 2); p += 4;
            if (!span(p, length, extra.size())) fail("ZIP extra field exceeds its entry.");
            const auto payload = extra.mid(p, length); p += length;
            if (type == 1 && !hasZip64) {
                hasZip64 = true; quint64 q = 0;
                auto next64 = [&] { const auto value = u64(payload, q); q += 8; return value; };
                if (size == 0xffffffffULL) size = next64();
                if (compressed == 0xffffffffULL) compressed = next64();
                if (localOffset == 0xffffffffULL) localOffset = next64();
                if (disk == 0xffff) { disk = u32(payload, q); }
            }
            if (type == 0x7075 && !hasUnicodeName && length >= 6 && quint8(payload[0]) == 1
                && u32(payload, 1) == crc32(0, reinterpret_cast<const Bytef *>(rawName.constData()), uInt(rawName.size()))) {
                unicodeName = utf8(payload.mid(5)); hasUnicodeName = true;
            }
        }
        if (!hasZip64 && (size == 0xffffffffULL || compressed == 0xffffffffULL || localOffset == 0xffffffffULL || disk == 0xffff)) fail("Required ZIP64 extra field is missing.");
        if (disk) fail("Split ZIP member is not supported.", "unsupported");
        // No member contents are decoded. Bound the local header and encoded
        // payload extent so corrupt central offsets cannot describe nonexistent
        // bytes. This is stricter than yauzl's listing-only central scan.
        if (!span(localOffset, 30, directoryStart) || u32(data, localOffset) != 0x04034b50) fail("ZIP local header offset is outside the captured archive.");
        const auto dataStart = localOffset + 30 + u16(data, localOffset + 26) + u16(data, localOffset + 28);
        if (!span(dataStart, compressed, directoryStart)) fail("ZIP encoded member extent is outside the captured archive.");
        if (method == 0 && (size > std::numeric_limits<quint64>::max() - ((flags & 1) ? 12 : 0)
            || compressed != size + ((flags & 1) ? 12 : 0))) fail("Stored ZIP member has inconsistent sizes.");
        QString name = hasUnicodeName ? unicodeName : (flags & 0x800) ? utf8(rawName) : cp437(rawName);
        name.replace('\\', '/');
        if (name.startsWith('/') || QRegularExpression("^[A-Za-z]:").match(name).hasMatch() || name.split('/').contains("..")) fail("ZIP member name is absolute or escapes its archive.");
        const bool directory = name.endsWith('/'); work.add({name, directory, directory ? 0 : size});
        if (method != 0 && method != 8) work.encodedMethods.insert(method);
        work.encrypted |= flags & 1;
        offset += recordSize;
    }
    if (desired == count && offset != directoryEnd) fail("ZIP central directory size does not match its entry records.");
}
quint64 tarNumber(QByteArray bytes) {
    if (!bytes.isEmpty() && (quint8(bytes[0]) & 0x80)) fail("Base-256 TAR numeric fields are not supported.", "unsupported");
    const auto nul = bytes.indexOf('\0'); if (nul >= 0) bytes.truncate(nul); bytes = bytes.trimmed();
    quint64 value = 0;
    for (const auto ch : bytes) {
        if (ch < '0' || ch > '7' || value > (std::numeric_limits<quint64>::max() - quint64(ch - '0')) / 8) fail("Invalid or overflowing TAR octal field.");
        value = value * 8 + quint64(ch - '0');
    }
    return value;
}
QString tarString(QByteArray bytes) { const auto nul = bytes.indexOf('\0'); if (nul >= 0) bytes.truncate(nul); return utf8(bytes); }
class TarReader {
public:
    explicit TarReader(Work &work) : work(work) {}
    void feed(const char *data, qsizetype size) {
        while (size && !ended && !work.entryLimit) {
            work.check();
            if (skip) { const auto take = qMin<quint64>(skip, size); data += take; size -= take; skip -= take; continue; }
            const auto take = qMin<qsizetype>(512 - header.size(), size); header.append(data, take); data += take; size -= take;
            if (header.size() != 512) continue;
            if (++headers > work.limits.maxHeaders) fail("TAR header work limit exceeded.", "limit");
            if (std::all_of(header.cbegin(), header.cend(), [](char ch) { return ch == 0; })) { ended = true; header.clear(); break; }
            // Native validation deliberately rejects bad checksums; the pinned
            // manual TAR parser does not verify this field.
            quint64 checksum = 0; for (int i = 0; i < 512; ++i) checksum += i >= 148 && i < 156 ? 32 : quint8(header[i]);
            if (tarNumber(header.mid(148, 8)) != checksum) fail("TAR header checksum is invalid.");
            auto name = tarString(header.left(100)); const auto prefix = tarString(header.mid(345, 155)); if (!prefix.isEmpty()) name = prefix + '/' + name;
            const auto bytes = tarNumber(header.mid(124, 12));
            if (bytes > std::numeric_limits<quint64>::max() - 511) fail("TAR member size overflows its padding.");
            skip = ((bytes + 511) / 512) * 512;
            const auto type = header[156];
            if ((type == 0 || type == '0' || type == '5' || type == '2') && !name.isEmpty()) work.add({name, type == '5' || name.endsWith('/'), bytes});
            header.clear();
        }
    }
    void finish() { if (!work.entryLimit && (!header.isEmpty() || skip)) fail("TAR archive ends inside a header or member body."); }
private:
    Work &work; QByteArray header; quint64 skip = 0; int headers = 0; bool ended = false;
};
void listGzip(const QByteArray &data, Work &work) {
    z_stream stream{}; if (inflateInit2(&stream, 15 + 16) != Z_OK) fail("Cannot initialize native gzip decoder.");
    struct End { z_stream *stream; ~End() { inflateEnd(stream); } } cleanup{&stream};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData())); stream.avail_in = uInt(data.size());
    TarReader reader(work); QByteArray chunk(32768, Qt::Uninitialized); quint64 total = 0; int calls = 0, members = 1;
    while (true) {
        work.check(); if (++calls > work.limits.maxInflateCalls) fail("Gzip decoder work limit exceeded.", "limit");
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data()); stream.avail_out = uInt(chunk.size());
        const auto before = stream.avail_in; const int status = inflate(&stream, Z_NO_FLUSH); const auto count = chunk.size() - stream.avail_out;
        if (status != Z_OK && status != Z_STREAM_END) fail("Gzip archive is truncated, corrupt, or has an invalid trailer.");
        if (quint64(count) > quint64(work.limits.maxInflatedBytes) - total) fail("Gzip inflated-byte limit exceeded.", "limit");
        total += count; reader.feed(chunk.constData(), count);
        if (work.entryLimit) { work.gzipPartial = true; return; } // No claim that an unvisited trailer was verified.
        if (status == Z_STREAM_END) {
            if (!stream.avail_in || std::all_of(stream.next_in, stream.next_in + stream.avail_in, [](Bytef ch) { return ch == 0; })) break;
            if (stream.avail_in < 2 || stream.next_in[0] != 0x1f || stream.next_in[1] != 0x8b) fail("Unexpected trailing bytes after gzip member.");
            if (++members > work.limits.maxGzipMembers) fail("Gzip member count limit exceeded.", "limit");
            auto *next = stream.next_in; const auto remaining = stream.avail_in;
            if (inflateReset2(&stream, 15 + 16) != Z_OK) fail("Cannot reset native gzip decoder.");
            stream.next_in = next; stream.avail_in = remaining; continue;
        }
        if (!count && before == stream.avail_in) fail("Gzip archive is truncated, corrupt, or has an invalid trailer.");
    }
    reader.finish();
}
struct Node { QString name; std::optional<Entry> entry; QList<int> children; QHash<QString, int> indexes; };
QString render(Work &work, double depth, bool *truncated) {
    QList<Node> nodes; nodes.append(Node{});
    for (const auto &entry : work.entries) {
        work.check(); auto path = entry.path; while (path.endsWith('/')) path.chop(1); if (path.isEmpty()) continue;
        const auto segments = path.split('/'); int current = 0;
        for (const auto &segment : segments) {
            if (!nodes[current].indexes.contains(segment)) {
                if (nodes.size() > work.limits.maxTreeNodes) fail("Archive tree-node limit exceeded.", "limit");
                const int index = nodes.size(); nodes[current].indexes.insert(segment, index); nodes[current].children.append(index);
                Node node; node.name = segment; nodes.append(std::move(node));
            }
            current = nodes[current].indexes.value(segment);
        }
        nodes[current].entry = entry; // The source's last duplicate entry wins in the tree.
    }
    QCollator collator(QLocale(QLocale::English, QLocale::UnitedStates));
    auto directory = [&](int index) { return nodes[index].entry ? nodes[index].entry->directory : !nodes[index].children.isEmpty(); };
    QStringList lines; int displayed = 0;
    std::function<void(int, int)> visit = [&](int parent, int level) {
        work.check(); auto children = nodes[parent].children;
        std::stable_sort(children.begin(), children.end(), [&](int a, int b) { if (directory(a) != directory(b)) return directory(a); return collator.compare(nodes[a].name, nodes[b].name) < 0; });
        const int cap = level == 0 ? 20 : level == 1 ? 10 : 5; const QString indent(level * 2, ' ');
        for (int position = 0; position < qMin(cap, int(children.size())); ++position) {
            if (displayed >= 500) { *truncated = true; return; }
            const int index = children[position]; const bool dir = directory(index);
            lines.append(indent + textName(nodes[index].name) + (dir ? "/" : "  (size:" + formatBytes(nodes[index].entry ? nodes[index].entry->size : 0) + ')')); ++displayed;
            if (dir && level < depth) visit(index, level + 1);
        }
        if (children.size() > cap) { lines.append(indent + QString("… +%1 more").arg(children.size() - cap)); *truncated = true; }
    };
    visit(0, 0); if (*truncated) lines.append("… (tree truncated at 500 displayed entries)"); return lines.join('\n');
}
bool valid(const ArchiveLimits &l) {
    return l.maxInputBytes > 0 && l.maxInputBytes <= 128 * 1024 * 1024 && l.maxInflatedBytes > 0 && l.maxInflatedBytes <= 256 * 1024 * 1024
        && l.maxEntries > 0 && l.maxEntries <= 2000 && l.maxHeaders > 0 && l.maxHeaders <= 100000
        && l.maxTreeNodes > 0 && l.maxTreeNodes <= 100000 && l.maxPathDepth > 0 && l.maxPathDepth <= 64
        && l.maxNameBytes > 0 && l.maxNameBytes <= 65535 && l.maxOutputChars > 0 && l.maxOutputChars <= 4 * 1024 * 1024
        && l.maxInflateCalls > 0 && l.maxInflateCalls <= 65536 && l.maxGzipMembers > 0 && l.maxGzipMembers <= 128;
}
}
FileTransformResult ArchiveTransformer::fromBytes(const QByteArray &data, const FileDescriptor &file,
    const FileReadParams &params, const ArchiveLimits &limits, const std::atomic_bool *cancelled) {
    FileTransformResult result; result.requested = params;
    const auto name = (file.originalFileName.isEmpty() ? file.mountedPath : file.originalFileName).toLower();
    const auto ext = name.mid(name.lastIndexOf('.'));
    const QString type = QStringList{".zip", ".jar", ".war"}.contains(ext) ? "zip" : ext == ".tar" ? "tar" : (ext == ".tgz" || name.endsWith(".tar.gz")) ? "tar.gz" : "archive";
    result.metadata = {{"size", formatBytes(file.size >= 0 ? quint64(file.size) : quint64(data.size()))}, {"format", type}};
    if (file.modified.isValid()) result.metadata["modified"] = file.modified.toUTC().toString(Qt::ISODateWithMs);
    try {
        if (!valid(limits) || file.size < -1 || file.mountedPath.size() > 4096 || file.originalFileName.size() > 4096
            || file.mountedPath.contains(QChar::Null) || file.originalFileName.contains(QChar::Null)) fail("Invalid archive descriptor or limits.", "limit");
        if (data.size() > limits.maxInputBytes) fail("Archive exceeds its captured-input limit.", "limit");
        const double depth = params.preview.value_or(false) ? 0 : params.depth.value_or(4);
        if (!std::isfinite(depth) || depth < 0 || depth > limits.maxPathDepth) fail("Invalid or excessive archive listing depth.", "limit");
        Work work{limits, cancelled, {}, {}, false, false, false}; work.check();
        if (data.isEmpty()) fail("Empty archive file.", "empty");
        if (type == "zip") listZip(data, work);
        else if (type == "tar.gz") listGzip(data, work);
        else if (type == "tar") { TarReader reader(work); reader.feed(data.constData(), data.size()); reader.finish(); }
        else fail("This archive type is not implemented; supported listings are ZIP/JAR/WAR, TAR and TGZ/TAR.GZ.", "unsupported");
        quint64 total = 0; int files = 0, directories = 0;
        for (const auto &entry : work.entries) { if (entry.size > std::numeric_limits<quint64>::max() - total) fail("Archive uncompressed-size total overflows."); total += entry.size; entry.directory ? ++directories : ++files; }
        result.metadata["files"] = QString::number(files); result.metadata["directories"] = QString::number(directories);
        result.metadata["totalEntries"] = QString::number(work.entries.size()); result.metadata["uncompressedSize"] = formatBytes(total);
        if (work.entryLimit) result.metadata["truncated"] = QString("%1+ entries").arg(limits.maxEntries);
        if (work.gzipPartial) result.metadata["gzipIntegrity"] = "not-verified-after-entry-limit";
        if (work.encrypted) result.metadata["encryptedContent"] = "listing-only";
        if (!work.encodedMethods.isEmpty()) { auto values = work.encodedMethods.values(); std::sort(values.begin(), values.end()); QStringList methods; for (int method : values) methods.append(QString::number(method)); result.metadata["unsupportedContentMethods"] = methods.join(','); }
        auto output = render(work, depth, &result.truncated);
        result.truncated |= work.entryLimit;
        if (params.preview.value_or(false)) { result.metadata["preview"] = "true"; FileReadParams delivered; delivered.preview = true; delivered.depth = 0; result.effective = delivered; }
        if (output.size() > limits.maxOutputChars) {
            const auto notice = QString("\n… (native archive output limit reached)").left(limits.maxOutputChars);
            auto count = limits.maxOutputChars - notice.size(); if (count > 0 && output[count - 1].isHighSurrogate()) --count;
            output = output.left(count) + notice; result.truncated = true; result.coverageKnown = false; result.metadata["partialContent"] = "true";
            FileReadParams delivered; delivered.preview = params.preview; result.effective = delivered;
        }
        result.parts = {QJsonObject{{"type", "text"}, {"text", output}}};
    } catch (const Failure &failure) {
        result.error = failure.message; result.metadata["error"] = failure.kind; result.coverageKnown = false;
        result.supported = failure.kind != "unsupported";
        const auto display = failure.message.left(qBound(1, limits.maxOutputChars, 4 * 1024 * 1024));
        result.parts = {QJsonObject{{"type", "text"}, {"text", display}}};
    }
    return result;
}
}
