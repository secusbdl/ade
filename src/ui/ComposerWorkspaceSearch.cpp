#include "ComposerWorkspaceSearch.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif
namespace ade {
namespace {
#ifdef Q_OS_WIN
struct Pins {
    std::vector<HANDLE> values;
    ~Pins(){for(auto it=values.rbegin();it!=values.rend();++it)CloseHandle(*it);}
};
QString finalPath(HANDLE handle){
    const auto needed=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!needed||needed>32768)return {};
    std::vector<wchar_t> buffer(needed+1);const auto count=GetFinalPathNameByHandleW(handle,buffer.data(),buffer.size(),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!count||count>=buffer.size())return {};
    auto value=QString::fromWCharArray(buffer.data(),count);if(value.startsWith("\\\\?\\"))value=value.mid(4);return QDir::cleanPath(QDir::fromNativeSeparators(value));
}
QString identity(HANDLE handle){BY_HANDLE_FILE_INFORMATION data{};if(!GetFileInformationByHandle(handle,&data))return {};return QString::number(data.dwVolumeSerialNumber,16)+':'+QString::number(data.nFileIndexHigh,16)+':'+QString::number(data.nFileIndexLow,16);}
HANDLE openPinned(const QString &path,bool directory,const std::shared_ptr<Pins> &pins){
    // Attribute-only file handles do not enforce the desired delete-sharing
    // exclusion on Windows. Request data/list access without reading contents.
    const auto native=QDir::toNativeSeparators(path);const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()),FILE_READ_ATTRIBUTES|FILE_READ_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),nullptr);
    if(handle==INVALID_HANDLE_VALUE)return handle;
    BY_HANDLE_FILE_INFORMATION data{};const bool valid=GetFileInformationByHandle(handle,&data)&&!(data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory&&finalPath(handle).compare(QDir::cleanPath(path),Qt::CaseInsensitive)==0;
    if(!valid){CloseHandle(handle);return INVALID_HANDLE_VALUE;}pins->values.push_back(handle);return handle;
}
bool localAbsolute(const QString &path){return QRegularExpression("^[A-Za-z]:/").match(path).hasMatch()&&!path.contains("/../")&&!path.contains("/./");}
std::shared_ptr<Pins> pinRoot(const QString &root,QString *id){
    if(!localAbsolute(root))return {};
    auto pins=std::make_shared<Pins>();QString current=root.left(3);HANDLE last=openPinned(current,true,pins);if(last==INVALID_HANDLE_VALUE)return {};
    for(const auto &part:root.mid(3).split('/',Qt::SkipEmptyParts)){current=QDir(current).filePath(part);last=openPinned(current,true,pins);if(last==INVALID_HANDLE_VALUE)return {};}
    *id=identity(last);return id->isEmpty()?std::shared_ptr<Pins>{}:pins;
}
#endif
int score(const QString &relative,const QString &query){
    if(query.isEmpty())return relative.contains('/')?20:30;
    const auto text=relative.toCaseFolded(),needle=query.toCaseFolded();const auto file=text.section('/',-1);
    if(file==needle)return 1000;
    if(file.startsWith(needle))return 800;
    if(file.contains(needle))return 600;
    if(text.contains(needle))return 400;
    int position=0;for(const auto c:needle){position=text.indexOf(c,position);if(position<0)return 0;++position;}return 100;
}
}
ComposerPathPin ComposerWorkspaceSearch::pin(const QString &rawRoot,const QString &relative,const QString &expectedRootIdentity,QString *error){
    if(error)error->clear();
    ComposerPathPin result;
#ifdef Q_OS_WIN
    const auto root=QDir::cleanPath(QDir::fromNativeSeparators(rawRoot));
    if(relative.contains('\\')||relative.contains(':')||relative.startsWith('/')||relative.split('/').contains("..")||relative.split('/').contains(".")){if(error)*error="Invalid workspace completion path.";return {};}
    QString rootId;auto pins=pinRoot(root,&rootId);
    if(!pins||(!expectedRootIdentity.isEmpty()&&rootId!=expectedRootIdentity)){if(error)*error="The workspace changed since this result was found.";return {};}
    auto components=relative.split('/',Qt::SkipEmptyParts);QString current=root;
    for(int i=0;i<components.size();++i){current=QDir(current).filePath(components[i]);const auto native=QDir::toNativeSeparators(current);const auto attrs=GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));if(attrs==INVALID_FILE_ATTRIBUTES||attrs&FILE_ATTRIBUTE_REPARSE_POINT){if(error)*error="The selected entry is unavailable or links outside this search.";return {};}
        const bool dir=attrs&FILE_ATTRIBUTE_DIRECTORY;if(i+1<components.size()&&!dir)return {};
        if(openPinned(current,dir,pins)==INVALID_HANDLE_VALUE){if(error)*error="Cannot pin the selected workspace entry.";return {};}result.directory=dir;
    }
    if(components.isEmpty())result.directory=true;
    result.absolutePath=current;result.rootIdentity=rootId;result.handles=pins;
