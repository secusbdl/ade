#include "attachments/FileTransformer.h"
#include <QJsonDocument>
#include <QtTest>
using namespace ade;
namespace {
FileDescriptor source(QString name = "w1/file.txt") {
    return {name, {}, -1, QDateTime::fromString("2026-01-02T03:04:05.006Z", Qt::ISODateWithMs)};
}
QString text(const FileTransformResult &result) { return result.parts.first().toObject()["text"].toString(); }
QByteArray lines(int count, const QByteArray &value = "line") {
    return (value + '\n').repeated(count - 1) + value;
}
FileTreeEntry node(const QString &name, bool directory = false, QList<FileTreeEntry> children = {}) { return {name, directory, children, {}}; }
}
class StructuredReadTest : public QObject {
    Q_OBJECT
private slots:
    void currentReadAndListParameters();
    void invalidParametersAreAtomic_data();
    void invalidParametersAreAtomic();
    void textFormattingAndRanges();
    void tokenBudgetReportsActualRange();
    void promotionBoundaries_data();
    void promotionBoundaries();
    void promotionPreservesExplicitRange();
    void markdownSourceOutline();
    void markdownFullAndNoHeadingFallback();
    void svgRemainsText();
    void utf8AndBinaryFallback();
    void originalExtensionAndDeferredFormats();
    void directoryDepthSortingAndNoMutation();
    void directoryPerLevelAndTotalLimits();
    void directoryNamesAndMalformedManifest();
    void wrappersPreservePartsAndEscapeMetadata();
    void cancellationAndNativeBounds();
    void placeholderAndEnvelopeBounds();
};

