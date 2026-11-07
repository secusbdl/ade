// NotificationSoundService: upstream trigger rules, synthesized ADE-owned
// packs, PCM scaling and the import format with ADE's limits. Before this
// change the stored loudness/sound preferences had no consumer at all: the
// service, its helpers and the importer did not exist, so every test here
// fails to build or observe playback on the old code. No test plays audio:
// a recording ISoundOutput replaces winmm (winOutputSmoke is opt-in).
#include "ui/NotificationSounds.h"
#include "ui/SoundOutput.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <QtTest>
#include <memory>

using namespace ade;

namespace {
class RecordingOutput final : public ISoundOutput {
public:
    bool result = true;
    QList<QByteArray> wavs;
    QList<QPair<QString, double>> files;
    int stops = 0;
    bool playWav(const QByteArray& wav) override { wavs.append(wav); return result; }
    bool playFile(const QString& path, double gain) override { files.append({path, gain}); return result; }
    void stop() override { ++stops; }
    int plays() const { return int(wavs.size() + files.size()); }
};

QByteArray pcm16(const QList<qint16>& samples, int channels = 1, int rate = 44100, int format = 1, int bits = 16) {
    QByteArray data(samples.size() * 2, '\0');
    for (int i = 0; i < samples.size(); ++i) qToLittleEndian<qint16>(samples[i], reinterpret_cast<uchar*>(data.data()) + i * 2);
    QByteArray header(44, '\0');
    auto* out = reinterpret_cast<uchar*>(header.data());
    memcpy(out, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(36 + data.size()), out + 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, out + 16);
    qToLittleEndian<quint16>(quint16(format), out + 20);
    qToLittleEndian<quint16>(quint16(channels), out + 22);
    qToLittleEndian<quint32>(quint32(rate), out + 24);
    const int align = channels * bits / 8;
    qToLittleEndian<quint32>(quint32(rate * align), out + 28);
    qToLittleEndian<quint16>(quint16(align), out + 32);
    qToLittleEndian<quint16>(quint16(bits), out + 34);
    memcpy(out + 36, "data", 4);
    qToLittleEndian<quint32>(quint32(data.size()), out + 40);
    return header + data;
}
qint16 sampleAt(const QByteArray& wav, int index) { return qFromLittleEndian<qint16>(reinterpret_cast<const uchar*>(wav.constData()) + 44 + index * 2); }
const QByteArray mp3Bytes = QByteArray("\xFF\xFB\x90\x64", 4) + QByteArray(400, '\x11');

bool write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }
QJsonObject json(const QString& path) { return QJsonDocument::fromJson(read(path)).object(); }
QStringList entries(const QString& dir) { return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot); }

struct Fixture {
    QTemporaryDir root;
    RecordingOutput output;
    qint64 now = 0;
    std::unique_ptr<NotificationSoundService> service;
    QString packs() const { return root.filePath("data/imported-sound-packs"); }
    Fixture() { service = std::make_unique<NotificationSoundService>(packs(), &output, [this] { return now; }); }
};
}