#else
    Q_UNUSED(rawRoot);Q_UNUSED(relative);Q_UNUSED(expectedRootIdentity);if(error)*error="Workspace completion currently requires Windows.";
#endif
    return result;
}
ComposerSearchResult ComposerWorkspaceSearch::search(const QString &rawRoot,const QString &query,const std::shared_ptr<std::atomic_bool> &cancelled){
    ComposerSearchResult result;result.root=QDir::cleanPath(QDir::fromNativeSeparators(rawRoot));
#ifdef Q_OS_WIN
    QString rootId;auto rootPins=pinRoot(result.root,&rootId);if(!rootPins){result.error="Workspace file search requires an accessible local directory without reparse aliases.";return result;}result.rootIdentity=rootId;
    const QSet<QString> omitted{".git",".hg",".svn","node_modules",".next",".cache","__pycache__","build","dist","target"};
    struct Match{int score;QString relative;bool directory;};QList<Match> matches;
    struct Directory{QString relative;int depth;};QList<Directory> pending{{{},0}};QElapsedTimer time;time.start();int scanned=0;
    while(!pending.isEmpty()){
        if((cancelled&&cancelled->load())||scanned>=4096||time.elapsed()>750){result.limited=true;break;}
        const auto directory=pending.takeFirst();auto scope=pin(result.root,directory.relative,rootId);if(!scope)continue;
        const auto pins=std::static_pointer_cast<Pins>(scope.handles);const auto handle=pins->values.back();
        QByteArray buffer(64*1024,Qt::Uninitialized);bool first=true;
        while(scanned<4096&&time.elapsed()<=750&&!(cancelled&&cancelled->load())){
            const auto kind=first?FileIdBothDirectoryRestartInfo:FileIdBothDirectoryInfo;first=false;
            if(!GetFileInformationByHandleEx(handle,kind,buffer.data(),buffer.size()))break;
            qsizetype offset=0;
            while(offset+qsizetype(offsetof(FILE_ID_BOTH_DIR_INFO,FileName))<=buffer.size()){
                const auto *entry=reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.constData()+offset);const auto span=entry->NextEntryOffset?qsizetype(entry->NextEntryOffset):buffer.size()-offset;
                if(span<qsizetype(offsetof(FILE_ID_BOTH_DIR_INFO,FileName))||offset+span>buffer.size()||entry->FileNameLength%2||entry->FileNameLength>span-qsizetype(offsetof(FILE_ID_BOTH_DIR_INFO,FileName)))break;
                const auto name=QString::fromWCharArray(entry->FileName,entry->FileNameLength/2);
                if(name!="."&&name!=".."){
                    ++scanned;const bool dir=entry->FileAttributes&FILE_ATTRIBUTE_DIRECTORY;
                    if(!(entry->FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&!(dir&&omitted.contains(name.toLower()))){
                        const auto relative=directory.relative.isEmpty()?name:directory.relative+'/'+name;const int rank=score(relative,query);
                        if(rank>0)matches.append({rank,relative,dir});
                        if(dir&&directory.depth<7&&pending.size()<512)pending.append({relative,directory.depth+1});
                    }
                }
                if(scanned>=4096||!entry->NextEntryOffset)break;
                offset+=entry->NextEntryOffset;
            }
        }
    }
    if(cancelled&&cancelled->load())return {};
    result.limited=result.limited||scanned>=4096||time.elapsed()>750;
    std::sort(matches.begin(),matches.end(),[](const Match &a,const Match &b){return a.score!=b.score?a.score>b.score:a.relative.compare(b.relative,Qt::CaseInsensitive)<0;});
    for(const auto &match:matches){if(result.items.size()>=64)break;const auto mounted="ws1/"+match.relative+(match.directory?"/":"");QUrl url;url.setScheme("path");url.setPath(mounted);
        result.items.append(QJsonObject{{"id","file:"+mounted},{"kind",match.directory?"directory":"file"},{"label",match.relative.section('/',-1)+(match.directory?"/":"")},{"description",match.relative},{"group","Files"},{"insertText","[]("+url.toString(QUrl::FullyEncoded)+")"},{"relativePath",match.relative},{"workspace",result.root},{"workspaceIdentity",rootId},{"mountedPath",mounted}});
    }
#else
    Q_UNUSED(query);Q_UNUSED(cancelled);result.error="Workspace completion currently requires Windows.";
#endif
    return result;
}
}
