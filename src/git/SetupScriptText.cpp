#include "SetupScriptText.h"
#include <QStringConverter>
#include <QVector>
#include <QHash>
namespace ade::gitdetail {
namespace {
struct Decoded { QString text;QVector<QString> endings;QString fallback="\n";bool bom=false; };
bool decode(const QByteArray &bytes,Decoded *out,QString *error){
    if(error)error->clear();
    if(bytes.size()>1024*1024){if(error)*error="Setup scripts exceed the 1 MiB limit.";return false;}
    QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
    const QString source=decoder(bytes);
    if(decoder.hasError()||source.contains(QChar::Null)){if(error)*error="Setup script is not valid UTF-8 text. Its original bytes were preserved.";return false;}
    *out={};out->bom=bytes.startsWith(QByteArray::fromHex("efbbbf"));
    for(qsizetype i=0;i<source.size();++i){
        const auto c=source[i];QString ending;
        if(c=='\r'){ending="\r";if(i+1<source.size()&&source[i+1]=='\n'){ending="\r\n";++i;}}
        else if(c=='\n'||c==QChar(0x2028)||c==QChar(0x2029))ending=c;
        if(ending.isEmpty())out->text+=c;else{out->text+='\n';out->endings.append(ending);}
    }
    QHash<QString,int> counts;for(const auto &ending:out->endings)++counts[ending];
    int most=0;for(const auto &ending:out->endings){const int count=counts.value(ending);if(count>most){most=count;out->fallback=ending;}}
    return true;
}
}
bool decodeSetupText(const QByteArray &bytes,QString *text,QString *error){Decoded data;if(!decode(bytes,&data,error))return false;*text=data.text;return true;}
bool encodeSetupText(const QByteArray &baseline,const QString &text,QByteArray *bytes,QString *error){
    if(text.size()>1024*1024){if(error)*error="Setup scripts exceed the 1 MiB limit.";return false;}
    Decoded data;if(!decode(baseline,&data,error))return false;
    if(text.contains(QChar::Null)||text.contains('\r')||text.contains(QChar(0x2028))||text.contains(QChar(0x2029))){if(error)*error="Setup editor text must use normalized line breaks and contain no null characters.";return false;}
    if(text==data.text){*bytes=baseline;return true;}
    // Same prefix/suffix delimiter reconciliation as native TextFileIO; this
    // small UTF-8-only helper keeps the Git backend independent of QtWidgets.
    qsizetype prefix=0,suffix=0;while(prefix<data.text.size()&&prefix<text.size()&&data.text[prefix]==text[prefix])++prefix;
    while(suffix<data.text.size()-prefix&&suffix<text.size()-prefix&&data.text[data.text.size()-1-suffix]==text[text.size()-1-suffix])++suffix;
    const auto start=QStringView(data.text).left(prefix).count('\n');
    const auto removed=QStringView(data.text).mid(prefix,data.text.size()-prefix-suffix).count('\n');
    const auto added=QStringView(text).mid(prefix,text.size()-prefix-suffix).count('\n');
    const auto previous=data.endings;data.endings.remove(start,removed);
    for(qsizetype i=0;i<added;++i)data.endings.insert(start+i,i<removed?previous[start+i]:data.fallback);
    QString restored;qsizetype line=0;for(const auto c:text){if(c=='\n')restored+=data.endings[line++];else restored+=c;}
    QStringEncoder encoder(QStringEncoder::Utf8,QStringConverter::Flag::Stateless);*bytes=encoder(restored);
    if(encoder.hasError()){if(error)*error="Setup script contains an invalid Unicode sequence.";return false;}
    if(data.bom)bytes->prepend(QByteArray::fromHex("efbbbf"));
    if(bytes->size()>1024*1024){if(error)*error="Setup scripts exceed the 1 MiB limit.";return false;}
    return true;
}
}
