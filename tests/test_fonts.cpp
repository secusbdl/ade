#include "ui/AppFonts.h"

#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextLayout>

using namespace ade;

class FontsTest : public QObject {
    Q_OBJECT
private slots:
    void registersExactFamiliesAndStyles();
    void containsPinnedLicensedFonts();
    void preservesReferenceAdvanceMetrics();
    void usesSystemFallbackForArabic();
};

void FontsTest::registersExactFamiliesAndStyles() {
    const auto families=AppFonts::load();
    QVERIFY2(families.fullyLoaded(),qPrintable(families.errors.join('\n')));
    QCOMPARE(families.body,QString("Geist"));
    QCOMPARE(families.mono,QString("Roboto Mono"));
    for(const auto &family:{families.body,families.mono}) {
        for(const auto weight:{QFont::Normal,QFont::Medium,QFont::DemiBold,QFont::Bold}) {
            QFont font(family);font.setPixelSize(14);font.setWeight(weight);
            QCOMPARE(QFontInfo(font).family(),family);
            QCOMPARE(QFontInfo(font).weight(),weight);
            font.setItalic(true);
            QCOMPARE(QFontInfo(font).family(),family);
            QVERIFY(QFontInfo(font).italic());
        }
    }
    const auto again=AppFonts::load();
    QCOMPARE(again.body,families.body);QCOMPARE(again.mono,families.mono);
    QVERIFY(again.errors.isEmpty());
}

void FontsTest::containsPinnedLicensedFonts() {
    AppFonts::load();
    const QList<QPair<QString,QByteArray>> assets={
        {"Geist-Variable.ttf","995c579489b33cd28a6732d7aa7ca1b46c2faa832c18b4063933d7652c39932b"},
        {"Geist-Italic-Variable.ttf","73eacb165c003de2159368f09dda4b2953683c3b455d8ff1afafe74b6d1b54df"},
        {"RobotoMono-Variable.ttf","ffe6db2a820fad13df78365b4d471a5adb5d7530f409d462aa105a109586eb94"},
        {"RobotoMono-Italic-Variable.ttf","5f960ce95c4c0ef44fe76095901eb44b5e02e82cc38f0093741191f0732e453b"}
    };
    for(const auto &asset:assets){
        QFile file(":/fonts/"+asset.first);QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex(),asset.second);
    }
    for(const auto &name:{"Geist-OFL.txt","RobotoMono-OFL.txt"}){
        QFile license(":/fonts/"+QString(name));QVERIFY(license.open(QIODevice::ReadOnly));
        const auto bytes=license.readAll();
        QVERIFY(bytes.contains("SIL OPEN FONT LICENSE Version 1.1"));
        QVERIFY(bytes.contains("Copyright"));
    }
}

void FontsTest::preservesReferenceAdvanceMetrics() {
    const auto families=AppFonts::load();QVERIFY(families.fullyLoaded());
    QFont body(families.body);body.setPixelSize(1000);body.setWeight(QFont::Normal);
    body.setHintingPreference(QFont::PreferNoHinting);
    const auto raw=QRawFont::fromFont(body);QVERIFY(raw.isValid());
    const auto glyphs=raw.glyphIndexesForString("AWi0 ");
    const auto advances=raw.advancesForGlyphIndexes(glyphs,QRawFont::UseDesignMetrics);
    const QList<qreal> expected={668,945,244,663,250};
    QCOMPARE(advances.size(),expected.size());
    for(qsizetype i=0;i<expected.size();++i)
        QVERIFY2(qAbs(advances[i].x()-expected[i])<0.1,qPrintable(QString("Geist advance %1: %2").arg(i).arg(advances[i].x())));

    QFont mono(families.mono);mono.setPixelSize(1000);mono.setWeight(QFont::Normal);
    mono.setHintingPreference(QFont::PreferNoHinting);
    const auto monoRaw=QRawFont::fromFont(mono);QVERIFY(monoRaw.isValid());
    const auto monoAdvances=monoRaw.advancesForGlyphIndexes(monoRaw.glyphIndexesForString("AWi0 {}"),QRawFont::UseDesignMetrics);
    for(const auto &advance:monoAdvances)QVERIFY(qAbs(advance.x()-1229.0/2048.0*1000.0)<0.1);
    QFont small(families.body);small.setPixelSize(14);
    const QFontMetricsF metrics(small);
    QVERIFY(metrics.horizontalAdvance("Models & Providers")>70);
    QVERIFY(metrics.horizontalAdvance("Models & Providers")<180);
    QVERIFY(metrics.height()>10 && metrics.height()<30);
}

void FontsTest::usesSystemFallbackForArabic() {
    const auto families=AppFonts::load();QVERIFY(families.fullyLoaded());
    if(QFontDatabase::families(QFontDatabase::Arabic).isEmpty())QSKIP("This Windows image has no Arabic fallback font installed.");
    QFont font(families.body);font.setPixelSize(16);
    const auto raw=QRawFont::fromFont(font);
    QVERIFY(!raw.supportsCharacter(0x0645)); // Geist does not claim Arabic coverage.
    QVERIFY(!(font.styleStrategy() & QFont::NoFontMerging));
    QTextLayout layout(QString::fromUtf8("Settings مرحبا"),font);
    layout.beginLayout();auto line=layout.createLine();QVERIFY(line.isValid());line.setLineWidth(500);layout.endLayout();
    bool usedFallback=false;
    for(const auto &run:layout.glyphRuns()){
        if(run.rawFont().familyName()!=families.body)usedFallback=true;
        for(const auto glyph:run.glyphIndexes())QVERIFY(glyph!=0);
    }
    QVERIFY(usedFallback);
    // This verifies one installed fallback script, not universal Unicode coverage.
}

QTEST_MAIN(FontsTest)
#include "test_fonts.moc"
