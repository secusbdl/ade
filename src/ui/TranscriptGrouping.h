#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace ade {
// View-only references. These values are derived from the raw projection, never
// read from model-controlled metadata or written back into provider history.
struct TranscriptPart {
    QString messageId;
    QJsonObject value;
    QString key() const;
    bool operator==(const TranscriptPart&) const = default;
};
struct TranscriptGroup {
    QJsonObject first;
    QStringList messageIds;
    QList<TranscriptPart> parts;
    QJsonArray attachments;
    bool operator==(const TranscriptGroup&) const = default;
};
QList<TranscriptGroup> groupTranscriptRows(const QJsonArray& rows);
}