void StructuredReadTest::currentReadAndListParameters() {
    FileReadParams result; QString error;
    QVERIFY(FileTransformer::readParams({{"start_line", 2}, {"end_line", 9}, {"start_page", 1}, {"end_page", 3}, {"preview", false}, {"depth", 99}}, &result, &error));
    QCOMPARE(result.startLine, std::optional<int>(2)); QCOMPARE(result.endPage, std::optional<int>(3));
    QCOMPARE(result.preview, std::optional<bool>(false)); QVERIFY(!result.depth);
    QVERIFY(FileTransformer::listParams({}, &result, &error)); QCOMPARE(result.depth, std::optional<double>(0)); QVERIFY(!result.startLine);
    QVERIFY(FileTransformer::listParams({{"depth", 2}, {"preview", true}}, &result)); QCOMPARE(result.depth, std::optional<double>(2)); QVERIFY(!result.preview);
    QVERIFY(FileTransformer::listParams({{"depth", 1.5}}, &result)); QCOMPARE(result.depth, std::optional<double>(1.5));
    QVERIFY(!FileTransformer::readParams({}, nullptr, &error)); QVERIFY(!error.isEmpty());
}
void StructuredReadTest::invalidParametersAreAtomic_data() {
    QTest::addColumn<QJsonObject>("arguments"); QTest::addColumn<bool>("list");
    QTest::newRow("negative-line") << QJsonObject{{"start_line", 0}} << false;
    QTest::newRow("fractional-line") << QJsonObject{{"end_line", 1.5}} << false;
    QTest::newRow("string-line") << QJsonObject{{"start_line", "1"}} << false;
    QTest::newRow("overflow") << QJsonObject{{"start_line", 1e20}} << false;
    QTest::newRow("reversed-line") << QJsonObject{{"start_line", 2}, {"end_line", 1}} << false;
    QTest::newRow("reversed-page") << QJsonObject{{"start_page", 4}, {"end_page", 3}} << false;
    QTest::newRow("invalid-preview") << QJsonObject{{"preview", 1}} << false;
    QTest::newRow("negative-depth") << QJsonObject{{"depth", -1}} << true;
    QTest::newRow("string-depth") << QJsonObject{{"depth", "1"}} << true;
}
void StructuredReadTest::invalidParametersAreAtomic() {
    QFETCH(QJsonObject, arguments); QFETCH(bool, list);
    FileReadParams result; result.startLine = 88; const auto before = result; QString error;
    QVERIFY(!(list ? FileTransformer::listParams(arguments, &result, &error) : FileTransformer::readParams(arguments, &result, &error)));
    QVERIFY(!error.isEmpty()); QVERIFY(result == before);
}
void StructuredReadTest::textFormattingAndRanges() {
    const auto bytes = QByteArray("alpha\r\nbeta\n"); const auto result = FileTransformer::fromBytes(bytes, source("w1/code.ts"));
    QVERIFY(result.error.isEmpty()); QCOMPARE(text(result), QString("1|alpha\r\n2|beta\n3|"));
    QCOMPARE(result.metadata["lines"].toString(), QString("3")); QCOMPARE(result.metadata["language"].toString(), QString("typescript"));
    QCOMPARE(result.metadata["modified"].toString(), QString("2026-01-02T03:04:05.006Z")); QVERIFY(!result.effective);
    FileReadParams params; params.startLine = 2; params.endLine = 99; params.preview = false;
    const auto range = FileTransformer::fromBytes(bytes, source(), params);
    QCOMPARE(text(range), QString("2|beta\n3|")); QCOMPARE(range.delivered().endLine, std::optional<int>(3));
    params.startLine = 8; const auto beyond = FileTransformer::fromBytes(bytes, source(), params);
    QCOMPARE(text(beyond), QString("8|")); QCOMPARE(beyond.delivered().endLine, std::optional<int>(7));
    QCOMPARE(text(FileTransformer::fromBytes({}, source())), QString("1|"));
}
void StructuredReadTest::tokenBudgetReportsActualRange() {
    FileTransformLimits limits; limits.maxReadChars = 7;
    const auto all = FileTransformer::fromBytes("abcd\nef\ngh", source(), {}, limits);
    QCOMPARE(text(all), QString("1|abcd\n… (truncated — token budget reached, 2 more lines remaining)"));
    QVERIFY(all.truncated); QCOMPARE(all.delivered().startLine, std::optional<int>(1)); QCOMPARE(all.delivered().endLine, std::optional<int>(1));
    FileReadParams params; params.startLine = 2; params.endLine = 4; limits.maxReadChars = 3;
    const auto range = FileTransformer::fromBytes("a\nbb\ncc\ndd", source(), params, limits);
    QCOMPARE(text(range), QString("2|bb\n… (truncated — token budget reached, 2 more lines until line 4)"));
    QCOMPARE(range.delivered().endLine, std::optional<int>(2));
    limits.maxReadChars = 1;
    QCOMPARE(text(FileTransformer::fromBytes("a longer first line", source(), {}, limits)), QString("1|a longer first line"));
}
void StructuredReadTest::promotionBoundaries_data() {
    QTest::addColumn<QByteArray>("bytes"); QTest::addColumn<bool>("promoted");
    QTest::newRow("empty") << QByteArray() << true;
    QTest::newRow("6144-bytes") << QByteArray(6144, 'a') << true;
    QTest::newRow("6145-bytes") << QByteArray(6145, 'a') << false;
    QTest::newRow("150-lines") << lines(150, "x") << true;
    QTest::newRow("151-lines") << lines(151, "x") << false;
    QTest::newRow("trailing-newline-is-line") << (lines(150, "x") + '\n') << false;
}
void StructuredReadTest::promotionBoundaries() {
    QFETCH(QByteArray, bytes); QFETCH(bool, promoted);
    FileReadParams params; params.preview = true; FileTransformLimits limits; limits.maxPreviewLines = 2;
    const auto result = FileTransformer::fromBytes(bytes, source(), params, limits);
    QCOMPARE(result.previewPromoted, promoted); QCOMPARE(result.delivered().preview.value_or(false), !promoted);
    QCOMPARE(result.metadata["preview"].toString(), promoted ? QString() : QString("true"));
}
void StructuredReadTest::promotionPreservesExplicitRange() {
    FileReadParams params; params.startLine = 2; params.endLine = 3; params.preview = true;
    const auto result = FileTransformer::fromBytes("one\ntwo\nthree\nfour", source(), params);
    QVERIFY(result.previewPromoted); QCOMPARE(text(result), QString("2|two\n3|three"));
    QCOMPARE(result.delivered().startLine, std::optional<int>(2)); QVERIFY(!result.delivered().preview);
}
void StructuredReadTest::markdownSourceOutline() {
    QByteArray bytes("# Top #\nintro\n## Child\nchild\n### Deep\n```\n# Fenced heading\n```\nSetext\n======\n## After\n");
    bytes += lines(151, "padding"); // Prevent small-file promotion.
    FileReadParams params; params.preview = true;
    const auto result = FileTransformer::fromBytes(bytes, source("w1/README.md"), params);
    QVERIFY(!result.previewPromoted); QCOMPARE(result.metadata["format"].toString(), QString("markdown"));
    const QString outline = "<outline>\n# Top [lines 1–6]\n  ## Child [lines 3–6]\n    ### Deep [lines 5–6]\n# Fenced heading [lines 7–162]\n  ## After [lines 11–162]\n</outline>\n\n";
    QVERIFY2(text(result).startsWith(outline), qPrintable(text(result)));
    QVERIFY(!text(result).contains("# Setext")); QVERIFY(text(result).contains("15|padding\n… (147 more lines)"));
    QCOMPARE(result.delivered().endLine, std::optional<int>(15));
}
void StructuredReadTest::markdownFullAndNoHeadingFallback() {
    const auto full = FileTransformer::fromBytes("# Title\nbody", source("w1/README.mdx"));
    QCOMPARE(text(full), QString("1|# Title\n2|body"));
    FileReadParams params; params.preview = true;
    const auto plain = FileTransformer::fromBytes(lines(151), source("w1/no-headings.md"), params);
    QVERIFY(!text(plain).contains("<outline>")); QCOMPARE(plain.delivered().endLine, std::optional<int>(15));
}
void StructuredReadTest::svgRemainsText() {
    const auto svg = FileTransformer::fromBytes("<svg><path d=\"M0 0\"/></svg>", source("att/random"));
    auto descriptor = source("att/random"); descriptor.originalFileName = "Vector.SVG";
    const auto result = FileTransformer::fromBytes("<svg><path d=\"M0 0\"/></svg>", descriptor);
    QCOMPARE(result.metadata["format"].toString(), QString("svg")); QCOMPARE(result.metadata["language"].toString(), QString("xml"));
    QCOMPARE(result.parts.size(), 1); QCOMPARE(result.parts.first().toObject()["type"].toString(), QString("text"));
    QCOMPARE(text(result), text(svg));
}
void StructuredReadTest::utf8AndBinaryFallback() {
    const QByteArray bytes = QByteArray::fromHex("efbbbf") + QString::fromUtf8("é😀�\n").toUtf8();
    const auto valid = FileTransformer::fromBytes(bytes, source());
    QCOMPARE(text(valid), "1|" + QString(QChar(0xfeff)) + QString::fromUtf8("é😀�\n2|"));
    QVERIFY(!valid.metadata.contains("format"));
    for (const auto &binary : {QByteArray::fromHex("41e282"), QByteArray::fromHex("fffe4100"), QByteArray(8200, 'x') + QByteArray(1, '\0')}) {
        const auto result = FileTransformer::fromBytes(binary, source("w1/misnamed.md"));
        QVERIFY(!result.metadata.contains("format")); QVERIFY(text(result).startsWith("Binary file"));
    }
}
void StructuredReadTest::originalExtensionAndDeferredFormats() {
    auto descriptor = source("att/blob-id"); descriptor.originalFileName = "report.DOCX";
    const auto office = FileTransformer::fromBytes("ASCII content is still a known binary format", descriptor);
    QVERIFY(office.supported); QCOMPARE(office.metadata["format"].toString(), QString("binary")); QVERIFY(!text(office).contains("ASCII"));
    FileReadParams preview; preview.preview = true;
    const auto promotedBinary = FileTransformer::fromBytes("ASCII", descriptor, preview);
    QVERIFY(promotedBinary.previewPromoted); QVERIFY(!promotedBinary.delivered().preview);
    for (const auto *name : {"file.pdf", "file.nef", "file.iso", "file.png"}) {
        const auto result = FileTransformer::fromBytes("plausible text bytes", source(name));
        QVERIFY(!result.supported); QCOMPARE(result.metadata["format"].toString(), QString("unavailable-transform"));
    }
    const auto unknown = FileTransformer::fromBytes("text", source("w1/unknown.suffix")); QCOMPARE(text(unknown), QString("1|text"));
}
void StructuredReadTest::directoryDepthSortingAndNoMutation() {
    const QList<FileTreeEntry> entries{node("z.txt"), node("b", true, {node("child", true, {node("deep.txt")})}), node("a", true), node("a.txt")};
    const auto normal = FileTransformer::fromDirectory(entries, source("w1"));
    QCOMPARE(text(normal), QString("a/\nb/\n  child/\n    deep.txt\na.txt\nz.txt"));
    QCOMPARE(normal.metadata["depth"].toString(), QString("3")); QCOMPARE(normal.metadata["entries"].toString(), QString("6"));
    FileReadParams params; QVERIFY(FileTransformer::listParams({}, &params));
    QCOMPARE(text(FileTransformer::fromDirectory(entries, source("w1"), params)), QString("a/\nb/\na.txt\nz.txt"));
    params.depth = 9; params.preview = true;
    const auto preview = FileTransformer::fromDirectory(entries, source("w1"), params);
    QCOMPARE(preview.delivered().depth, std::optional<double>(0)); QCOMPARE(preview.metadata["depth"].toString(), QString("1"));
    params.preview = false; params.depth = 1.5;
    const auto fractional = FileTransformer::fromDirectory(entries, source("w1"), params);
    QCOMPARE(text(fractional), text(normal)); QCOMPARE(fractional.metadata["depth"].toString(), QString("2.5"));
    QCOMPARE(entries.first().name, QString("z.txt")); QCOMPARE(entries[1].children.first().children.first().name, QString("deep.txt"));
}
void StructuredReadTest::directoryPerLevelAndTotalLimits() {
    QList<FileTreeEntry> entries;
    for (int index = 0; index < 21; ++index) entries.append(node(QString("f%1").arg(index, 2, 10, QChar('0'))));
    const auto top = FileTransformer::fromDirectory(entries, source());
    QCOMPARE(top.metadata["entries"].toString(), QString("20")); QVERIFY(top.truncated);
    QVERIFY(text(top).contains("… +1 more\n... (truncated at 200 entries)"));
    QList<FileTreeEntry> children; for (int index = 0; index < 11; ++index) children.append(node(QString::number(index)));
    const auto perLevel = FileTransformer::fromDirectory({node("root", true, children)}, source());
    QCOMPARE(perLevel.metadata["entries"].toString(), QString("11")); QVERIFY(text(perLevel).contains("  … +1 more"));
    entries.clear();
    for (int index = 0; index < 20; ++index) entries.append(node(QString("d%1").arg(index), true, children));
    const auto total = FileTransformer::fromDirectory(entries, source());
    QCOMPARE(total.metadata["entries"].toString(), QString("200")); QVERIFY(total.truncated);
}
void StructuredReadTest::directoryNamesAndMalformedManifest() {
    const auto result = FileTransformer::fromDirectory({node("line\nbreak"), node("back\\slash")}, source());
    QVERIFY(text(result).contains("line\\nbreak")); QVERIFY(text(result).contains("back\\\\slash"));
    QVERIFY(!FileTransformer::fromDirectory({node("file", false, {node("invalid-child")})}, source()).error.isEmpty());
    QVERIFY(!FileTransformer::fromDirectory({node(QString(5000, 'x'))}, source()).error.isEmpty());
}
void StructuredReadTest::wrappersPreservePartsAndEscapeMetadata() {
    auto descriptor = source("w1/a<&\"'.md"); auto result = FileTransformer::fromBytes("content", descriptor);
    result.metadata["custom"] = "<unsafe>&"; const auto before = result.parts;
    const auto wrapped = FileTransformer::wrappedParts(descriptor, result); const auto content = wrapped.first().toObject()["text"].toString();
    QVERIFY(content.contains("path=\"w1/a&lt;&amp;&quot;&apos;.md\"")); QVERIFY(content.contains("custom:&lt;unsafe&gt;&amp;"));
    QCOMPARE(result.parts, before); QVERIFY(content.contains("<content>\n1|content\n</content>"));
    FileReadParams params; params.startLine = 1; params.endLine = 1; params.preview = false;
    const auto exactRange = FileTransformer::fromBytes("line", source(), params);
    QVERIFY(!FileTransformer::wrappedParts(source(), exactRange).first().toObject()["text"].toString().contains("truncated=\"true\""));
    const QJsonObject image{{"type", "image"}, {"mediaType", "image/png"}, {"dataBase64", "fixture"}};
    result.parts = {image}; const auto mixed = FileTransformer::wrappedParts(descriptor, result);
    QCOMPARE(mixed.size(), 3); QCOMPARE(mixed[1].toObject(), image); QCOMPARE(result.parts, QJsonArray{image});
}
void StructuredReadTest::cancellationAndNativeBounds() {
    std::atomic_bool cancelled{true};
    QVERIFY(FileTransformer::fromBytes("text", source(), {}, {}, &cancelled).error.contains("cancelled"));
    QVERIFY(!FileTransformer::fromBytes("text", source(), {}, {}, &cancelled).coverageKnown);
    QVERIFY(FileTransformer::fromDirectory({}, source(), {}, {}, &cancelled).error.contains("cancelled"));
    FileTransformLimits limits; limits.maxFileBytes = 3;
    QVERIFY(!FileTransformer::fromBytes("four", source(), {}, limits).error.isEmpty());
    limits = {}; limits.maxTextLines = 2; QVERIFY(!FileTransformer::fromBytes("a\nb\nc", source(), {}, limits).error.isEmpty());
    limits = {}; limits.maxTreeNodes = 1; QVERIFY(!FileTransformer::fromDirectory({node("a"), node("b")}, source(), {}, limits).error.isEmpty());
    limits = {}; limits.maxTreeDepth = 0; FileReadParams params; params.depth = 1;
    QVERIFY(!FileTransformer::fromDirectory({}, source(), params, limits).error.isEmpty());
    limits = {}; limits.maxOutputChars = 256;
    const auto hardCut = FileTransformer::fromBytes(QString(1000, QChar(0x61)).toUtf8(), source(), {}, limits);
    QVERIFY(hardCut.truncated); QVERIFY(!hardCut.coverageKnown); QVERIFY(!hardCut.delivered().endLine);
    QCOMPARE(hardCut.metadata["partialContent"].toString(), QString("true")); QVERIFY(text(hardCut).size() <= 256);
    limits.maxReadChars = 0; QVERIFY(!FileTransformer::fromBytes("a", source(), {}, limits).error.isEmpty());
}
void StructuredReadTest::placeholderAndEnvelopeBounds() {
    auto descriptor = source("w1/" + QString(4000, 'x') + ".bin");
    FileTransformLimits limits; limits.maxOutputChars = 256;
    const auto result = FileTransformer::fromBytes(QByteArray::fromHex("00ff"), descriptor, {}, limits);
    QVERIFY(text(result).size() <= limits.maxOutputChars); QVERIFY(result.truncated); QVERIFY(!result.coverageKnown);
    // The documented cap applies to the inner body; the envelope preserves the
    // bounded path and metadata without cutting XML or pretending full coverage.
    const auto wrapped = FileTransformer::wrappedParts(descriptor, result).first().toObject()["text"].toString();
    QVERIFY(wrapped.contains(descriptor.mountedPath)); QVERIFY(wrapped.endsWith("</file>"));
    QVERIFY(wrapped.size() < limits.maxOutputChars + 5000);
    descriptor = source();
    const auto unicode = FileTransformer::fromBytes(QString::fromUtf8("😀").repeated(1000).toUtf8(), descriptor, {}, limits);
    const auto output = text(unicode); QVERIFY(output.size() <= limits.maxOutputChars);
    QVERIFY(!output.contains(QChar::ReplacementCharacter)); QVERIFY(output.toUtf8().contains(QByteArray::fromHex("f09f9880")));
    limits.maxOutputChars = 1;
    QCOMPARE(text(FileTransformer::fromBytes("oversized", descriptor, {}, limits)).size(), 1);
}
QTEST_GUILESS_MAIN(StructuredReadTest)
#include "test_file_transformer.moc"
