#pragma once
#include <QByteArray>
#include <QString>
namespace ade::gitdetail {
// Settings scripts are bounded UTF-8 text. Preserve existing BOMs/delimiters;
// reject unsupported bytes rather than displaying replacement characters.
bool decodeSetupText(const QByteArray &bytes,QString *text,QString *error);
bool encodeSetupText(const QByteArray &baseline,const QString &text,QByteArray *bytes,QString *error);
}
