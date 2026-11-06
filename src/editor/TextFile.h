#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace ade {

enum class TextEncoding { Utf8, Utf16LE, Utf16BE };
enum class LineEnding : quint8 { LF, CRLF, CR, LineSeparator, ParagraphSeparator };

struct TextFile {
    QString path;
    QString text;
    QByteArray originalBytes;
    TextEncoding encoding = TextEncoding::Utf8;
    bool hasBom = false;
    QVector<LineEnding> lineEndings;
    LineEnding defaultLineEnding = LineEnding::LF;
    QString encodingName() const;
    QString lineEndingName() const;
};

class TextFileIO {
public:
    static constexpr qint64 MaxBytes = 20 * 1024 * 1024;
    static bool decode(const QByteArray &bytes, TextFile *result, QString *error = nullptr);
    static bool read(const QString &path, TextFile *result, QString *error = nullptr);
    static bool encode(const TextFile &baseline, const QString &text,
                       const QVector<LineEnding> &endings, QByteArray *bytes, QString *error = nullptr);
    static bool write(const TextFile &baseline, const QByteArray &bytes, bool force,
                      QString *error = nullptr);
    // Preserve delimiters outside the changed range. Replaced/new line breaks
    // use the prior delimiters where possible, then the document's default.
    static QVector<LineEnding> reconcileEndings(const QString &before, const QString &after,
                                               const QVector<LineEnding> &endings, LineEnding fallback);
};

} // namespace ade
