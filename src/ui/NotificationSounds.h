#pragma once
#include "AgentActivityMonitor.h"
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace ade {
class ISoundOutput;

struct SoundPackInfo {
    QString id;
    QString name;
    bool builtin = false;
};

struct SoundImportResult {
    QString id;
    QString name;
    QString error;
};

// Notification sounds for agent attention events, following the upstream
// trigger rules: focus resets the debounce, only Done is debounced (10 s), a
// sound is suppressed only while the triggering chat is visible in the active
// window, and the debounce is armed only when a sound was delivered.
//
// Built-in packs are synthesized PCM (ADE-owned, no binary assets). Imported
// packs live in UUID-named folders under the imported-packs directory.
class NotificationSoundService final : public QObject {
public:
    using Clock = std::function<qint64()>;
    NotificationSoundService(QString importedPacksDir, ISoundOutput* output, Clock clock = {}, QObject* parent = nullptr);

    // Reads agent.notificationLoudness and agent.notificationSound. The pack
    // listing is enumerated only when the requested pack id changes (or after
    // an import), not on every preference save.
    void configure(const QJsonObject& agentPreferences);
    QString activePack() const { return m_pack; }
    QString loudness() const { return m_loudness; }
    QString importedPacksDirectory() const { return m_dir; }

    // Built-in packs first, then imported packs sorted by id.
    QList<SoundPackInfo> packs() const;
    // "default", unknown and invalid ids resolve to the default built-in pack.
    QString resolvePack(const QString& id) const;

    bool notify(AttentionEvent event, const QString& agentId, bool windowFocused, bool agentVisible);
    // Plays the pack's done sound; false when loudness is off or the pack has
    // no done sound. Never arms the debounce.
    bool preview(const QString& packId, const QString& loudness);
    // A single .mp3/.wav file, or a JSON manifest {name, description?, sounds}.
    SoundImportResult importFile(const QString& path);

    static constexpr qint64 doneDebounceMs = 10000;
    static constexpr qint64 maxAudioBytes = 5 * 1024 * 1024;
    static constexpr qint64 maxManifestBytes = 64 * 1024;
    static constexpr int maxImportedPacks = 256;
    static constexpr int sampleRate = 44100;
    static QString defaultPackId() { return QStringLiteral("ade-chime"); }
    static QList<SoundPackInfo> builtinPacks();
    static QString eventName(AttentionEvent event);

    // Deterministic 16-bit mono 44.1 kHz WAV for a built-in pack and event.
    static QByteArray synthesizeWav(const QString& pack, AttentionEvent event);
    // Multiplies every sample by gain in [0, 4] with clamping. Empty on
    // malformed or non-PCM16 input.
    static QByteArray scalePcm16Wav(const QByteArray& wav, double gain);
    // "ID3", or frame sync: b0 == 0xFF && (b1 & 0xE0) == 0xE0 (upstream mask).
    static bool looksLikeMp3(const QByteArray& head);
    // RIFF/WAVE, PCM format 1, 16-bit, mono or stereo, bounded chunk walk.
    static bool looksLikePcm16Wav(const QByteArray& bytes);
    // off = 0, normal (Subtle, -12 dB) = 0.251, loud = 1.0; unknown = normal.
    static double gainFor(const QString& loudness);

private:
    struct StagedSound {
        QString event;
        QString extension;
        QByteArray bytes;
    };
    bool play(const QString& pack, AttentionEvent event, double gain);
    bool loadAudio(const QString& path, const QString& displayName, StagedSound* sound, QString* error) const;
    SoundImportResult importManifest(const QString& path);
    SoundImportResult commit(const QString& name, const QString& description, const QList<StagedSound>& sounds);
    int importedPackCount() const;
    void removeStaleStaging() const;

    QString m_dir;
    ISoundOutput* m_output = nullptr;
    Clock m_clock;
    QString m_pack = defaultPackId();
    std::optional<QString> m_requestedPack; // the id m_pack was resolved from
    QString m_loudness = QStringLiteral("normal");
    std::optional<qint64> m_lastPlayedAt;
};

} // namespace ade
