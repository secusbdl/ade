#include "CommandFileSearch.h"
#include "ComposerWorkspaceSearch.h"
#include "CommandCenter.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
namespace ade {
namespace {
bool stopped(const std::shared_ptr<std::atomic_bool>& value){return value&&value->load();}
QString decode(const QByteArray& bytes){
    QStringDecoder decoder(QStringDecoder::Utf8);QByteArray data=bytes;
    if(bytes.startsWith("\xff\xfe")){decoder=QStringDecoder(QStringDecoder::Utf16LE);data=bytes.mid(2);}
    else if(bytes.startsWith("\xfe\xff")){decoder=QStringDecoder(QStringDecoder::Utf16BE);data=bytes.mid(2);}
    else if(bytes.contains('\0'))return {};
    const QString text=decoder(data);return decoder.hasError()?QString():text;
}
}
CommandFileResult CommandFileSearch::search(const CommandFileQuery& query,const std::shared_ptr<std::atomic_bool>& canceled){
    if(stopped(canceled))return {};
    CommandFileResult result;QString failure;const auto root=ComposerWorkspaceSearch::pin(query.root,{}, {},&failure);if(!root){result.error=failure;return result;}
    QElapsedTimer elapsed;elapsed.start();QProcess git;git.setWorkingDirectory(root.absolutePath);git.setProgram("git");
    QStringList args{"-c","core.quotepath=false","ls-files","-z","--cached","--others"};if(!query.includeIgnored)args<<"--exclude-standard";args<<"--";git.setArguments(args);git.start();
    QByteArray output;bool gitOk=git.waitForStarted(500);
    if(gitOk){while(git.state()!=QProcess::NotRunning){git.waitForFinished(20);output+=git.readAllStandardOutput();if(output.size()>8*1024*1024||elapsed.elapsed()>2000||stopped(canceled)){git.kill();git.waitForFinished(1000);result.limited=true;gitOk=false;break;}}output+=git.readAllStandardOutput();gitOk=gitOk&&git.exitStatus()==QProcess::NormalExit&&git.exitCode()==0;}
    if(stopped(canceled))return {};
    QStringList paths;QSet<QString> unique;bool fallback=false;
    if(gitOk){for(const auto& bytes:output.split('\0')){if(bytes.isEmpty())continue;QStringDecoder decoder(QStringDecoder::Utf8);const QString relative=decoder(bytes);if(decoder.hasError()||relative.startsWith('/')||relative.contains(':')||relative.contains('\\')||relative.split('/').contains(".."))continue;if(!unique.contains(relative)){unique.insert(relative);paths.append(relative);}if(paths.size()>=4096){result.limited=true;break;}}}
    else{
        const auto found=ComposerWorkspaceSearch::search(query.root,query.content?QString():query.query,canceled);result.limited=true;fallback=true;
        if(!found.error.isEmpty()){result.error=found.error;return result;}for(const auto& value:found.items){const auto path=value.toObject()["relativePath"].toString();if(!unique.contains(path)){paths.append(path);unique.insert(path);}}
    }
    // Directories are genuine navigation targets, derived only from enumerated entries.
    if(!query.content){const auto files=paths;for(const auto& path:files){auto parent=path.section('/',0,-2);while(!parent.isEmpty()){if(!unique.contains(parent)){unique.insert(parent);paths.append(parent);}const auto next=parent.section('/',0,-2);if(next==parent)break;parent=next;}}}
    QJsonArray matches;qint64 readBytes=0;int contentFiles=0;const auto needle=query.query.trimmed();
    for(const auto& path:paths){if(stopped(canceled))return {};if(elapsed.elapsed()>3500||matches.size()>=256){result.limited=true;break;}
        auto pin=ComposerWorkspaceSearch::pin(query.root,path,root.rootIdentity);if(!pin)continue;
        const QFileInfo info(pin.absolutePath);QJsonArray lines;QString description=path;int firstLine=0;
        if(query.content){if(pin.directory||needle.isEmpty())continue;if(contentFiles>=256||readBytes>=16*1024*1024){result.limited=true;break;}++contentFiles;QFile file(pin.absolutePath);if(!file.open(QIODevice::ReadOnly)||file.size()>512*1024)continue;const auto bytes=file.read(512*1024+1);if(bytes.size()>512*1024)continue;readBytes+=bytes.size();const auto text=decode(bytes);if(text.isEmpty())continue;
            int lineNumber=0,offset=0;while(offset<text.size()){++lineNumber;const auto end=text.indexOf('\n',offset);const auto line=text.mid(offset,end<0?-1:end-offset);if(line.contains(needle,Qt::CaseInsensitive)){if(!firstLine){firstLine=lineNumber;description=path+':'+QString::number(lineNumber)+" · "+line.trimmed().left(160);}lines.append(QJsonObject{{"line",lineNumber},{"text",line.left(512)}});if(lines.size()>=8)break;}if(end<0)break;offset=end+1;}if(lines.isEmpty())continue;
        }
        QJsonObject item{{"id","file:"+root.absolutePath+':'+path},{"kind","file"},{"title",info.fileName()+(pin.directory?"/":"")},{"subtitle",description},{"relativePath",path},{"workspace",root.absolutePath},{"workspaceIdentity",root.rootIdentity},{"directory",pin.directory},{"line",firstLine},{"contentMatches",lines},{"recency",double(info.lastModified().toMSecsSinceEpoch())},{"keywords",QJsonArray{path}}};
        matches.append(item);
    }
    if(query.content)result.items=matches;else result.items=CommandCenter::rank(matches,query.query,128);
    if(fallback)result.error="Non-Git workspace: bounded filename scan; ignored-file rules are unavailable.";
    if(query.content&&needle.isEmpty())result.error="Enter text to search file contents.";
    return result;
}
}
