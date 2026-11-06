#include "links/TranscriptLinkProvenanceCodec.h"
#include <QTest>
using namespace ade;
class LinkProvenanceCodecTest : public QObject {
  Q_OBJECT
  TranscriptLinkProvenance sample() {
    TranscriptLinkProvenance row;
    row.mountBindings = {{"ws1", QString(64, 'a')}};
    row.browserSessionId = "window-1";
    row.historicalTabUrls = {
        {"t_1", QUrl("https://example.test/docs?q=a%20b")}};
    row.attachmentNames = {{"att/hash.png", "Original image.png"}};
    return row;
  }
private slots:
  void roundtripIsCanonicalAndPreservesAuthority() {
    QHash<QString, TranscriptLinkProvenance> rows{{"message-1", sample()}};
    const auto encoded = encodeLinkProvenance(rows);
    const auto decoded = decodeLinkProvenance(encoded);
    QCOMPARE(decoded.size(), 1);
    QCOMPARE(decoded["message-1"].mountBindings,
             rows["message-1"].mountBindings);
    QCOMPARE(decoded["message-1"].historicalTabUrls,
             rows["message-1"].historicalTabUrls);
    QCOMPARE(decoded["message-1"].browserSessionId, "window-1");
    QCOMPARE(encodeLinkProvenance(decoded), encoded);
  }
  void invalidRowsNeverGainAuthority() {
    auto encoded = encodeLinkProvenance({{"valid", sample()}});
    auto row = encoded["valid"].toObject();
    row["version"] = 2;
    encoded["version"] = row;
    row = encoded["valid"].toObject();
    row["mountBindings"] = QJsonObject{{"ws1", "not-a-root-identity"}};
    encoded["mount"] = row;
    row = encoded["valid"].toObject();
    row["historicalTabUrls"] = QJsonObject{{"t_1", "file:///C:/private.txt"}};
    encoded["scheme"] = row;
    row = encoded["valid"].toObject();
    row["historicalTabUrls"] =
        QJsonObject{{"t_1", "https://user:password@example.test/"}};
    encoded["credentials"] = row;
    row = encoded["valid"].toObject();
    row["mountBindings"] = QJsonObject{{"../escape", QString(64, 'b')}};
    encoded["prefix"] = row;
    const auto result = decodeLinkProvenance(encoded);
    QCOMPARE(result.size(), 1);
    QVERIFY(result.contains("valid"));
  }
  void boundsRejectWholeOversizedState() {
    QJsonObject encoded;
    const auto row = encodeLinkProvenance({{"row", sample()}}).value("row");
    for (int i = 0; i < 8193; ++i)
      encoded[QString::number(i)] = row;
    QVERIFY(decodeLinkProvenance(encoded).isEmpty());
    encoded = {};
    auto big = row.toObject();
    big["attachmentNames"] =
        QJsonObject{{"att/a", QString(2 * 1024 * 1024, 'x')}};
    encoded["row"] = big;
    QVERIFY(decodeLinkProvenance(encoded).isEmpty());
  }
};
QTEST_APPLESS_MAIN(LinkProvenanceCodecTest)
#include "test_link_provenance_codec.moc"
