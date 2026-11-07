#include "BrowserNavigationInput.h"
#include <QRegularExpression>
namespace ade {
QUrl browserNavigationInput(const QString& input,const QString& searchTemplate){
    const auto text=input.trimmed();if(text.isEmpty())return {};
    const QUrl parsed(text,QUrl::TolerantMode);
    const auto scheme=parsed.scheme();
    if(parsed.isValid()&&(scheme=="http"||scheme=="https")&&!parsed.host().isEmpty())return parsed;
    if(parsed.isValid()&&(scheme=="file"||scheme=="about"))return parsed;
    static const QRegularExpression local("^(?:localhost(?::[0-9]+)?|127\\.0\\.0\\.1(?::[0-9]+)?|\\[::1\\](?::[0-9]+)?)(?:/|$)",QRegularExpression::CaseInsensitiveOption);
    // Match local paths before generic search. Normalize IPv6 loopback so QUrl
    // receives a legal authority. Uppercase localhost also uses HTTP deliberately.
    if(!text.contains(' ')&&local.match(text).hasMatch())return QUrl("http://"+text,QUrl::StrictMode);
    static const QRegularExpression bareLoopback("^::1(?::([0-9]+))?(/.*)?$");
    const auto loop=bareLoopback.match(text);
    if(loop.hasMatch())return QUrl("http://[::1]"+(loop.captured(1).isEmpty()?QString():":"+loop.captured(1))+loop.captured(2),QUrl::StrictMode);
    if(!text.contains(' ')&&text.split('.',Qt::SkipEmptyParts).size()>1)return QUrl("https://"+text,QUrl::StrictMode);
    auto query=searchTemplate;query.replace("%s",QString::fromLatin1(QUrl::toPercentEncoding(text)));
    return QUrl(query,QUrl::StrictMode);
}
}
