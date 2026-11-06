#include "attachments/archive/ArchiveTransformer.h"
#include <QtZlib/zlib.h>
#include <QtEndian>
#include <QtTest>
#include <limits>
using namespace ade;
namespace {
void put16(QByteArray &b, qsizetype p, quint16 v) { qToLittleEndian<quint16>(v, b.data() + p); }
void put32(QByteArray &b, qsizetype p, quint32 v) { qToLittleEndian<quint32>(v, b.data() + p); }
void put64(QByteArray &b, qsizetype p, quint64 v) { qToLittleEndian<quint64>(v, b.data() + p); }
QByteArray field(quint16 id, const QByteArray &data) { QByteArray b(4, 0); put16(b, 0, id); put16(b, 2, data.size()); return b + data; }
struct Item {
    QByteArray name, payload, extra; quint64 size; quint16 flags, method; bool zip64;
    Item(QByteArray name = {}, QByteArray payload = "abc", QByteArray extra = {}, quint64 size = std::numeric_limits<quint64>::max(), quint16 flags = 0, quint16 method = 0, bool zip64 = false)
        : name(std::move(name)), payload(std::move(payload)), extra(std::move(extra)), size(size), flags(flags), method(method), zip64(zip64) {}
};
QByteArray zip(const QList<Item> &items, bool zip64 = false) {
    QByteArray result, central;
    for (const auto &item : items) {
        const quint64 size = item.size == std::numeric_limits<quint64>::max() ? item.payload.size() : item.size;
        const auto localOffset = result.size(); const auto crc = crc32(0, reinterpret_cast<const Bytef *>(item.payload.constData()), uInt(item.payload.size()));
        QByteArray local(30, 0); put32(local, 0, 0x04034b50); put16(local, 4, 20); put16(local, 6, item.flags); put16(local, 8, item.method);
        put32(local, 14, crc); put32(local, 18, item.payload.size()); put32(local, 22, quint32(size)); put16(local, 26, item.name.size());
        result += local + item.name + item.payload;
        auto extra = item.extra;
        if (item.zip64) { QByteArray values(24, 0); put64(values, 0, size); put64(values, 8, item.payload.size()); put64(values, 16, localOffset); extra += field(1, values); }
        QByteArray c(46, 0); put32(c, 0, 0x02014b50); put16(c, 4, 20); put16(c, 6, item.zip64 ? 45 : 20); put16(c, 8, item.flags); put16(c, 10, item.method);
        put32(c, 16, crc); put32(c, 20, item.zip64 ? 0xffffffff : item.payload.size()); put32(c, 24, item.zip64 ? 0xffffffff : quint32(size));
        put16(c, 28, item.name.size()); put16(c, 30, extra.size()); put32(c, 42, item.zip64 ? 0xffffffff : localOffset);
        central += c + item.name + extra;
    }
    const auto start = result.size(); result += central;
    if (zip64) {
        const auto end64 = result.size(); QByteArray record(56, 0); put32(record, 0, 0x06064b50); put64(record, 4, 44); put16(record, 12, 45); put16(record, 14, 45);
        put64(record, 24, items.size()); put64(record, 32, items.size()); put64(record, 40, central.size()); put64(record, 48, start); result += record;
        QByteArray locator(20, 0); put32(locator, 0, 0x07064b50); put64(locator, 8, end64); put32(locator, 16, 1); result += locator;
    }
    QByteArray end(22, 0); put32(end, 0, 0x06054b50); put16(end, 8, zip64 ? 0xffff : items.size()); put16(end, 10, zip64 ? 0xffff : items.size());
    put32(end, 12, zip64 ? 0xffffffff : central.size()); put32(end, 16, zip64 ? 0xffffffff : start); return result + end;
}
void checksum(QByteArray &header) {
    header.replace(148, 8, QByteArray(8, ' ')); quint64 total = 0; for (auto ch : header.left(512)) total += quint8(ch);
    header.replace(148, 8, QByteArray::number(total, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
}
QByteArray tarEntry(const QByteArray &name, const QByteArray &payload = "abc", char type = '0', const QByteArray &prefix = {}) {
    QByteArray header(512, 0); header.replace(0, qMin<qsizetype>(100, name.size()), name.left(100)); header.replace(345, qMin<qsizetype>(155, prefix.size()), prefix.left(155));
    header.replace(124, 12, QByteArray::number(payload.size(), 8).rightJustified(11, '0') + '\0'); header[156] = type; header.replace(257, 5, "ustar"); checksum(header);
    return header + payload + QByteArray((512 - payload.size() % 512) % 512, 0);
}
QByteArray gzip(const QByteArray &input) {
    z_stream stream{}; if (deflateInit2(&stream, 9, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData())); stream.avail_in = uInt(input.size());
    QByteArray result, chunk(32768, 0); int status;
    do { stream.next_out = reinterpret_cast<Bytef *>(chunk.data()); stream.avail_out = chunk.size(); status = deflate(&stream, Z_FINISH); result += chunk.left(chunk.size() - stream.avail_out); } while (status == Z_OK);
    deflateEnd(&stream); return status == Z_STREAM_END ? result : QByteArray();
}
FileDescriptor file(const QString &name = "w1/archive.zip") { return {name, {}, -1, QDateTime::fromString("2026-01-02T03:04:05.006Z", Qt::ISODateWithMs)}; }
QString text(const FileTransformResult &r) { return r.parts[0].toObject()["text"].toString(); }
}
class ArchiveTest : public QObject {
    Q_OBJECT
private slots:
    void ordinaryZipAndSourceTree();
    void zip64AndUnicodeNames();
    void zipMalformed_data();
    void zipMalformed();
    void encodedContentIsListingOnly();
    void tarSourceSemanticsAndInertNames();
    void tarMalformed_data();
    void tarMalformed();
    void gzipIntegrityConcatenationAndBounds();
    void separateListingAndTreeLimits();
    void cancellationAndCustomBounds();
    void productionDispatchAndWrappedMetadata();
    void truncatedStreamsAndDeterministicMutations();
};
void ArchiveTest::ordinaryZipAndSourceTree() {
    const auto data = zip({{"src/", ""}, {"src/a.txt", "hello"}, {"readme.md", "abc"}, {"src/a.txt", "last"}});
    const auto r = ArchiveTransformer::fromBytes(data, file()); QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(text(r), "src/\n  a.txt  (size:4.0B)\nreadme.md  (size:3.0B)");
    QCOMPARE(r.metadata["files"].toString(), "3"); QCOMPARE(r.metadata["directories"].toString(), "1"); QCOMPARE(r.metadata["totalEntries"].toString(), "4");
    QCOMPARE(r.metadata["uncompressedSize"].toString(), "12B"); QVERIFY(!r.effective);
    FileReadParams preview; preview.preview = true; const auto p = ArchiveTransformer::fromBytes(data, file(), preview);
    QCOMPARE(text(p), "src/\nreadme.md  (size:3.0B)"); QCOMPARE(p.delivered().depth, std::optional<double>(0));
    auto renamed = file("att/random"); renamed.originalFileName = "Bundle.WAR"; QCOMPARE(ArchiveTransformer::fromBytes(data, renamed).metadata["format"].toString(), "zip");
    QVERIFY(ArchiveTransformer::fromBytes(zip({}), file()).error.isEmpty());
    auto commented = data; const QByteArray comment = QByteArray("prefix") + QByteArray::fromHex("504b0506") + QByteArray(30, 'x');
    put16(commented, commented.size() - 2, comment.size()); commented += comment;
    QCOMPARE(ArchiveTransformer::fromBytes(commented, file()).parts, r.parts);
}
void ArchiveTest::zip64AndUnicodeNames() {
    Item large{"huge.bin", "encoded", {}, 5ULL * 1024 * 1024 * 1024, 0, 8, true};
    const auto r = ArchiveTransformer::fromBytes(zip({large}, true), file()); QVERIFY2(r.error.isEmpty(), qPrintable(r.error)); QCOMPARE(r.metadata["uncompressedSize"].toString(), "5.0GB");
    Item cp; cp.name = QByteArray(1, char(0x82)) + ".txt";
    Item utf; utf.name = QString::fromUtf8("日本語.txt").toUtf8(); utf.flags = 0x800;
    Item unicode{"legacy.txt"}; QByteArray fieldData(5, 0); fieldData[0] = 1; put32(fieldData, 1, crc32(0, reinterpret_cast<const Bytef *>(unicode.name.constData()), unicode.name.size()));
    unicode.extra = field(0x7075, fieldData + QString::fromUtf8("مرحبا.txt").toUtf8());
    const auto names = ArchiveTransformer::fromBytes(zip({cp, utf, unicode, {"folder\\file", "x"}}), file()); QVERIFY2(names.error.isEmpty(), qPrintable(names.error));
    QVERIFY(text(names).contains(QString::fromUtf8("é.txt"))); QVERIFY(text(names).contains(QString::fromUtf8("日本語.txt"))); QVERIFY(text(names).contains(QString::fromUtf8("مرحبا.txt"))); QVERIFY(text(names).contains("folder/\n  file"));
    unicode.extra[5] ^= 1; const auto ignored = ArchiveTransformer::fromBytes(zip({unicode}), file()); QVERIFY(text(ignored).contains("legacy.txt"));
}
void ArchiveTest::zipMalformed_data() {
    QTest::addColumn<QByteArray>("data");
    auto original = zip({{"a", "abc"}}); const auto central = original.indexOf(QByteArray::fromHex("504b0102")); const auto end = original.size() - 22;
    QTest::newRow("missing-end") << original.left(end);
    auto b = original; put32(b, end + 16, 0xfffffff0); QTest::newRow("directory-outside") << b;
    b = original; put32(b, end + 12, 0xfffffffe); QTest::newRow("directory-size-overflow") << b;
    b = original; put16(b, end + 4, 1); QTest::newRow("split") << b;
    b = original; put16(b, central + 28, 65535); QTest::newRow("record-truncated") << b;
    b = original; put32(b, central + 24, 0xffffffff); QTest::newRow("missing-zip64-extra") << b;
    b = original; put32(b, central + 42, 0xfffffffe); QTest::newRow("local-offset-outside") << b;
    b = original; put16(b, 26, 65535); QTest::newRow("local-header-name-outside") << b;
    b = original; put16(b, central + 10, 8); put32(b, central + 20, 100); QTest::newRow("member-extent-outside") << b;
    b = original; put32(b, central + 20, 2); QTest::newRow("stored-size-mismatch") << b;
    b = original; put16(b, central + 8, 64); QTest::newRow("strong-encryption") << b;
    b = original; put16(b, end + 20, 2); QTest::newRow("comment-truncated") << b;
    QTest::newRow("unsafe-parent") << zip({{"../outside", "x"}});
    QTest::newRow("unsafe-drive") << zip({{"C:/outside", "x"}});
    QTest::newRow("unsafe-absolute") << zip({{"/outside", "x"}});
    QTest::newRow("nul-name") << zip({{QByteArray("a\0b", 3), "x"}});
    Item invalid; invalid.name = QByteArray::fromHex("e282"); invalid.flags = 0x800; QTest::newRow("invalid-utf8") << zip({invalid});
    invalid = {"a", "x", QByteArray::fromHex("0100080001")}; QTest::newRow("invalid-extra-length") << zip({invalid});
    b = zip({{"a", "x"}}, true); put64(b, b.size() - 34, std::numeric_limits<quint64>::max()); QTest::newRow("zip64-offset-overflow") << b;
    b = zip({{"a", "x"}}, true); const auto z = b.indexOf(QByteArray::fromHex("504b0606")); put64(b, z + 40, std::numeric_limits<quint64>::max()); QTest::newRow("zip64-directory-overflow") << b;
}
void ArchiveTest::zipMalformed() { QFETCH(QByteArray, data); const auto r = ArchiveTransformer::fromBytes(data, file()); QVERIFY(!r.error.isEmpty()); QVERIFY(!r.coverageKnown); }
void ArchiveTest::encodedContentIsListingOnly() {
    Item unsupported{"coded.bin", "data", {}, 30, 0, 99}; const auto r = ArchiveTransformer::fromBytes(zip({unsupported}), file());
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error)); QCOMPARE(r.metadata["unsupportedContentMethods"].toString(), "99"); QVERIFY(text(r).contains("coded.bin"));
    Item encrypted{"private.bin", QByteArray(15, 'x'), {}, 3, 1, 0}; const auto e = ArchiveTransformer::fromBytes(zip({encrypted}), file());
    QVERIFY2(e.error.isEmpty(), qPrintable(e.error)); QCOMPARE(e.metadata["encryptedContent"].toString(), "listing-only");
    QVERIFY(!ArchiveTransformer::fromBytes("unknown", file("w1/file.7z")).supported);
}
void ArchiveTest::tarSourceSemanticsAndInertNames() {
    auto data = tarEntry("root/", {}, '5') + tarEntry("file.txt", "abc", '0', "root") + tarEntry("link", {}, '2')
        + tarEntry("ignored-long-name", "ignored", 'L') + tarEntry("ignored-pax", "path=ignored", 'x') + tarEntry("../outside", "x") + tarEntry("/absolute", "y")
        + tarEntry("line\nbreak", "z") + QByteArray(512, 0) + tarEntry("after-zero", "ignored");
    const auto r = ArchiveTransformer::fromBytes(data, file("w1/test.tar")); QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.metadata["totalEntries"].toString(), "6"); QCOMPARE(r.metadata["files"].toString(), "5");
    QVERIFY(text(r).contains("root/\n  file.txt")); QVERIFY(text(r).contains("../\n  outside")); QVERIFY(text(r).contains("line\\nbreak"));
    QVERIFY(!text(r).contains("ignored")); QVERIFY(!text(r).contains("after-zero"));
}
void ArchiveTest::tarMalformed_data() {
    QTest::addColumn<QByteArray>("data"); const auto original = tarEntry("a", "abc");
    QTest::newRow("short-header") << original.left(500);
    QTest::newRow("short-payload") << original.left(514);
    auto b = original; b[10] = 'X'; QTest::newRow("checksum") << b;
    b = original; b[124] = char(0x80); checksum(b); QTest::newRow("base256") << b;
    b = original; b.replace(124, 12, "77777777777\0", 12); checksum(b); QTest::newRow("impossible-size") << b;
    b = original; b[124] = '9'; checksum(b); QTest::newRow("non-octal") << b;
}
void ArchiveTest::tarMalformed() { QFETCH(QByteArray, data); const auto r = ArchiveTransformer::fromBytes(data, file("w1/file.tar")); QVERIFY(!r.error.isEmpty()); }
void ArchiveTest::gzipIntegrityConcatenationAndBounds() {
    const auto tar = tarEntry("a.txt", "hello") + QByteArray(1024, 0), compressed = gzip(tar);
    QVERIFY(!compressed.isEmpty()); const auto r = ArchiveTransformer::fromBytes(compressed, file("w1/file.tgz")); QVERIFY2(r.error.isEmpty(), qPrintable(r.error)); QCOMPARE(text(r), "a.txt  (size:5.0B)");
    const auto concatenated = gzip(tar.left(600)) + gzip(tar.mid(600)); QVERIFY(ArchiveTransformer::fromBytes(concatenated, file("w1/file.tar.gz")).error.isEmpty());
    auto corrupt = compressed; corrupt[corrupt.size() - 8] ^= 1; QVERIFY(!ArchiveTransformer::fromBytes(corrupt, file("w1/file.tgz")).error.isEmpty());
    QVERIFY(!ArchiveTransformer::fromBytes(compressed.left(compressed.size() - 3), file("w1/file.tgz")).error.isEmpty());
    ArchiveLimits limits; limits.maxGzipMembers = 1; QCOMPARE(ArchiveTransformer::fromBytes(concatenated, file("w1/file.tgz"), {}, limits).metadata["error"].toString(), "limit");
    limits = {}; limits.maxInflatedBytes = 4096; const auto bomb = gzip(QByteArray(1024 * 1024, 0)); QCOMPARE(ArchiveTransformer::fromBytes(bomb, file("w1/file.tgz"), {}, limits).metadata["error"].toString(), "limit");
    limits = {}; limits.maxInflateCalls = 1; QCOMPARE(ArchiveTransformer::fromBytes(bomb, file("w1/file.tgz"), {}, limits).metadata["error"].toString(), "limit");
    limits = {}; limits.maxEntries = 1; const auto partial = ArchiveTransformer::fromBytes(compressed, file("w1/file.tgz"), {}, limits); QVERIFY(partial.error.isEmpty()); QCOMPARE(partial.metadata["gzipIntegrity"].toString(), "not-verified-after-entry-limit");
}
void ArchiveTest::separateListingAndTreeLimits() {
    QList<Item> entries; for (int a = 0; a < 20; ++a) for (int b = 0; b < 10; ++b) for (int c = 0; c < 5; ++c) entries.append({QByteArray::number(a) + '/' + QByteArray::number(b) + '/' + QByteArray::number(c), ""});
    auto r = ArchiveTransformer::fromBytes(zip(entries), file()); QVERIFY2(r.error.isEmpty(), qPrintable(r.error)); QVERIFY(r.truncated);
    QCOMPARE(text(r).split('\n').size(), 501); QVERIFY(text(r).endsWith("… (tree truncated at 500 displayed entries)"));
    entries.clear(); for (int i = 0; i < 2001; ++i) entries.append({QByteArray::number(i), ""});
    r = ArchiveTransformer::fromBytes(zip(entries), file()); QVERIFY(r.error.isEmpty()); QCOMPARE(r.metadata["totalEntries"].toString(), "2000"); QCOMPARE(r.metadata["truncated"].toString(), "2000+ entries");
    QVERIFY(text(r).contains("… +1980 more"));
}
void ArchiveTest::cancellationAndCustomBounds() {
    const auto data = zip({{"a/b/c", "x"}}); std::atomic_bool cancelled{true};
    QCOMPARE(ArchiveTransformer::fromBytes(data, file(), {}, {}, &cancelled).metadata["error"].toString(), "cancelled");
    ArchiveLimits limits; limits.maxInputBytes = 1; QVERIFY(!ArchiveTransformer::fromBytes(data, file(), {}, limits).error.isEmpty());
    limits = {}; limits.maxNameBytes = 2; QVERIFY(!ArchiveTransformer::fromBytes(data, file(), {}, limits).error.isEmpty());
    limits = {}; limits.maxPathDepth = 2; FileReadParams p; p.depth = 0; QVERIFY(!ArchiveTransformer::fromBytes(data, file(), p, limits).error.isEmpty());
    limits = {}; limits.maxTreeNodes = 1; QVERIFY(!ArchiveTransformer::fromBytes(data, file(), {}, limits).error.isEmpty());
    limits = {}; limits.maxOutputChars = 5; const auto cut = ArchiveTransformer::fromBytes(data, file(), {}, limits); QVERIFY(cut.truncated); QVERIFY(!cut.coverageKnown); QVERIFY(text(cut).size() <= 5);
    limits = {}; limits.maxHeaders = 1; const auto special = tarEntry("ignored", {}, 'x') + tarEntry("a"); QCOMPARE(ArchiveTransformer::fromBytes(special, file("w1/f.tar"), {}, limits).metadata["error"].toString(), "limit");
    QVERIFY(!ArchiveTransformer::fromBytes({}, file()).error.isEmpty());
    limits = {}; limits.maxOutputChars = 1;
    const auto failed = ArchiveTransformer::fromBytes("broken", file(), {}, limits); QVERIFY(!failed.error.isEmpty()); QCOMPARE(text(failed).size(), 1);
}
void ArchiveTest::productionDispatchAndWrappedMetadata() {
    const auto data = zip({{"src/a.txt", "hello"}}); auto descriptor = file("att/blob"); descriptor.originalFileName = "source.JAR";
    FileReadParams preview; preview.preview = true;
    const auto r = FileTransformer::fromBytes(data, descriptor, preview); QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
    QCOMPARE(r.metadata["format"].toString(), "zip"); QCOMPARE(r.metadata["totalEntries"].toString(), "1"); QVERIFY(r.supported);
    QCOMPARE(r.delivered().depth, std::optional<double>(0));
    const auto body = FileTransformer::wrappedParts(descriptor, r)[0].toObject()["text"].toString();
    QVERIFY(body.contains("<file path=\"att/blob\"")); QVERIFY(body.contains("<preview>\nsrc/\n</preview>")); QVERIFY(body.contains("format:zip"));
    const auto tar = tarEntry("readme.txt", "text") + QByteArray(512, 0);
    const auto compressed = FileTransformer::fromBytes(gzip(tar), file("ws1/bundle.tar.gz"));
    QVERIFY(compressed.error.isEmpty()); QVERIFY(text(compressed).contains("readme.txt"));
}
void ArchiveTest::truncatedStreamsAndDeterministicMutations() {
    const auto ordinary = zip({{"small.txt", "content"}});
    const auto compressed = gzip(tarEntry("small.txt", "content") + QByteArray(1024, 0));
    for (int i = 0; i < ordinary.size(); ++i) QVERIFY(!ArchiveTransformer::fromBytes(ordinary.left(i), file()).error.isEmpty());
    for (int i = 0; i < compressed.size(); ++i) QVERIFY(!ArchiveTransformer::fromBytes(compressed.left(i), file("w1/test.tgz")).error.isEmpty());
    // A reproducible parser smoke corpus, not a claim of exhaustive fuzzing.
    quint32 seed = 0x9e3779b9; auto next = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    for (int i = 0; i < 600; ++i) {
        auto mutated = i % 2 ? ordinary : compressed;
        for (int n = 0; n < 1 + i % 4; ++n) mutated[next() % mutated.size()] = char(next());
        const auto result = ArchiveTransformer::fromBytes(mutated, file(i % 2 ? "w1/a.zip" : "w1/a.tgz"));
        QCOMPARE(result.parts.size(), 1); QVERIFY(text(result).size() <= 65536); if (!result.error.isEmpty()) QVERIFY(!result.coverageKnown);
    }
}
QTEST_GUILESS_MAIN(ArchiveTest)
#include "test_archive_transformer.moc"
