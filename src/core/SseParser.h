#pragma once

#include <QByteArray>
#include <QList>

namespace ade {

// Keeps bytes until complete lines/events so split UTF-8 sequences are preserved.
class SseParser {
public:
    QList<QByteArray> feed(const QByteArray &bytes);
    QList<QByteArray> finish();
    void reset();
    bool exceededLimit() const { return m_overflow; }
private:
    void processLine(QByteArray line, QList<QByteArray> &events);
    QByteArray m_buffer;
    QByteArray m_data;
    bool m_hasData = false;
    bool m_overflow = false;
    static constexpr qsizetype MaxEventBytes = 4 * 1024 * 1024;
};

} // namespace ade
