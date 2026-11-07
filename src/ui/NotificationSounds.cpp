#include "NotificationSounds.h"
#include "SoundOutput.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ade {
namespace {
constexpr AttentionEvent allEvents[] = {AttentionEvent::Done, AttentionEvent::Question, AttentionEvent::Error};

bool isLink(const QFileInfo& info) {
    return info.isSymLink() || info.isSymbolicLink() || info.isJunction() || info.isShortcut();
}

bool isPackDirectoryName(const QString& name) {
    const auto uuid = QUuid::fromString(name);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == name;
}

quint16 le16(const QByteArray& bytes, qsizetype offset) {
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}
quint32 le32(const QByteArray& bytes, qsizetype offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}

struct PcmLayout {
    qsizetype dataOffset = 0;
    qsizetype dataSize = 0;
};

// Walks at most 64 chunks and rejects truncated chunks, compressed or
// extensible formats, and anything but 16-bit mono or stereo PCM.
bool parsePcm16(const QByteArray& bytes, PcmLayout* layout) {
    if (bytes.size() < 12 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WAVE") return false;
    qsizetype offset = 12;
    bool format = false;
    int blockAlign = 0;
    for (int chunk = 0; chunk < 64 && offset + 8 <= bytes.size(); ++chunk) {
        const auto id = bytes.mid(offset, 4);
        const qsizetype size = le32(bytes, offset + 4);
        const qsizetype body = offset + 8;
        if (size > bytes.size() - body) return false;
        if (id == "fmt ") {
            if (format || size < 16) return false;
            const int audioFormat = le16(bytes, body);
            const int channels = le16(bytes, body + 2);
            const quint32 rate = le32(bytes, body + 4);
            const quint32 byteRate = le32(bytes, body + 8);
            blockAlign = le16(bytes, body + 12);
            const int bits = le16(bytes, body + 14);
            if (audioFormat != 1 || (channels != 1 && channels != 2) || bits != 16 || blockAlign != channels * 2
                || rate < 8000 || rate > 192000 || byteRate != rate * quint32(blockAlign))
                return false;
            format = true;
        } else if (id == "data") {
            if (!format || size <= 0 || size % blockAlign != 0) return false;
            layout->dataOffset = body;
            layout->dataSize = size;
            return true;
        }
        offset = body + size + (size & 1);
    }
    return false;
}

QByteArray wavFile(const QByteArray& samples) {
    QByteArray header(44, '\0');
    auto* out = reinterpret_cast<uchar*>(header.data());
    memcpy(out, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(36 + samples.size()), out + 4);
    memcpy(out + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, out + 16);
    qToLittleEndian<quint16>(1, out + 20);
    qToLittleEndian<quint16>(1, out + 22);
    qToLittleEndian<quint32>(NotificationSoundService::sampleRate, out + 24);
    qToLittleEndian<quint32>(NotificationSoundService::sampleRate * 2, out + 28);
    qToLittleEndian<quint16>(2, out + 32);
    qToLittleEndian<quint16>(16, out + 34);
    memcpy(out + 36, "data", 4);
    qToLittleEndian<quint32>(quint32(samples.size()), out + 40);
    return header + samples;
}

// One note: a linear glide from `from` to `to` Hz.
struct Tone {
    double startMs;
    double durationMs;
    double from;
    double to;
};
// Harmonic weights and an attack/exponential-decay envelope per pack.
struct Timbre {
    double second;
    double third;
    double fifth;
    double attackMs;
    double decayMs;
    double peak;
};

struct Voice {
    Timbre timbre;
    QList<Tone> done, question, error;
};

const QHash<QString, Voice>& voices() {
    // Pitches are equal-tempered notes. Done rises, Question is two equal
    // pings, Error falls. Every sound ends within 600 ms.
    static const QHash<QString, Voice> table{
        {"ade-chime", {{0.35, 0.12, 0.0, 4.0, 220.0, 0.42},
                       {{0, 170, 659.25, 659.25}, {150, 280, 880.00, 880.00}},
                       {{0, 150, 783.99, 783.99}, {200, 150, 783.99, 783.99}},
                       {{0, 380, 523.25, 349.23}}}},
        {"ade-soft", {{0.08, 0.0, 0.0, 18.0, 320.0, 0.30},
                      {{0, 220, 523.25, 523.25}, {180, 300, 659.25, 659.25}},
                      {{0, 200, 587.33, 587.33}, {240, 200, 587.33, 587.33}},
                      {{0, 450, 440.00, 293.66}}}},
        {"ade-pulse", {{0.0, 0.33, 0.20, 2.0, 90.0, 0.32},
                       {{0, 110, 880.00, 880.00}, {120, 140, 1174.66, 1174.66}},
                       {{0, 90, 987.77, 987.77}, {160, 90, 987.77, 987.77}},
                       {{0, 300, 698.46, 466.16}}}},
    };
    return table;
}
}

NotificationSoundService::NotificationSoundService(QString importedPacksDir, ISoundOutput* output, Clock clock, QObject* parent)
    : QObject(parent), m_dir(QDir::cleanPath(std::move(importedPacksDir))), m_output(output), m_clock(std::move(clock)) {
    if (!m_clock) m_clock = [] { return QDateTime::currentMSecsSinceEpoch(); };
    removeStaleStaging();
}

QList<SoundPackInfo> NotificationSoundService::builtinPacks() {
    return {{"ade-chime", "ADE Chime", true}, {"ade-soft", "Soft Tones", true}, {"ade-pulse", "Digital Pulse", true}};
}

QString NotificationSoundService::eventName(AttentionEvent event) {
    switch (event) {
    case AttentionEvent::Done: return QStringLiteral("done");
    case AttentionEvent::Question: return QStringLiteral("question");
    case AttentionEvent::Error: return QStringLiteral("error");
    }
    return {};
}

double NotificationSoundService::gainFor(const QString& loudness) {
    if (loudness == "off") return 0.0;
    if (loudness == "loud") return 1.0;
    // -12 dB amplitude scalar, matching upstream "subtle".
    return 0.251;
}

void NotificationSoundService::configure(const QJsonObject& agentPreferences) {
    const auto loudness = agentPreferences.value("notificationLoudness").toString("normal");
    m_loudness = QStringList{"off", "normal", "loud"}.contains(loudness) ? loudness : QStringLiteral("normal");
    const auto requested = agentPreferences.value("notificationSound").toString("default");
    if (m_requestedPack == requested) return;
    m_requestedPack = requested;
    m_pack = resolvePack(requested);
}

void NotificationSoundService::removeStaleStaging() const {
    QDir dir(m_dir);
    if (!dir.exists()) return;
    const auto stale = dir.entryInfoList({".staging-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto& info : stale)
        if (!isLink(info)) QDir(info.absoluteFilePath()).removeRecursively();
}

QList<SoundPackInfo> NotificationSoundService::packs() const {
    auto result = builtinPacks();
    QList<SoundPackInfo> imported;
    const auto entries = QDir(m_dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const auto& info : entries) {
        const auto id = info.fileName();
        if (!isPackDirectoryName(id) || isLink(info)) continue;
        bool hasSound = false;
        for (const auto event : allEvents)
            for (const auto& extension : {".wav", ".mp3"}) {
                const QFileInfo file(info.absoluteFilePath() + "/" + eventName(event) + extension);
                hasSound |= file.isFile() && !isLink(file);
            }
        if (!hasSound) continue;
        QString name;
        QFile manifest(info.absoluteFilePath() + "/pack.json");
        if (manifest.open(QIODevice::ReadOnly) && manifest.size() <= maxManifestBytes)
            name = QJsonDocument::fromJson(manifest.read(maxManifestBytes)).object().value("name").toString().trimmed().left(200);
        if (name.isEmpty()) name = id.left(1).toUpper() + id.mid(1);
        imported.append({id, name, false});
    }
    std::sort(imported.begin(), imported.end(), [](const SoundPackInfo& a, const SoundPackInfo& b) { return a.id < b.id; });
    return result + imported;
}

QString NotificationSoundService::resolvePack(const QString& id) const {
    if (id.isEmpty() || id == "default") return defaultPackId();
    // Resolve only through the enumerated listing; never join a stored id
    // into a path.
    for (const auto& pack : packs())
        if (pack.id == id) return id;
    return defaultPackId();
}

bool NotificationSoundService::notify(AttentionEvent event, const QString& agentId, bool windowFocused, bool agentVisible) {
    Q_UNUSED(agentId);
    if (windowFocused) m_lastPlayedAt.reset();
    const qint64 now = m_clock();
    if (event == AttentionEvent::Done && m_lastPlayedAt && now - *m_lastPlayedAt < doneDebounceMs) return false;
    if (agentVisible && windowFocused) return false;
    if (m_loudness == "off") return false;
    if (!play(m_pack, event, gainFor(m_loudness))) return false;
    m_lastPlayedAt = now;
    return true;
}

bool NotificationSoundService::preview(const QString& packId, const QString& loudness) {
    if (loudness == "off") return false;
    return play(resolvePack(packId), AttentionEvent::Done, gainFor(loudness));
}

bool NotificationSoundService::play(const QString& pack, AttentionEvent event, double gain) {
    if (!m_output) return false;
    if (voices().contains(pack)) {
        static QHash<QString, QByteArray> cache;
        const auto key = pack + "/" + eventName(event);
        if (!cache.contains(key)) cache.insert(key, synthesizeWav(pack, event));
        const auto wav = scalePcm16Wav(cache.value(key), gain);
        return !wav.isEmpty() && m_output->playWav(wav);
    }
    const auto base = m_dir + "/" + pack + "/" + eventName(event);
    const QFileInfo wavInfo(base + ".wav");
    if (wavInfo.isFile() && !isLink(wavInfo)) {
        QFile file(wavInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly) || file.size() > maxAudioBytes) return false;
        const auto wav = scalePcm16Wav(file.read(maxAudioBytes), gain);
        return !wav.isEmpty() && m_output->playWav(wav);
    }
    const QFileInfo mp3Info(base + ".mp3");
    if (mp3Info.isFile() && !isLink(mp3Info)) return m_output->playFile(mp3Info.absoluteFilePath(), gain);
    // A pack without this event delivers nothing.
    return false;
}

QByteArray NotificationSoundService::synthesizeWav(const QString& pack, AttentionEvent event) {
    const auto voice = voices().value(voices().contains(pack) ? pack : defaultPackId());
    const auto& tones = event == AttentionEvent::Done ? voice.done : event == AttentionEvent::Question ? voice.question : voice.error;
    double endMs = 0;
    for (const auto& tone : tones) endMs = std::max(endMs, tone.startMs + tone.durationMs);
    const int frames = int(std::ceil(endMs * sampleRate / 1000.0));
    std::vector<double> mix(size_t(frames), 0.0);
    const auto& timbre = voice.timbre;
    const double norm = 1.0 + timbre.second + timbre.third + timbre.fifth;
    constexpr double twoPi = 6.283185307179586;
    constexpr double releaseMs = 8.0;
    for (const auto& tone : tones) {
        const int first = int(std::lround(tone.startMs * sampleRate / 1000.0));
        const int count = int(std::lround(tone.durationMs * sampleRate / 1000.0));
        double phase = 0.0;
        for (int i = 0; i < count && first + i < frames; ++i) {
            const double ms = i * 1000.0 / sampleRate;
            const double frequency = tone.from + (tone.to - tone.from) * (ms / tone.durationMs);
            double envelope = ms < timbre.attackMs ? ms / timbre.attackMs : std::exp(-(ms - timbre.attackMs) / timbre.decayMs);
            const double remaining = tone.durationMs - ms;
            if (remaining < releaseMs) envelope *= std::max(0.0, remaining / releaseMs);
            const double wave = std::sin(phase) + timbre.second * std::sin(2 * phase) + timbre.third * std::sin(3 * phase)
                + timbre.fifth * std::sin(5 * phase);
            mix[size_t(first + i)] += timbre.peak * envelope * wave / norm;
            phase += twoPi * frequency / sampleRate;
            if (phase > twoPi) phase -= twoPi;
        }
    }
    QByteArray samples(frames * 2, '\0');
    auto* out = reinterpret_cast<uchar*>(samples.data());
    for (int i = 0; i < frames; ++i) {
        const auto value = qint16(std::clamp<long>(std::lround(std::clamp(mix[size_t(i)], -1.0, 1.0) * 32767.0), -32768, 32767));
        qToLittleEndian<qint16>(value, out + i * 2);
    }
    return wavFile(samples);
}

QByteArray NotificationSoundService::scalePcm16Wav(const QByteArray& wav, double gain) {
    PcmLayout layout;
    if (!(gain >= 0.0 && gain <= 4.0) || !parsePcm16(wav, &layout)) return {};
    QByteArray scaled = wav;
    auto* data = reinterpret_cast<uchar*>(scaled.data()) + layout.dataOffset;
    for (qsizetype i = 0; i + 1 < layout.dataSize; i += 2) {
        const auto sample = qFromLittleEndian<qint16>(data + i);
        const long value = std::clamp<long>(std::lround(sample * gain), -32768, 32767);
        qToLittleEndian<qint16>(qint16(value), data + i);
    }
    return scaled;
}

bool NotificationSoundService::looksLikeMp3(const QByteArray& head) {
    if (head.size() >= 3 && head.startsWith("ID3")) return true;
    return head.size() >= 2 && uchar(head[0]) == 0xFF && (uchar(head[1]) & 0xE0) == 0xE0;
}

bool NotificationSoundService::looksLikePcm16Wav(const QByteArray& bytes) {
    PcmLayout layout;
    return parsePcm16(bytes, &layout);
}

bool NotificationSoundService::loadAudio(const QString& path, const QString& displayName, StagedSound* sound, QString* error) const {
    const QFileInfo info(path);
    const auto lower = path.toLower();
    const bool wav = lower.endsWith(".wav"), mp3 = lower.endsWith(".mp3");
    if (!wav && !mp3) { *error = QString("\"%1\" is not an .mp3 or .wav file.").arg(displayName); return false; }
    if (isLink(info)) { *error = QString("\"%1\" must be a regular file, not a link or shortcut.").arg(displayName); return false; }
    if (!info.exists() || !info.isFile()) { *error = QString("Sound file \"%1\" not found.").arg(displayName); return false; }
    if (info.size() > maxAudioBytes) { *error = QString("\"%1\" is larger than 5 MiB.").arg(displayName); return false; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = QString("Failed to read \"%1\". The file may be corrupted.").arg(displayName); return false; }
    // Validate and copy the same bytes, so the file cannot change in between.
    const auto bytes = file.read(maxAudioBytes + 1);
    if (bytes.size() > maxAudioBytes) { *error = QString("\"%1\" is larger than 5 MiB.").arg(displayName); return false; }
    if (mp3 && !looksLikeMp3(bytes.left(3))) { *error = QString("\"%1\" does not appear to be a valid MP3 file.").arg(displayName); return false; }
    if (wav && !looksLikePcm16Wav(bytes)) { *error = QString("\"%1\" is not a 16-bit PCM mono or stereo WAV file.").arg(displayName); return false; }
    sound->extension = wav ? QStringLiteral("wav") : QStringLiteral("mp3");
    sound->bytes = bytes;
    return true;
}

int NotificationSoundService::importedPackCount() const {
    int count = 0;
    for (const auto& name : QDir(m_dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (isPackDirectoryName(name)) ++count;
    return count;
}

SoundImportResult NotificationSoundService::importFile(const QString& path) {
    const auto failure = [](const QString& message) { return SoundImportResult{{}, {}, message}; };
    const QFileInfo source(path);
    const auto lower = path.toLower();
    const bool manifest = lower.endsWith(".json");
    if (!lower.endsWith(".mp3") && !lower.endsWith(".wav") && !manifest)
        return failure("Select an .mp3 or .wav file or a sound pack .json manifest.");
    if (isLink(source)) return failure(QString("\"%1\" must be a regular file, not a link or shortcut.").arg(source.fileName()));
    if (importedPackCount() >= maxImportedPacks)
        return failure(QString("You can import up to %1 sound packs. Remove unused packs from %2 and try again.")
                           .arg(maxImportedPacks).arg(QDir::toNativeSeparators(m_dir)));
    if (manifest) return importManifest(path);
    StagedSound sound;
    QString error;
    if (!loadAudio(path, source.fileName(), &sound, &error)) return failure(error);
    QList<StagedSound> sounds;
    for (const auto event : allEvents) {
        sound.event = eventName(event);
        sounds.append(sound);
    }
    return commit(source.completeBaseName(),
                  QString("Custom sound imported from a single %1 file.").arg(sound.extension.toUpper()), sounds);
}

SoundImportResult NotificationSoundService::importManifest(const QString& path) {
    const auto failure = [](const QString& message) { return SoundImportResult{{}, {}, message}; };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return failure("Failed to read file: " + file.errorString());
    const auto bytes = file.read(maxManifestBytes + 1);
    if (file.size() > maxManifestBytes || bytes.size() > maxManifestBytes) return failure("The sound pack manifest is larger than 64 KiB.");
    file.close();
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError) return failure("The file is not valid JSON.");
    if (!document.isObject()) return failure("Invalid sound pack JSON: Expected an object.");
    const auto object = document.object();
    QStringList issues;
    const auto name = object.value("name");
    if (!name.isString() || name.toString().isEmpty()) issues.append("name: The \"name\" field is required and must be a non-empty string.");
    if (object.contains("description") && !object.value("description").isString()) issues.append("description: Expected a string.");
    const auto soundsValue = object.value("sounds");
    if (!soundsValue.isObject()) issues.append("sounds: Expected an object.");
    const auto sounds = soundsValue.toObject();
    for (const auto event : allEvents) {
        const auto value = sounds.value(eventName(event));
        if (!value.isUndefined() && !value.isNull() && (!value.isString() || value.toString().isEmpty()))
            issues.append(QString("sounds.%1: Expected a non-empty string or null.").arg(eventName(event)));
    }
    if (!issues.isEmpty()) return failure("Invalid sound pack JSON: " + issues.join("; "));
    bool named = false;
    for (const auto event : allEvents) named |= sounds.value(eventName(event)).isString();
    if (!named) return failure("The sound pack must specify at least one sound file (done, question, or error).");

    // Prefixes end in one separator, also for a manifest at a drive root.
    const auto prefix = [](const QString& directory) { return directory.endsWith('/') ? directory : directory + '/'; };
    const auto manifestDirectory = QDir::cleanPath(QFileInfo(path).absolutePath());
    const auto sourceDirectory = prefix(manifestDirectory);
    const auto canonicalPath = QFileInfo(manifestDirectory).canonicalFilePath();
    const auto canonicalDirectory = canonicalPath.isEmpty() ? QString() : prefix(canonicalPath);
    QList<StagedSound> staged;
    QStringList errors;
    for (const auto event : allEvents) {
        const auto value = sounds.value(eventName(event));
        if (!value.isString()) continue;
        const auto fileName = value.toString();
        const auto relative = QDir::fromNativeSeparators(fileName);
        const auto outside = QString("\"%1\" must be relative to the manifest file.").arg(fileName);
        // Drive letters, NTFS alternate streams, absolute and UNC names.
        if (relative.contains(':') || relative.startsWith('/') || QDir::isAbsolutePath(relative)) { errors.append(outside); continue; }
        const auto resolved = QDir::cleanPath(sourceDirectory + relative);
        if (!resolved.startsWith(sourceDirectory, Qt::CaseInsensitive)) { errors.append(outside); continue; }
        // No component below the manifest folder may be a link or junction.
        bool linked = false;
        QString walk = manifestDirectory;
        for (const auto& part : resolved.mid(sourceDirectory.size()).split('/', Qt::SkipEmptyParts)) {
            walk = prefix(walk) + part;
            if (isLink(QFileInfo(walk))) { linked = true; break; }
        }
        if (linked) { errors.append(QString("\"%1\" must be a regular file, not a link or shortcut.").arg(fileName)); continue; }
        const QFileInfo info(resolved);
        if (info.exists()) {
            // Links or junctions inside the manifest folder must not escape it.
            const auto canonical = info.canonicalFilePath();
            if (canonicalDirectory.isEmpty() || canonical.isEmpty()
                || !canonical.startsWith(canonicalDirectory, Qt::CaseInsensitive)) {
                errors.append(outside);
                continue;
            }
        }
        StagedSound sound;
        QString error;
        if (!loadAudio(resolved, fileName, &sound, &error)) { errors.append(error); continue; }
        sound.event = eventName(event);
        staged.append(sound);
    }
    if (staged.isEmpty())
        return failure("No valid sound files found." + (errors.isEmpty() ? QString() : " " + errors.join(' ')));
    return commit(name.toString(), object.value("description").toString(), staged);
}

SoundImportResult NotificationSoundService::commit(const QString& name, const QString& description, const QList<StagedSound>& sounds) {
    const auto failure = [](const QString& message) { return SoundImportResult{{}, {}, message}; };
    if (!QDir().mkpath(m_dir)) return failure("Failed to create the sound pack folder.");
    // Stage beside the packs and publish with one rename; any failure leaves
    // nothing behind. Staging names are never listed as packs.
    QTemporaryDir staging(m_dir + "/.staging-XXXXXX");
    if (!staging.isValid()) return failure("Failed to create pack directory: " + staging.errorString());
    QJsonObject files{{"done", QJsonValue::Null}, {"question", QJsonValue::Null}, {"error", QJsonValue::Null}};
    for (const auto& sound : sounds) {
        const auto fileName = sound.event + "." + sound.extension;
        QFile output(staging.filePath(fileName));
        if (!output.open(QIODevice::WriteOnly) || output.write(sound.bytes) != sound.bytes.size()) return failure("Failed to save the imported sound.");
        output.close();
        files[sound.event] = fileName;
    }
    QJsonObject metadata{{"name", name}, {"sounds", files}};
    if (!description.isEmpty()) metadata["description"] = description;
    QFile manifest(staging.filePath("pack.json"));
    const auto json = QJsonDocument(metadata).toJson();
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(json) != json.size()) return failure("Failed to save the imported sound.");
    manifest.close();
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().rename(staging.path(), m_dir + "/" + id)) return failure("Failed to save the imported sound.");
    staging.setAutoRemove(false);
    // The listing changed: the next configure() resolves the pack again.
    m_requestedPack.reset();
    return {id, name, {}};
}

} // namespace ade
