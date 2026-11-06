#include "FileTransformer.h"
#include "archive/ArchiveTransformer.h"
#include <QCollator>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace ade {
namespace {
QJsonObject textPart(const QString &text) { return {{"type", "text"}, {"text", text}}; }
QString escaped(QString value) {
    value.replace('&', "&amp;"); value.replace('<', "&lt;"); value.replace('>', "&gt;");
    value.replace('"', "&quot;"); value.replace('\'', "&apos;");
    return value;
}
QString displayName(QString value) {
    value.replace('\\', "\\\\"); value.replace('\n', "\\n"); value.replace('\r', "\\r");
    value.replace('\t', "\\t"); return value;
}
QString extension(const FileDescriptor &file) {
    const auto name = file.originalFileName.isEmpty() ? file.mountedPath : file.originalFileName;
    const auto component = name.mid(qMax(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
    const auto dot = component.lastIndexOf('.');
    return dot <= 0 ? QString() : component.mid(dot).toLower();
}
QString formatBytes(qint64 bytes) {
    if (bytes == 0) return "0B";
    const QStringList units{"B", "KB", "MB", "GB"};
    int index = 0; double value = double(bytes);
    while (value >= 1024 && index < 3) { value /= 1024; ++index; }
    return (value < 10 ? QString::number(value, 'f', 1) : QString::number(qRound64(value))) + units[index];
}
const QMap<QString, QString> Languages{
#include "FileLanguageEntries.inc"
};
const QSet<QString> KnownBinary{
#include "FileBinaryExtensions.inc"
};
const QSet<QString> Advanced{
    ".pdf", ".zip", ".jar", ".war", ".tar", ".tgz", ".iso", ".img", ".dmg",
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".avif", ".ico",
    ".nef", ".cr2", ".cr3", ".arw", ".dng", ".orf", ".rw2", ".raf", ".pef",
    ".srw", ".erf", ".3fr", ".rwl", ".mrw", ".nrw", ".raw"};
bool cancelledNow(const std::atomic_bool *cancelled) { return cancelled && cancelled->load(std::memory_order_relaxed); }
bool validLimits(const FileTransformLimits &limits) {
    return limits.maxReadChars > 0 && limits.maxReadChars <= 4 * 1024 * 1024
        && limits.maxPreviewLines > 0 && limits.maxPreviewLines <= 10000
        && limits.maxFileBytes > 0 && limits.maxFileBytes <= 128 * 1024 * 1024
        && limits.maxOutputChars > 0 && limits.maxOutputChars <= 4 * 1024 * 1024
        && limits.maxTextLines > 0 && limits.maxTextLines <= 1000000
        && limits.maxTreeNodes > 0 && limits.maxTreeNodes <= 100000
        && limits.maxTreeDepth >= 0 && limits.maxTreeDepth <= 64;
}
QString validateParams(const FileReadParams &params) {
    for (const auto value : {params.startLine, params.endLine, params.startPage, params.endPage})
        if (value && *value < 1) return "Line and page numbers must be positive integers.";
    if (params.depth && (!std::isfinite(*params.depth) || *params.depth < 0)) return "Directory depth must be finite and nonnegative.";
    if (params.startLine && params.endLine && *params.startLine > *params.endLine) return "end_line must be equal or larger than start_line.";
    if (params.startPage && params.endPage && *params.startPage > *params.endPage) return "end_page must be equal or larger than start_page.";
    return {};
}
bool sameParams(const FileReadParams &a, const FileReadParams &b) {
    return a.startLine == b.startLine && a.endLine == b.endLine && a.startPage == b.startPage
        && a.endPage == b.endPage && a.depth == b.depth && a.preview.value_or(false) == b.preview.value_or(false);
}
FileTransformResult failure(const FileReadParams &params, const QString &error) {
    FileTransformResult result; result.requested = params; result.error = error;
    result.coverageKnown = false;
    result.metadata = {{"error", "true"}}; result.parts = {textPart(error)}; return result;
}
QJsonObject metadata(const FileDescriptor &file, qint64 byteSize) {
    QJsonObject result{{"size", formatBytes(byteSize)}};
    if (file.modified.isValid()) result["modified"] = file.modified.toUTC().toString(Qt::ISODateWithMs);
    return result;
}
int fittingLines(const QStringList &lines, int start, int end, int budget) {
    qint64 chars = 0;
    for (int index = start; index < end; ++index) {
        const qint64 cost = lines[index].size() + 1;
        if (chars + cost > budget && index > start) return index - start;
        chars += cost;
    }
    return end - start;
}
QString numbered(const QStringList &lines, int begin, int end, int lineStart) {
    QStringList output; output.reserve(qMax(1, end - begin));
    if (begin >= end) return QString::number(lineStart) + '|'; // Source prefixLineNumbers("").
    for (int index = begin; index < end; ++index) output.append(QString::number(lineStart + index - begin) + '|' + lines[index]);
    return output.join('\n');
}
void capOutput(FileTransformResult &result, QString output, const FileTransformLimits &limits) {
    if (output.size() > limits.maxOutputChars) {
        static const QString notice = "\n… (native output limit reached; final displayed line may be partial)";
        const auto suffix = notice.left(limits.maxOutputChars);
        auto end = limits.maxOutputChars - suffix.size();
        if (end > 0 && output[end - 1].isHighSurrogate()) --end;
        output = output.left(qMax<qsizetype>(0, end)) + suffix;
        result.truncated = true; result.metadata["nativeOutputLimit"] = "true";
        // The normal effective range describes selection, not full delivery after
        // a hard character cut. Mark it partial so callers cannot claim coverage.
        result.metadata["partialContent"] = "true";
        result.coverageKnown = false;
        FileReadParams partial; partial.preview = result.delivered().preview;
        result.effective = partial; // Never expose an endLine that the cut did not fully deliver.
    }
    result.parts = {textPart(output)};
}
struct Heading { int level, start, end; QString title; };
QString markdownOutline(const QStringList &lines, const std::atomic_bool *cancelled) {
    const QRegularExpression heading(QStringLiteral("^(#{1,6})\\s+(.+?)(?:\\s+#+)?\\s*$"), QRegularExpression::UseUnicodePropertiesOption);
    QList<Heading> headings; QList<int> open;
    for (int index = 0; index < lines.size(); ++index) {
        if (cancelledNow(cancelled)) return {};
        const auto match = heading.match(lines[index]);
        if (!match.hasMatch()) continue;
        const auto level = match.captured(1).size();
        while (!open.isEmpty() && headings[open.last()].level >= level) headings[open.takeLast()].end = index;
        headings.append({int(level), index + 1, int(lines.size()), match.captured(2).trimmed()});
        open.append(headings.size() - 1);
    }
    if (headings.isEmpty()) return {};
    QStringList output{"<outline>"};
    for (const auto &entry : headings)
        output.append(QString((entry.level - 1) * 2, ' ') + QString(entry.level, '#') + ' ' + entry.title
            + QString(" [lines %1–%2]").arg(entry.start).arg(entry.end));
    output.append("</outline>"); return output.join('\n');
}
bool optionalInteger(const QJsonObject &args, const QString &name, int minimum, std::optional<int> *out, QString *error) {
    const auto value = args[name]; if (value.isUndefined()) return true;
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < minimum || number > std::numeric_limits<int>::max() || std::floor(number) != number) {
        if (error) *error = name + " must be an integer in its supported range.";
        return false;
    }
    *out = int(number); return true;
}
bool validInput(const FileDescriptor &file, const FileReadParams &params, const FileTransformLimits &limits, QString *error) {
    if (!validLimits(limits)) *error = "Invalid native file-transform limits.";
    else if (file.mountedPath.size() > 4096 || file.originalFileName.size() > 4096 || file.mountedPath.contains(QChar::Null) || file.originalFileName.contains(QChar::Null) || file.size < -1)
        *error = "Invalid captured file descriptor.";
    else *error = validateParams(params);
    return error->isEmpty();
}
}

QJsonObject FileReadParams::toJson() const {
    QJsonObject result;
    if (startLine) result["startLine"] = *startLine;
    if (endLine) result["endLine"] = *endLine;
    if (startPage) result["startPage"] = *startPage;
    if (endPage) result["endPage"] = *endPage;
    if (depth) result["depth"] = *depth;
    if (preview) result["preview"] = *preview;
    return result;
}
bool FileTransformer::readParams(const QJsonObject &args, FileReadParams *result, QString *error) {
    if (error) error->clear();
    if (!result) { if (error) *error = "Missing parameter destination."; return false; }
    FileReadParams parsed;
    for (auto pair : {qMakePair(QString("start_line"), &parsed.startLine), qMakePair(QString("end_line"), &parsed.endLine),
                      qMakePair(QString("start_page"), &parsed.startPage), qMakePair(QString("end_page"), &parsed.endPage)})
        if (!optionalInteger(args, pair.first, 1, pair.second, error)) return false;
    if (!args["preview"].isUndefined()) {
        if (!args["preview"].isBool()) { if (error) *error = "preview must be boolean."; return false; }
        parsed.preview = args["preview"].toBool();
    }
    const auto invalid = validateParams(parsed); if (!invalid.isEmpty()) { if (error) *error = invalid; return false; }
    *result = parsed; return true; // As source Zod parsing does, ignore unrelated keys; no depth for read.
}
bool FileTransformer::listParams(const QJsonObject &args, FileReadParams *result, QString *error) {
    if (error) error->clear();
    if (!result) { if (error) *error = "Missing parameter destination."; return false; }
    FileReadParams parsed; parsed.depth = 0;
    if (!args["depth"].isUndefined()) {
        const auto depth = args["depth"].toDouble(-1);
        if (!args["depth"].isDouble() || !std::isfinite(depth) || depth < 0) {
            if (error) *error = "depth must be finite and nonnegative.";
            return false;
        }
        parsed.depth = depth;
    }
    *result = parsed; return true;
}

FileTransformResult FileTransformer::fromBytes(const QByteArray &bytes, const FileDescriptor &file,
    const FileReadParams &params, const FileTransformLimits &limits, const std::atomic_bool *cancelled) {
    QString invalid;
    if (!validInput(file, params, limits, &invalid)) return failure(params, invalid);
    if (cancelledNow(cancelled)) return failure(params, "File transformation cancelled.");
    if (bytes.size() > limits.maxFileBytes) return failure(params, "Captured file exceeds the native byte limit.");
    FileTransformResult result; result.requested = params; result.metadata = metadata(file, file.size < 0 ? bytes.size() : file.size);
    const auto ext = extension(file);
    const auto lowerName = (file.originalFileName.isEmpty() ? file.mountedPath : file.originalFileName).toLower();
    const bool archive = QStringList{".zip", ".jar", ".war", ".tar", ".tgz"}.contains(ext) || lowerName.endsWith(".tar.gz");
    // The source promotion precedes extension dispatch, including known binary
    // extensions whose captured bytes happen to be small valid UTF-8.
    const bool mayPromote = params.preview.value_or(false) && bytes.size() <= 6 * 1024;
    QString text; bool binary = false;
    if (mayPromote || (!archive && !KnownBinary.contains(ext) && !Advanced.contains(ext))) {
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
        text = decoder(bytes); binary = bytes.contains('\0') || decoder.hasError();
    }
    if (mayPromote && !binary && text.count('\n') < 150) {
        result.requested.preview.reset(); result.previewPromoted = true;
    }
    if (archive) {
        ArchiveLimits archiveLimits; archiveLimits.maxInputBytes = limits.maxFileBytes;
        archiveLimits.maxOutputChars = limits.maxOutputChars; archiveLimits.maxTreeNodes = limits.maxTreeNodes;
        archiveLimits.maxPathDepth = qMax(1, limits.maxTreeDepth);
        auto transformed = ArchiveTransformer::fromBytes(bytes, file, result.requested, archiveLimits, cancelled);
        transformed.previewPromoted = result.previewPromoted; return transformed;
    }
    if (KnownBinary.contains(ext)) {
        result.metadata["format"] = "binary";
        capOutput(result, "Unsupported binary file (" + ext + "). Use fs.readFile('" + file.mountedPath + "') in the sandbox to access raw bytes.", limits); return result;
    }
    if (Advanced.contains(ext)) {
        result.supported = false; result.metadata["format"] = "unavailable-transform";
        capOutput(result, "This file format requires a transformer outside the structured-text slice (" + ext + ").", limits); return result;
    }
    if (binary) {
        capOutput(result, QStringLiteral("Binary file — cannot be displayed inline. Use fs.readFile('") + file.mountedPath + "') in the sandbox to access raw bytes.", limits); return result;
    }
    if (text.count('\n') >= limits.maxTextLines) return failure(params, "Captured text exceeds the native line-count limit.");
    const auto lines = text.split('\n');
    const auto language = Languages.value(ext); if (!language.isEmpty()) result.metadata["language"] = language;
    result.metadata["lines"] = QString::number(lines.size()); result.metadata["chars"] = QString::number(text.size());
    const bool markdown = ext == ".md" || ext == ".mdx";
    if (markdown) result.metadata["format"] = "markdown";
    if (ext == ".svg") { result.metadata["format"] = "svg"; result.metadata["language"] = "xml"; }
    QString output;
    if (result.requested.preview.value_or(false)) {
        const int end = qMin(int(lines.size()), markdown ? 15 : limits.maxPreviewLines);
        if (markdown) {
            output = markdownOutline(lines, cancelled);
            if (cancelledNow(cancelled)) return failure(params, "File transformation cancelled.");
            if (!output.isEmpty()) output += "\n\n";
        }
        output += numbered(lines, 0, end, 1);
        if (lines.size() > end) output += QString("\n… (%1 more lines)").arg(lines.size() - end);
        result.metadata["preview"] = "true"; FileReadParams effective; effective.preview = true;
        effective.startLine = 1; effective.endLine = end; result.effective = effective;
        result.truncated = lines.size() > end;
    } else {
        const bool range = result.requested.startLine || result.requested.endLine;
        const int start = result.requested.startLine.value_or(1);
        const int requestedEnd = qMin(int(lines.size()), result.requested.endLine.value_or(int(lines.size())));
        const int begin = qMin(start - 1, int(lines.size()));
        const int count = fittingLines(lines, begin, qMax(begin, requestedEnd), limits.maxReadChars);
        const int end = start - 1 + count;
        output = numbered(lines, begin, begin + count, start);
        result.truncated = end < requestedEnd;
        if (range || result.truncated) { FileReadParams effective; effective.startLine = start; effective.endLine = end; result.effective = effective; }
        if (result.truncated) output += range
            ? QString("\n… (truncated — token budget reached, %1 more lines until line %2)").arg(requestedEnd - end).arg(requestedEnd)
            : QString("\n… (truncated — token budget reached, %1 more lines remaining)").arg(lines.size() - end);
    }
    if (cancelledNow(cancelled)) return failure(params, "File transformation cancelled.");
    capOutput(result, output, limits); return result;
}

FileTransformResult FileTransformer::fromDirectory(const QList<FileTreeEntry> &entries, const FileDescriptor &file,
    const FileReadParams &params, const FileTransformLimits &limits, const std::atomic_bool *cancelled) {
    QString invalid;
    if (!validInput(file, params, limits, &invalid)) return failure(params, invalid);
    if (cancelledNow(cancelled)) return failure(params, "Directory transformation cancelled.");
    const bool preview = params.preview.value_or(false); const double depth = preview ? 0 : params.depth.value_or(2);
    if (depth > limits.maxTreeDepth) return failure(params, "Requested directory depth exceeds the native limit.");
    // Validate the supplied manifest independently of visible output limits.
    int totalNodes = 0;
    std::function<bool(const QList<FileTreeEntry> &, int)> validate = [&](const QList<FileTreeEntry> &nodes, int level) {
        if (level > limits.maxTreeDepth || cancelledNow(cancelled)) return false;
        for (const auto &entry : nodes) {
            if (++totalNodes > limits.maxTreeNodes || entry.name.size() > 4096 || entry.name.isEmpty() || entry.name.contains(QChar::Null)) return false;
            if (!entry.children.isEmpty() && (!entry.isDirectory || !validate(entry.children, level + 1))) return false;
        }
        return true;
    };
    if (!validate(entries, 0)) return failure(params, cancelledNow(cancelled) ? "Directory transformation cancelled." : "Directory manifest exceeds native bounds or is malformed.");
    FileTransformResult result; result.requested = params; result.metadata = metadata(file, 0);
    result.metadata["size"] = QStringLiteral("—"); result.metadata["type"] = "directory";
    QCollator collator(QLocale(QLocale::English, QLocale::UnitedStates)); collator.setNumericMode(false);
    int emitted = 0; QStringList output;
    std::function<void(const QList<FileTreeEntry> &, int)> render = [&](const QList<FileTreeEntry> &nodes, int level) {
        if (cancelledNow(cancelled)) return;
        QList<const FileTreeEntry *> sorted; sorted.reserve(nodes.size()); for (const auto &entry : nodes) sorted.append(&entry);
        std::stable_sort(sorted.begin(), sorted.end(), [&](const FileTreeEntry *a, const FileTreeEntry *b) {
            if (a->isDirectory != b->isDirectory) return a->isDirectory;
            return collator.compare(a->name, b->name) < 0;
        });
        const int cap = level == 0 ? 20 : level == 1 ? 10 : 5;
        const int visible = qMin(int(sorted.size()), cap); const QString indent(level * 2, ' ');
        for (int index = 0; index < visible; ++index) {
            if (emitted >= 200) { result.truncated = true; return; }
            const auto &entry = *sorted[index]; output.append(indent + displayName(entry.name) + (entry.isDirectory ? "/" : "")); ++emitted;
            if (entry.isDirectory && !entry.children.isEmpty() && level < depth) render(entry.children, level + 1);
        }
        if (sorted.size() > cap) { output.append(indent + QString("… +%1 more").arg(sorted.size() - cap)); result.truncated = true; }
    };
    render(entries, 0);
    if (cancelledNow(cancelled)) return failure(params, "Directory transformation cancelled.");
    result.metadata["entries"] = QString::number(emitted); result.metadata["depth"] = QString::number(depth + 1);
    if (preview) { result.metadata["preview"] = "true"; FileReadParams effective; effective.preview = true; effective.depth = 0; result.effective = effective; }
    if (result.truncated) output.append("... (truncated at 200 entries)");
    capOutput(result, output.join('\n'), limits); return result;
}

QJsonArray FileTransformer::wrappedParts(const FileDescriptor &file, const FileTransformResult &result) {
    const auto delivered = result.delivered(); const bool directory = result.metadata["type"] == "directory";
    const auto tag = directory ? QString("dir") : QString("file");
    const auto inner = delivered.preview.value_or(false) ? QString("preview") : QString("content");
    QString attributes;
    if (!directory) {
        for (const auto &key : {"startLine", "endLine", "startPage", "endPage", "depth"})
            if (delivered.toJson().contains(key)) attributes += QString(" %1=\"%2\"").arg(key).arg(QString::number(delivered.toJson()[key].toDouble(), 'g', 15));
        if (delivered.preview.value_or(false)) attributes += " preview=\"true\"";
        if (result.truncated || (result.effective && !sameParams(*result.effective, result.requested))) attributes += " truncated=\"true\"";
    }
    QStringList metadataFields;
    for (auto it = result.metadata.begin(); it != result.metadata.end(); ++it) metadataFields.append(escaped(it.key()) + ':' + escaped(it.value().toString()));
    const auto open = '<' + tag + " path=\"" + escaped(file.mountedPath) + '\"' + attributes + ">\n<metadata>" + metadataFields.join('|') + "</metadata>\n<" + inner + ">\n";
    const auto close = "\n</" + inner + ">\n</" + tag + '>';
    auto parts = result.parts;
    if (!parts.isEmpty() && parts.first().toObject()["type"] == "text") { auto first = parts[0].toObject(); first["text"] = open + first["text"].toString(); parts[0] = first; }
    else parts.prepend(textPart(open));
    if (parts.last().toObject()["type"] == "text") { auto last = parts.last().toObject(); last["text"] = last["text"].toString() + close; parts[parts.size() - 1] = last; }
    else parts.append(textPart(close));
    return parts;
}
}