class NotificationSoundsTest : public QObject {
    Q_OBJECT
private slots:
    void gainMapping() {
        QCOMPARE(NotificationSoundService::gainFor("off"), 0.0);
        QCOMPARE(NotificationSoundService::gainFor("normal"), 0.251);
        QCOMPARE(NotificationSoundService::gainFor("loud"), 1.0);
        QCOMPARE(NotificationSoundService::gainFor("max"), 0.251);
    }
    void synthesizedWavIsValidAndDeterministic() {
        for (const auto& pack : NotificationSoundService::builtinPacks()) {
            QVERIFY(pack.builtin);
            QList<QByteArray> sounds;
            for (const auto event : {AttentionEvent::Done, AttentionEvent::Question, AttentionEvent::Error}) {
                const auto wav = NotificationSoundService::synthesizeWav(pack.id, event);
                QVERIFY(NotificationSoundService::looksLikePcm16Wav(wav));
                QCOMPARE(qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(wav.constData()) + 22), quint16(1));
                QCOMPARE(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(wav.constData()) + 24), quint32(44100));
                const int frames = int((wav.size() - 44) / 2);
                QVERIFY2(frames <= 44100 * 6 / 10, qPrintable(pack.id));
                int peak = 0;
                for (int i = 0; i < frames; ++i) peak = qMax(peak, qAbs(int(sampleAt(wav, i))));
                QVERIFY(peak > 3000);
                QVERIFY(peak < 32767);
                QCOMPARE(NotificationSoundService::synthesizeWav(pack.id, event), wav);
                QVERIFY(!sounds.contains(wav));
                sounds.append(wav);
            }
        }
        QVERIFY(NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Done)
                != NotificationSoundService::synthesizeWav("ade-soft", AttentionEvent::Done));
    }
    void scalePcm16Wav() {
        const auto wav = pcm16({10000, -10000, 0, 30000, -30000});
        const auto subtle = NotificationSoundService::scalePcm16Wav(wav, 0.251);
        QCOMPARE(subtle.size(), wav.size());
        QCOMPARE(sampleAt(subtle, 0), qint16(2510));
        QCOMPARE(sampleAt(subtle, 1), qint16(-2510));
        QCOMPARE(sampleAt(subtle, 2), qint16(0));
        const auto loud = NotificationSoundService::scalePcm16Wav(wav, 1.5);
        QCOMPARE(sampleAt(loud, 0), qint16(15000));
        QCOMPARE(sampleAt(loud, 3), qint16(32767));
        QCOMPARE(sampleAt(loud, 4), qint16(-32768));
        QCOMPARE(NotificationSoundService::scalePcm16Wav(wav, 1.0), wav);
        QVERIFY(NotificationSoundService::scalePcm16Wav(wav, 4.5).isEmpty());
        QVERIFY(NotificationSoundService::scalePcm16Wav(pcm16({1, 2}, 1, 44100, 3, 16), 1.0).isEmpty()); // float
        QVERIFY(NotificationSoundService::scalePcm16Wav(pcm16({1, 2}, 1, 44100, 1, 8), 1.0).isEmpty());
        QVERIFY(NotificationSoundService::scalePcm16Wav(pcm16({1, 2}, 6), 1.0).isEmpty());
        QVERIFY(NotificationSoundService::scalePcm16Wav(wav.left(wav.size() - 3), 1.0).isEmpty()); // truncated
        QVERIFY(NotificationSoundService::scalePcm16Wav("RIFF", 1.0).isEmpty());
        QVERIFY(NotificationSoundService::looksLikePcm16Wav(pcm16({1, 2, 3, 4}, 2)));
    }
    void debounceDoneOnly() {
        Fixture f;
        f.now = 0;
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false)); // never played: no debounce at t=0
        f.now = 9999;
        QVERIFY(!f.service->notify(AttentionEvent::Done, "a", false, false));
        QVERIFY(f.service->notify(AttentionEvent::Question, "a", false, false));
        QVERIFY(f.service->notify(AttentionEvent::Error, "a", false, false));
        f.now = 9999 + 10000;
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        QCOMPARE(f.output.plays(), 4);
    }
    void focusResetsDebounce() {
        Fixture f;
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        f.now = 5000;
        QVERIFY(!f.service->notify(AttentionEvent::Done, "a", false, false));
        f.now = 6000;
        QVERIFY(f.service->notify(AttentionEvent::Done, "b", true, false));
        QCOMPARE(f.output.plays(), 2);
    }
    void visibleAndFocusedSuppressed() {
        Fixture f;
        QVERIFY(!f.service->notify(AttentionEvent::Done, "a", true, true));
        QVERIFY(!f.service->notify(AttentionEvent::Question, "a", true, true));
        QVERIFY(!f.service->notify(AttentionEvent::Error, "a", true, true));
        QCOMPARE(f.output.plays(), 0);
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, true)); // selected but window inactive
        QVERIFY(f.service->notify(AttentionEvent::Error, "b", true, false)); // another chat
        QCOMPARE(f.output.plays(), 2);
    }
    void offPlaysNothingAndDoesNotArmDebounce() {
        Fixture f;
        f.service->configure({{"notificationLoudness", "off"}, {"notificationSound", "default"}});
        QVERIFY(!f.service->notify(AttentionEvent::Done, "a", false, false));
        QVERIFY(!f.service->notify(AttentionEvent::Error, "a", false, false));
        QCOMPARE(f.output.plays(), 0);
        f.service->configure({{"notificationLoudness", "loud"}, {"notificationSound", "default"}});
        f.now = 1;
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        QCOMPARE(f.output.wavs.last(), NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Done));
    }
    void failedOutputDoesNotArmDebounce() {
        Fixture f;
        f.output.result = false;
        QVERIFY(!f.service->notify(AttentionEvent::Done, "a", false, false));
        f.output.result = true;
        f.now = 1;
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        QCOMPARE(f.output.plays(), 2);
    }
    void missingEventDeliversNothing() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("done.mp3"), mp3Bytes));
        QVERIFY(write(source.filePath("pack.json"), R"({"name":"Done only","sounds":{"done":"done.mp3","question":null}})"));
        const auto imported = f.service->importFile(source.filePath("pack.json"));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        f.service->configure({{"notificationLoudness", "normal"}, {"notificationSound", imported.id}});
        QCOMPARE(f.service->activePack(), imported.id);
        QVERIFY(!f.service->notify(AttentionEvent::Question, "a", false, false));
        QVERIFY(!f.service->notify(AttentionEvent::Error, "a", false, false));
        QCOMPARE(f.output.plays(), 0);
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        QCOMPARE(f.output.files.size(), 1);
        QVERIFY(f.output.files.first().first.endsWith(imported.id + "/done.mp3"));
        QCOMPARE(f.output.files.first().second, 0.251);
    }
    void packFallback() {
        Fixture f;
        for (const auto& id : {"default", "", "missing", "..", "../x", "ade-soft/../ade-pulse"})
            QCOMPARE(f.service->resolvePack(id), QString("ade-chime"));
        QCOMPARE(f.service->resolvePack("ade-pulse"), QString("ade-pulse"));
        // A UUID-shaped id without a listed pack still falls back.
        QCOMPARE(f.service->resolvePack(QUuid::createUuid().toString(QUuid::WithoutBraces)), QString("ade-chime"));
        f.service->configure({{"notificationLoudness", "max"}, {"notificationSound", ".."}});
        QCOMPARE(f.service->activePack(), QString("ade-chime"));
        QCOMPARE(f.service->loudness(), QString("normal"));
    }
    void configureResolvesPackOnlyWhenRequestChanges() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("bell.mp3"), mp3Bytes));
        const auto imported = f.service->importFile(source.filePath("bell.mp3"));
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        const QJsonObject prefs{{"notificationLoudness", "normal"}, {"notificationSound", imported.id}};
        f.service->configure(prefs);
        QCOMPARE(f.service->activePack(), imported.id);
        // Saving unrelated preferences does not enumerate the packs again: a
        // pack removed behind the service's back stays selected until the
        // requested id changes.
        QVERIFY(QDir(f.packs() + "/" + imported.id).removeRecursively());
        f.service->configure({{"notificationLoudness", "loud"}, {"notificationSound", imported.id}});
        QCOMPARE(f.service->activePack(), imported.id);
        QCOMPARE(f.service->loudness(), QString("loud"));
        f.service->configure({{"notificationLoudness", "loud"}, {"notificationSound", "ade-soft"}});
        QCOMPARE(f.service->activePack(), QString("ade-soft"));
        f.service->configure(prefs);
        QCOMPARE(f.service->activePack(), QString("ade-chime"));
        // The pack reappears: the same request stays resolved as before...
        QVERIFY(write(f.packs() + "/" + imported.id + "/done.mp3", mp3Bytes));
        f.service->configure(prefs);
        QCOMPARE(f.service->activePack(), QString("ade-chime"));
        // ...until an import changes the listing and invalidates it.
        const auto next = f.service->importFile(source.filePath("bell.mp3"));
        QVERIFY2(next.error.isEmpty(), qPrintable(next.error));
        f.service->configure(prefs);
        QCOMPARE(f.service->activePack(), imported.id);
    }
    void importSingleMp3() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("Bell.v2.mp3"), mp3Bytes));
        const auto result = f.service->importFile(source.filePath("Bell.v2.mp3"));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.name, QString("Bell.v2"));
        QVERIFY(!QUuid::fromString(result.id).isNull());
        const auto dir = f.packs() + "/" + result.id;
        for (const auto& event : {"done", "question", "error"}) QCOMPARE(read(dir + "/" + event + ".mp3"), mp3Bytes);
        const auto meta = json(dir + "/pack.json");
        QCOMPARE(meta["name"].toString(), QString("Bell.v2"));
        QCOMPARE(meta["sounds"].toObject()["question"].toString(), QString("question.mp3"));
        QCOMPARE(f.service->resolvePack(result.id), result.id);
    }
    void importManifest() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("sounds/finish.mp3"), mp3Bytes));
        QVERIFY(write(source.filePath("ask.mp3"), QByteArray("ID3") + QByteArray(64, '\0')));
        QVERIFY(write(source.filePath("pack.json"),
                      R"({"name":"Studio","description":"Mine","sounds":{"done":"sounds/finish.mp3","question":"ask.mp3","error":null},"extra":1})"));
        const auto result = f.service->importFile(source.filePath("pack.json"));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.name, QString("Studio"));
        const auto dir = f.packs() + "/" + result.id;
        const auto meta = json(dir + "/pack.json");
        QCOMPARE(meta["description"].toString(), QString("Mine"));
        QCOMPARE(meta["sounds"].toObject()["done"].toString(), QString("done.mp3"));
        QCOMPARE(meta["sounds"].toObject()["question"].toString(), QString("question.mp3"));
        QVERIFY(meta["sounds"].toObject()["error"].isNull());
        QVERIFY(!QFile::exists(dir + "/error.mp3"));
    }
    void importWav() {
        Fixture f;
        QTemporaryDir source;
        const auto wav = pcm16({10000, -10000, 20000, -20000}, 2);
        QVERIFY(write(source.filePath("tone.wav"), wav));
        const auto result = f.service->importFile(source.filePath("tone.wav"));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(read(f.packs() + "/" + result.id + "/done.wav"), wav);
        QVERIFY(!QFile::exists(f.packs() + "/" + result.id + "/done.mp3"));
        f.service->configure({{"notificationLoudness", "normal"}, {"notificationSound", result.id}});
        QVERIFY(f.service->notify(AttentionEvent::Error, "a", false, false));
        QCOMPARE(f.output.wavs.size(), 1);
        QCOMPARE(sampleAt(f.output.wavs.first(), 0), qint16(2510));
    }
    void importRejectsNonPcm16Wav() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("float.wav"), pcm16({1, 2}, 1, 44100, 3)));
        QVERIFY(write(source.filePath("eight.wav"), pcm16({1, 2}, 1, 44100, 1, 8)));
        QVERIFY(write(source.filePath("fake.wav"), mp3Bytes));
        for (const auto& name : {"float.wav", "eight.wav", "fake.wav"}) {
            const auto result = f.service->importFile(source.filePath(name));
            QVERIFY(result.id.isEmpty());
            QVERIFY2(result.error.contains("16-bit PCM"), qPrintable(result.error));
        }
        QVERIFY(entries(f.packs()).isEmpty());
    }
    void importRejectsTraversal() {
        Fixture f;
        QTemporaryDir outer;
        QVERIFY(write(outer.filePath("outside.mp3"), mp3Bytes));
        QVERIFY(write(outer.filePath("pack/pack.json"), R"({"name":"Escape","sounds":{"done":"../outside.mp3","error":"..\\outside.mp3"}})"));
        const auto result = f.service->importFile(outer.filePath("pack/pack.json"));
        QVERIFY(result.id.isEmpty());
        QVERIFY2(result.error.startsWith("No valid sound files found."), qPrintable(result.error));
        QVERIFY(result.error.contains("must be relative to the manifest file"));
        QVERIFY(entries(f.packs()).isEmpty());
    }
    void importRejectsAbsoluteAndAds() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("a.mp3"), mp3Bytes));
        const auto absolute = QDir::toNativeSeparators(source.filePath("a.mp3"));
        QJsonObject manifest{{"name", "Streams"}, {"sounds", QJsonObject{{"done", absolute}, {"question", "a.wav:x.mp3"}, {"error", "//server/share/a.mp3"}}}};
        QVERIFY(write(source.filePath("pack.json"), QJsonDocument(manifest).toJson()));
        const auto result = f.service->importFile(source.filePath("pack.json"));
        QVERIFY(result.id.isEmpty());
        QCOMPARE(result.error.count("must be relative to the manifest file"), 3);
        QVERIFY(entries(f.packs()).isEmpty());
    }
    void importRejectsBadHeader() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("bad.mp3"), QByteArray("\xFF\xC0\x00\x00", 4)));
        QVERIFY(write(source.filePath("good.mp3"), QByteArray("\xFF\xFB\x00\x00", 4)));
        const auto bad = f.service->importFile(source.filePath("bad.mp3"));
        QCOMPARE(bad.error, QString("\"bad.mp3\" does not appear to be a valid MP3 file."));
        QVERIFY(f.service->importFile(source.filePath("good.mp3")).error.isEmpty());
        QVERIFY(!NotificationSoundService::looksLikeMp3(QByteArray("\xFF\xC0", 2)));
        QVERIFY(NotificationSoundService::looksLikeMp3(QByteArray("\xFF\xE0", 2)));
        QVERIFY(NotificationSoundService::looksLikeMp3("ID3"));
        QVERIFY(!NotificationSoundService::looksLikeMp3("ID"));
    }
    void importRejectsEmptySounds() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("empty.json"), R"({"name":"Empty","sounds":{"done":null}})"));
        QCOMPARE(f.service->importFile(source.filePath("empty.json")).error,
                 QString("The sound pack must specify at least one sound file (done, question, or error)."));
        QVERIFY(write(source.filePath("noname.json"), R"({"name":"","sounds":{"done":"a.mp3"}})"));
        QVERIFY(f.service->importFile(source.filePath("noname.json")).error.startsWith("Invalid sound pack JSON: name:"));
        QVERIFY(write(source.filePath("nosounds.json"), R"({"name":"X"})"));
        QVERIFY(f.service->importFile(source.filePath("nosounds.json")).error.contains("sounds: Expected an object."));
        QVERIFY(write(source.filePath("missing.json"), R"({"name":"X","sounds":{"done":"missing.mp3"}})"));
        QVERIFY(f.service->importFile(source.filePath("missing.json")).error.contains("Sound file \"missing.mp3\" not found."));
        QCOMPARE(f.service->importFile(source.filePath("notes.txt")).error, QString("Select an .mp3 or .wav file or a sound pack .json manifest."));
        QVERIFY(entries(f.packs()).isEmpty());
    }
    void importRejectsOversize() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("big.mp3"), mp3Bytes + QByteArray(NotificationSoundService::maxAudioBytes, '\0')));
        QVERIFY(f.service->importFile(source.filePath("big.mp3")).error.contains("larger than 5 MiB"));
        QVERIFY(write(source.filePath("big.json"), "{\"name\":\"" + QByteArray(NotificationSoundService::maxManifestBytes, 'a') + "\",\"sounds\":{}}"));
        QCOMPARE(f.service->importFile(source.filePath("big.json")).error, QString("The sound pack manifest is larger than 64 KiB."));
        // The pack cap names the folder holding imported packs.
        for (int i = 0; i < NotificationSoundService::maxImportedPacks; ++i)
            QVERIFY(QDir().mkpath(f.packs() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces)));
        QVERIFY(write(source.filePath("ok.mp3"), mp3Bytes));
        const auto capped = f.service->importFile(source.filePath("ok.mp3"));
        QVERIFY(capped.id.isEmpty());
        QVERIFY2(capped.error.contains(QDir::toNativeSeparators(f.packs())), qPrintable(capped.error));
    }
    void importRejectsSymlink() {
        Fixture f;
        QTemporaryDir source, outside;
        QVERIFY(write(outside.filePath("secret.mp3"), mp3Bytes));
        const auto junction = source.filePath("linked");
        QProcess process;
        process.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(junction), QDir::toNativeSeparators(outside.path())});
        QVERIFY(process.waitForFinished(5000));
        if (process.exitCode() != 0) QSKIP("Creating a test junction is not available on this host.");
        QVERIFY(write(source.filePath("pack.json"), R"({"name":"Linked","sounds":{"done":"linked/secret.mp3"}})"));
        const auto viaJunction = f.service->importFile(source.filePath("pack.json"));
        QVERIFY(QDir().rmdir(junction));
        QVERIFY(viaJunction.id.isEmpty());
        QVERIFY2(viaJunction.error.contains("not a link or shortcut"), qPrintable(viaJunction.error));
        // A file symbolic link needs a privilege; check it only when available.
        QProcess fileLink;
        fileLink.start("cmd.exe", {"/c", "mklink", QDir::toNativeSeparators(source.filePath("alias.mp3")), QDir::toNativeSeparators(outside.filePath("secret.mp3"))});
        QVERIFY(fileLink.waitForFinished(5000));
        if (fileLink.exitCode() == 0) {
            const auto direct = f.service->importFile(source.filePath("alias.mp3"));
            QVERIFY(QFile::remove(source.filePath("alias.mp3")));
            QVERIFY(direct.id.isEmpty());
            QVERIFY2(direct.error.contains("not a link or shortcut"), qPrintable(direct.error));
        }
        QVERIFY(entries(f.packs()).isEmpty());
    }
    void importRejectsInvalidJson() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("broken.json"), "{\"name\":"));
        QCOMPARE(f.service->importFile(source.filePath("broken.json")).error, QString("The file is not valid JSON."));
        QVERIFY(write(source.filePath("array.json"), "[]"));
        QCOMPARE(f.service->importFile(source.filePath("array.json")).error, QString("Invalid sound pack JSON: Expected an object."));
    }
    void importAtomicOnFailure() {
        Fixture f;
        QTemporaryDir source;
        QVERIFY(write(source.filePath("good.mp3"), mp3Bytes));
        QVERIFY(QDir().mkpath(f.packs()));
        // The packs folder cannot be created: the import fails cleanly.
        QVERIFY(QDir(f.packs()).removeRecursively());
        QVERIFY(write(f.packs(), "not a directory"));
        const auto result = f.service->importFile(source.filePath("good.mp3"));
        QVERIFY(result.id.isEmpty());
        QVERIFY(!result.error.isEmpty());
        QVERIFY(QFile::remove(f.packs()));
        // A rejected manifest leaves neither a pack nor a staging folder.
        QVERIFY(write(source.filePath("bad.mp3"), "nope"));
        QVERIFY(write(source.filePath("pack.json"), R"({"name":"Bad","sounds":{"done":"bad.mp3"}})"));
        QVERIFY(!f.service->importFile(source.filePath("pack.json")).error.isEmpty());
        QVERIFY(entries(f.packs()).isEmpty());
        QVERIFY(f.service->importFile(source.filePath("good.mp3")).error.isEmpty());
        QCOMPARE(entries(f.packs()).size(), 1);
        QVERIFY(!entries(f.packs()).first().startsWith(".staging-"));
    }
    void staleStagingIgnoredAndCleaned() {
        QTemporaryDir root;
        const auto packs = root.filePath("imported-sound-packs");
        QVERIFY(write(packs + "/.staging-abc123/done.mp3", mp3Bytes));
        RecordingOutput output;
        NotificationSoundService service(packs, &output);
        QVERIFY(!QFileInfo::exists(packs + "/.staging-abc123"));
        QVERIFY(write(packs + "/.staging-late/done.mp3", mp3Bytes));
        QCOMPARE(service.packs().size(), 3);
        QCOMPARE(service.resolvePack(".staging-late"), QString("ade-chime"));
    }
    void packsListingAndDisplayNames() {
        Fixture f;
        const auto a = QString("a0000000-0000-4000-8000-00000000000a"), b = QString("00000000-0000-4000-8000-00000000000b");
        QVERIFY(write(f.packs() + "/" + b + "/done.mp3", mp3Bytes));
        QVERIFY(write(f.packs() + "/" + b + "/pack.json", R"({"name":"Named pack"})"));
        QVERIFY(write(f.packs() + "/" + a + "/error.wav", pcm16({1, 2})));
        QVERIFY(QDir().mkpath(f.packs() + "/00000000-0000-4000-8000-00000000000c")); // no event file
        QVERIFY(write(f.packs() + "/not-a-uuid/done.mp3", mp3Bytes));
        const auto packs = f.service->packs();
        QStringList ids;
        for (const auto& pack : packs) ids.append(pack.id);
        QCOMPARE(ids, QStringList({"ade-chime", "ade-soft", "ade-pulse", b, a}));
        QCOMPARE(packs[0].name, QString("ADE Chime"));
        QCOMPARE(packs[3].name, QString("Named pack"));
        QCOMPARE(packs[4].name, QString("A") + a.mid(1));
        QVERIFY(packs[0].builtin && !packs[3].builtin);
        QCOMPARE(f.service->resolvePack("not-a-uuid"), QString("ade-chime"));
    }
    void previewUsesDoneSoundAndDoesNotArmDebounce() {
        Fixture f;
        QVERIFY(f.service->preview("ade-soft", "loud"));
        QCOMPARE(f.output.wavs.size(), 1);
        QCOMPARE(f.output.wavs.first(), NotificationSoundService::synthesizeWav("ade-soft", AttentionEvent::Done));
        QVERIFY(f.service->preview("missing", "normal"));
        QCOMPARE(f.output.wavs.last(), NotificationSoundService::scalePcm16Wav(NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Done), 0.251));
        QVERIFY(!f.service->preview("ade-soft", "off"));
        QCOMPARE(f.output.wavs.size(), 2);
        QVERIFY(f.service->notify(AttentionEvent::Done, "a", false, false));
        // A pack without a done sound cannot preview.
        const auto id = QString("00000000-0000-4000-8000-0000000000ee");
        QVERIFY(write(f.packs() + "/" + id + "/error.mp3", mp3Bytes));
        QVERIFY(!f.service->preview(id, "loud"));
    }
    void winOutputSmoke() {
        if (qEnvironmentVariable("ADE_AUDIO_SMOKE") != "1") QSKIP("Set ADE_AUDIO_SMOKE=1 to play two short tones.");
        WinSoundOutput output;
        const auto wav = NotificationSoundService::scalePcm16Wav(NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Done), 0.251);
        QVERIFY(output.playWav(wav));
        // Immediately replace it: exercises stop-then-replace ordering.
        QVERIFY(output.playWav(NotificationSoundService::synthesizeWav("ade-chime", AttentionEvent::Question)));
        QTest::qWait(700);
        output.stop();
    }
};

QTEST_MAIN(NotificationSoundsTest)
#include "test_notification_sounds.moc"
