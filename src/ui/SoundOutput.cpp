#include "SoundOutput.h"
#include <QDir>
#include <QtGlobal>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace ade {
namespace {
constexpr const wchar_t* alias = L"ade_notify";
bool mci(const QString& command) {
    return mciSendStringW(reinterpret_cast<LPCWSTR>(command.utf16()), nullptr, 0, nullptr) == 0;
}
void stopMemorySound() { PlaySoundW(nullptr, nullptr, 0); }
}

WinSoundOutput::~WinSoundOutput() { stop(); }

void WinSoundOutput::closeFile() {
    if (!m_fileOpen) return;
    mci(QString("close %1").arg(QString::fromWCharArray(alias)));
    m_fileOpen = false;
}

bool WinSoundOutput::playWav(const QByteArray& wav) {
    // Stop the asynchronous player before replacing the buffer it reads.
    stopMemorySound();
    closeFile();
    m_buffer = wav;
    if (m_buffer.isEmpty()) return false;
    return PlaySoundW(reinterpret_cast<LPCWSTR>(m_buffer.constData()), nullptr,
                      SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
}

bool WinSoundOutput::playFile(const QString& path, double gain) {
    stopMemorySound();
    m_buffer.clear();
    closeFile();
    const auto native = QDir::toNativeSeparators(path);
    if (native.isEmpty() || native.contains('"')) return false;
    const auto name = QString::fromWCharArray(alias);
    if (!mci(QString("open \"%1\" type mpegvideo alias %2").arg(native, name))) return false;
    m_fileOpen = true;
    const int volume = qBound(0, qRound(qBound(0.0, gain, 1.0) * 1000.0), 1000);
    // Windows N editions can lack the MP3 decoder; any MCI error is a failure.
    if (!mci(QString("setaudio %1 volume to %2").arg(name).arg(volume)) || !mci(QString("play %1").arg(name))) {
        closeFile();
        return false;
    }
    return true;
}

void WinSoundOutput::stop() {
    stopMemorySound();
    closeFile();
    m_buffer.clear();
}

} // namespace ade
