#include "TextFile.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringConverter>
#include <array>

namespace ade {
namespace {
bool fail(QString *error, const QString &message) { if(error)*error=message;return false; }
bool validUnicode(const QString &text) {
    for(qsizetype i=0;i<text.size();++i){
        if(text[i].isHighSurrogate()){
            if(i+1>=text.size()||!text[i+1].isLowSurrogate())return false;
            ++i;
        }else if(text[i].isLowSurrogate())return false;
    }
    return true;
}
QString delimiter(LineEnding ending) {
    switch(ending){
    case LineEnding::CRLF:return "\r\n";
    case LineEnding::CR:return "\r";
    case LineEnding::LineSeparator:return QString(QChar(0x2028));
    case LineEnding::ParagraphSeparator:return QString(QChar(0x2029));
    default:return "\n";
    }
}
QStringConverter::Encoding converter(TextEncoding encoding) {
    return encoding==TextEncoding::Utf16LE?QStringConverter::Utf16LE:
           encoding==TextEncoding::Utf16BE?QStringConverter::Utf16BE:QStringConverter::Utf8;
}
}

QString TextFile::encodingName() const {
    return (encoding==TextEncoding::Utf8?QString("UTF-8"):
            encoding==TextEncoding::Utf16LE?QString("UTF-16 LE"):QString("UTF-16 BE"))+(hasBom?" BOM":"");
}
QString TextFile::lineEndingName() const {
    if(!lineEndings.isEmpty())for(const auto ending:lineEndings)if(ending!=lineEndings.first())return "Mixed EOL";
    switch(defaultLineEnding){case LineEnding::CRLF:return "CRLF";case LineEnding::CR:return "CR";
    case LineEnding::LineSeparator:return "LS";case LineEnding::ParagraphSeparator:return "PS";default:return "LF";}
}

bool TextFileIO::decode(const QByteArray &bytes,TextFile *result,QString *error) {
    if(error)error->clear();
    if(!result)return fail(error,"No document destination was supplied.");
    if(bytes.size()>MaxBytes)return fail(error,"File exceeds the 20 MiB editable-file limit.");
    TextFile file;file.originalBytes=bytes;
    QByteArray payload=bytes;
    if(payload.startsWith(QByteArray::fromHex("0000feff"))||payload.startsWith(QByteArray::fromHex("fffe0000")))
        return fail(error,"UTF-32 files are not supported by this editor.");
    if(payload.startsWith(QByteArray::fromHex("efbbbf"))){file.hasBom=true;payload.remove(0,3);}
    else if(payload.startsWith(QByteArray::fromHex("fffe"))){file.hasBom=true;file.encoding=TextEncoding::Utf16LE;payload.remove(0,2);}
    else if(payload.startsWith(QByteArray::fromHex("feff"))){file.hasBom=true;file.encoding=TextEncoding::Utf16BE;payload.remove(0,2);}
    if(file.encoding!=TextEncoding::Utf8 && payload.size()%2)return fail(error,"The UTF-16 file has an incomplete code unit.");
    QStringDecoder decoder(converter(file.encoding),QStringConverter::Flag::Stateless|QStringConverter::Flag::ConvertInitialBom);
    const QString decoded=decoder(payload);
    if(decoder.hasError()||!validUnicode(decoded))return fail(error,"The file contains invalid text encoding. Its bytes were not changed.");
    if(decoded.contains(QChar(0)))return fail(error,"Binary or NUL-containing files cannot be edited as text.");
    std::array<int,5> counts{};
    for(qsizetype i=0;i<decoded.size();++i){
        const auto ch=decoded[i];
        LineEnding ending=LineEnding::LF;bool newline=true;
        if(ch=='\r'){
            if(i+1<decoded.size()&&decoded[i+1]=='\n'){ending=LineEnding::CRLF;++i;}else ending=LineEnding::CR;
        }else if(ch=='\n')ending=LineEnding::LF;
        else if(ch.unicode()==0x2028)ending=LineEnding::LineSeparator;
        else if(ch.unicode()==0x2029)ending=LineEnding::ParagraphSeparator;
        else newline=false;
        if(newline){file.text+='\n';file.lineEndings.append(ending);++counts[static_cast<int>(ending)];}
        else file.text+=ch;
    }
    if(!file.lineEndings.isEmpty()){
        file.defaultLineEnding=file.lineEndings.first();
        for(int i=0;i<static_cast<int>(counts.size());++i)
            if(counts[i]>counts[static_cast<int>(file.defaultLineEnding)])file.defaultLineEnding=static_cast<LineEnding>(i);
    }
    *result=std::move(file);return true;
}

bool TextFileIO::read(const QString &path,TextFile *result,QString *error) {
    if(error)error->clear();
    if(!result)return fail(error,"No document destination was supplied.");
    const QFileInfo info(path);
    if(!info.exists()||!info.isFile())return fail(error,"The file no longer exists or is not a regular file.");
    QFile file(info.canonicalFilePath());
    if(!file.open(QIODevice::ReadOnly))return fail(error,file.errorString());
    if(file.size()>MaxBytes)return fail(error,"File exceeds the 20 MiB editable-file limit.");
    const auto bytes=file.read(MaxBytes+1);
    if(file.error()!=QFileDevice::NoError)return fail(error,file.errorString());
    TextFile parsed;if(!decode(bytes,&parsed,error))return false;
    parsed.path=info.canonicalFilePath();*result=std::move(parsed);return true;
}

bool TextFileIO::encode(const TextFile &baseline,const QString &text,const QVector<LineEnding> &endings,QByteArray *bytes,QString *error) {
    if(error)error->clear();
    if(!bytes)return fail(error,"No output destination was supplied.");
    if(text==baseline.text){*bytes=baseline.originalBytes;return true;}
    if(!validUnicode(text))return fail(error,"The document contains an invalid Unicode sequence.");
    if(text.contains(QChar(0)))return fail(error,"NUL characters cannot be saved in a text document.");
    QString expanded;expanded.reserve(text.size()+endings.size());qsizetype newline=0;
    for(const auto ch:text){
        if(ch=='\n'){expanded+=delimiter(newline<endings.size()?endings[newline]:baseline.defaultLineEnding);++newline;}
        else expanded+=ch;
    }
    QStringEncoder encoder(converter(baseline.encoding),QStringConverter::Flag::Stateless);
    QByteArray encoded=encoder(expanded);
    if(encoder.hasError())return fail(error,"The document contains an invalid Unicode sequence.");
    if(baseline.hasBom)encoded.prepend(QByteArray::fromHex(baseline.encoding==TextEncoding::Utf8?"efbbbf":baseline.encoding==TextEncoding::Utf16LE?"fffe":"feff"));
    if(encoded.size()>MaxBytes)return fail(error,"The saved document would exceed the 20 MiB limit.");
    *bytes=std::move(encoded);return true;
}

bool TextFileIO::write(const TextFile &baseline,const QByteArray &bytes,bool force,QString *error) {
    if(error)error->clear();
    if(baseline.path.isEmpty())return fail(error,"No file is open.");
    if(bytes.size()>MaxBytes)return fail(error,"The saved document exceeds the 20 MiB limit.");
    const QFileInfo info(baseline.path);
    if(info.exists()&&(!info.isFile()||info.canonicalFilePath()!=baseline.path))
        return fail(error,"The file path now points to a different target. Reopen it before saving.");
    if(info.exists()){
        QFile current(baseline.path);
        if(!current.open(QIODevice::ReadOnly))return fail(error,current.errorString());
        if(!force&&(current.size()>MaxBytes||current.read(MaxBytes+1)!=baseline.originalBytes))
            return fail(error,"The file changed on disk. Reload it or explicitly overwrite the external changes.");
    }else if(!force)return fail(error,"The file was deleted. Explicitly choose Re-create to save a new copy.");
    QSaveFile output(baseline.path);output.setDirectWriteFallback(false);
    if(!output.open(QIODevice::WriteOnly))return fail(error,output.errorString());
    if(output.write(bytes)!=bytes.size()||!output.commit())return fail(error,"Save failed; the previous file was preserved: "+output.errorString());
    return true;
}

QVector<LineEnding> TextFileIO::reconcileEndings(const QString &before,const QString &after,const QVector<LineEnding> &endings,LineEnding fallback) {
    qsizetype prefix=0;
    while(prefix<before.size()&&prefix<after.size()&&before[prefix]==after[prefix])++prefix;
    qsizetype suffix=0;
    while(suffix<before.size()-prefix&&suffix<after.size()-prefix&&before[before.size()-1-suffix]==after[after.size()-1-suffix])++suffix;
    const auto start=QStringView(before).left(prefix).count('\n');
    const auto removed=QStringView(before).mid(prefix,before.size()-prefix-suffix).count('\n');
    const auto added=QStringView(after).mid(prefix,after.size()-prefix-suffix).count('\n');
    QVector<LineEnding> result=endings;
    if(result.size()!=before.count('\n'))result=QVector<LineEnding>(before.count('\n'),fallback);
    const auto previous=result;
    result.remove(start,removed);
    for(qsizetype i=0;i<added;++i)result.insert(start+i,i<removed&&start+i<previous.size()?previous[start+i]:fallback);
    return result;
}

} // namespace ade
