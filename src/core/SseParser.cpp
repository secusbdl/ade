#include "SseParser.h"

namespace ade {

QList<QByteArray> SseParser::feed(const QByteArray &bytes) {
    QList<QByteArray> events;
    if (m_overflow) return events;
    m_buffer += bytes;
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_buffer.size(); ++i) {
        const char c = m_buffer.at(i);
        if (c != '\n' && c != '\r') continue;
        if (c == '\r' && i + 1 == m_buffer.size()) break;
        processLine(m_buffer.mid(start, i - start), events);
        if (c == '\r' && i + 1 < m_buffer.size() && m_buffer.at(i + 1) == '\n') ++i;
        start = i + 1;
    }
    m_buffer.remove(0, start);
    if (m_buffer.size() + m_data.size() > MaxEventBytes) {
        m_overflow = true;
        m_buffer.clear();
        m_data.clear();
    }
    return events;
}

void SseParser::processLine(QByteArray line, QList<QByteArray> &events) {
    if (line.isEmpty()) {
        if (m_hasData) events.append(m_data);
        m_data.clear();
        m_hasData = false;
    } else if (line.startsWith("data:")) {
        line.remove(0, 5);
        if (line.startsWith(' ')) line.remove(0, 1);
        if (m_hasData) m_data += '\n';
        m_data += line;
        m_hasData = true;
        if (m_data.size() > MaxEventBytes) m_overflow = true;
    }
}

QList<QByteArray> SseParser::finish() {
    QList<QByteArray> events;
    if (m_overflow) return events;
    if (m_buffer.endsWith('\r')) m_buffer.chop(1);
    if (!m_buffer.isEmpty()) processLine(m_buffer, events);
    m_buffer.clear();
    if (m_hasData) events.append(m_data);
    m_data.clear();
    m_hasData = false;
    return events;
}

void SseParser::reset() {
    m_buffer.clear();
    m_data.clear();
    m_hasData = false;
    m_overflow = false;
}

} // namespace ade
