#include "AppResourceStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QStringDecoder>
#include <vector>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ade {
namespace {
bool decodePath(const QUrl& url, QStringList* parts) {
    const QByteArray encoded = url.path(QUrl::FullyEncoded).toLatin1();
    if (!encoded.startsWith('/')) return false;
    const auto segments = encoded.mid(1).split('/');
    for (const auto& segment : segments) {
        for (qsizetype i=0;i<segment.size();++i) if (segment[i]=='%') {
            if (i+2>=segment.size()) return false;
            const auto hex=[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');};
            if (!hex(segment[i+1])||!hex(segment[i+2])) return false;
            i+=2;
        }
        QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless|QStringConverter::Flag::ConvertInitialBom);
        const QString text=decoder(QByteArray::fromPercentEncoding(segment));
        if (decoder.hasError()||!AppResourceStore::safeSegment(text)) return false;
        parts->append(text);
    }
    return true;
}
bool inside(const QString& root, const QString& path) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity=Qt::CaseInsensitive;
#else
    constexpr auto sensitivity=Qt::CaseSensitive;
#endif
    return path.startsWith(root.endsWith('/')?root:root+'/',sensitivity);
}
#ifdef Q_OS_WIN
class FileHandle final {
public:
    HANDLE value=INVALID_HANDLE_VALUE;
    ~FileHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
QString handlePath(HANDLE handle) {
    const DWORD length=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!length)return {};
    std::vector<wchar_t> buffer(length+1);
    const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),static_cast<DWORD>(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!written||written>=buffer.size())return {};
    QString result=QString::fromWCharArray(buffer.data(),written);
    if(result.startsWith(QStringLiteral("\\\\?\\UNC\\")))result=QStringLiteral("\\\\")+result.mid(8);
    else if(result.startsWith(QStringLiteral("\\\\?\\")))result.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(result));
}
#endif
AppResourceResponse failure(int status,const QByteArray& message) {
    return {status,message,"text/plain; charset=utf-8",{{"Cache-Control","no-store"},{"X-Content-Type-Options","nosniff"}}};
}
}

bool AppResourceStore::safeSegment(const QString& segment) {
    if(segment.isEmpty()||segment.size()>255||segment=="."||segment==".."||segment.endsWith('.')||segment.endsWith(' '))return false;
    for(const auto c:segment)if(c.unicode()<32||c==QChar(127)||QStringLiteral("/\\:*?\"<>|").contains(c))return false;
    const QString stem=segment.section('.',0,0).toUpper();
    static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$"));
    return !reserved.match(stem).hasMatch();
}

bool AppResourceStore::configure(const QUrl& baseUrl,const QString& rootDirectory,QString* error) {
    m_root.clear();m_prefix.clear();m_baseUrl=QUrl();
    const auto fail=[&](const QString& message){if(error)*error=message;return false;};
    QStringList prefix;
    if(!baseUrl.isValid()||baseUrl.scheme()!="app"||!QStringList{"agents","plugins"}.contains(baseUrl.host())
        ||!baseUrl.userInfo().isEmpty()||baseUrl.port()!=-1||baseUrl.hasQuery()||baseUrl.hasFragment()
        ||!decodePath(baseUrl,&prefix)||prefix.size()!=2)return fail(QStringLiteral("Invalid app identity URL."));
    QString root;
#ifdef Q_OS_WIN
    FileHandle directory;
    const auto native=QDir::toNativeSeparators(QFileInfo(rootDirectory).absoluteFilePath()).toStdWString();
    directory.value=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directory.value!=INVALID_HANDLE_VALUE)root=handlePath(directory.value);
#else
    root=QFileInfo(rootDirectory).canonicalFilePath();
#endif
    if(root.isEmpty()||!QFileInfo(root).isDir())return fail(QStringLiteral("App root directory is unavailable."));
    m_root=root;m_baseUrl=baseUrl;m_prefix=prefix;if(error)error->clear();return true;
}

AppResourceResponse AppResourceStore::read(const QUrl& url,const QByteArray& method) const {
    if(!configured())return failure(503,"App is not configured");
    if(method!="GET"&&method!="HEAD")return failure(405,"App resources are read-only");
    if(!url.isValid()||url.scheme()!=m_baseUrl.scheme()||url.host()!=m_baseUrl.host()||url.port()!=-1||!url.userInfo().isEmpty())return failure(403,"App identity denied");
    QStringList parts;
    if(!decodePath(url,&parts)||parts.size()<3)return failure(400,"Invalid app resource path");
    if(parts.mid(0,2)!=m_prefix)return failure(403,"App identity denied");
    const QString relative=parts.mid(2).join('/');
    const QString path=QDir(m_root).filePath(relative);
    QByteArray bytes;qint64 length=0;
#ifdef Q_OS_WIN
    FileHandle file;
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    // Validate and read the same opened object. Junction/symlink replacement
    // after validation cannot redirect the subsequent read to another object.
    file.value=CreateFileW(native.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)return failure(404,"App resource not found");
    const QString resolved=handlePath(file.value);
    if(resolved.isEmpty()||!inside(m_root,resolved))return failure(403,"App resource outside selected root");
    BY_HANDLE_FILE_INFORMATION information{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.value,&information)||(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!GetFileSizeEx(file.value,&size))return failure(404,"App resource unavailable");
    length=size.QuadPart;
    if(length<0||length>maximumAssetBytes)return failure(413,"App resource exceeds size limit");
    if(method=="GET") {
        bytes.resize(static_cast<qsizetype>(length));DWORD received=0;
        if(length&&(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(length),&received,nullptr)||received!=length))return failure(500,"App resource changed or could not be read");
    }
#else
    const QString resolved=QFileInfo(path).canonicalFilePath();
    if(resolved.isEmpty())return failure(404,"App resource not found");
    if(!inside(m_root,resolved))return failure(403,"App resource outside selected root");
    QFile file(resolved);if(!file.open(QIODevice::ReadOnly))return failure(404,"App resource unavailable");
    length=file.size();if(length<0||length>maximumAssetBytes)return failure(413,"App resource exceeds size limit");
    if(method=="GET"){bytes=file.read(maximumAssetBytes+1);if(bytes.size()!=length)return failure(500,"App resource changed or could not be read");}
#endif
    QByteArray mime=QMimeDatabase().mimeTypeForFile(relative,QMimeDatabase::MatchExtension).name().toUtf8();
    if(mime.startsWith("text/")||mime=="application/javascript"||mime=="application/json")mime+="; charset=utf-8";
    return {200,bytes,mime,{{"Content-Length",QByteArray::number(length)},{"Cache-Control","no-store"},{"Referrer-Policy","same-origin"},{"X-Content-Type-Options","nosniff"}}};
}
}
