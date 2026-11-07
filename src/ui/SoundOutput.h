#pragma once
#include <QByteArray>
#include <QString>

namespace ade {

// Plays notification audio. Implementations are used on the GUI thread only.
class ISoundOutput {
public:
    virtual ~ISoundOutput() = default;
    // A complete RIFF/WAVE PCM file; the volume is already applied to samples.
    virtual bool playWav(const QByteArray& wav) = 0;
    // A compressed file (MP3) played at the given linear gain in [0, 1].
    virtual bool playFile(const QString& path, double gain) = 0;
    virtual void stop() = 0;
};

// Tests and headless hosts: never plays and reports nothing delivered.
class NullSoundOutput final : public ISoundOutput {
public:
    bool playWav(const QByteArray&) override { return false; }
    bool playFile(const QString&, double) override { return false; }
    void stop() override {}
};

// winmm output: PlaySound for in-memory WAV, MCI mpegvideo for MP3. Only one
// sound plays at a time; a new sound stops the previous one first.
class WinSoundOutput final : public ISoundOutput {
public:
    WinSoundOutput() = default;
    WinSoundOutput(const WinSoundOutput&) = delete;
    WinSoundOutput& operator=(const WinSoundOutput&) = delete;
    ~WinSoundOutput() override;
    bool playWav(const QByteArray& wav) override;
    bool playFile(const QString& path, double gain) override;
    void stop() override;

private:
    void closeFile();
    // PlaySound(SND_MEMORY|SND_ASYNC) reads this buffer until it is stopped.
    QByteArray m_buffer;
    bool m_fileOpen = false;
};

} // namespace ade
