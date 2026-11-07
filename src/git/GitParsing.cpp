#include "GitParsing.h"
#include <QHash>
namespace ade::gitdetail {
bool parseStatus(const QByteArray &bytes, GitStatus *status) {
    *status={};
    if(!bytes.isEmpty() && !bytes.endsWith('\0')) return false;
    const auto records=bytes.split('\0');
    for(qsizetype i=0;i<records.size()-1;++i) {
        const auto &record=records[i];
        if(record.size()<4 || record[2]!=' ') return false;
        const char x=record[0], y=record[1];
        if(x=='?' && y=='?') ++status->untrackedCount;
        else if(x!='!' || y!='!') {
            if(x!=' ') ++status->stagedCount;
            if(y!=' ') ++status->unstagedCount;
        }
        if(x=='R'||x=='C'||y=='R'||y=='C') { if(++i>=records.size()-1 || records[i].isEmpty()) return false; }
    }
    status->known=true;
    status->dirty=status->stagedCount || status->unstagedCount || status->untrackedCount;
    return true;
}
bool parseWorktrees(const QByteArray &bytes, QList<GitWorktree> *worktrees) {
    worktrees->clear();
    if(!bytes.isEmpty() && !bytes.endsWith('\0')) return false;
    GitWorktree current;
    auto flush=[&] { if(!current.path.isEmpty()) { current.main=worktrees->isEmpty(); worktrees->append(current); current={}; } };
    for(const auto &record:bytes.split('\0')) {
        if(record.isEmpty()) { flush(); continue; }
        if(record.startsWith("worktree ")) current.path=QString::fromUtf8(record.mid(9));
        else if(record.startsWith("HEAD ")) current.headSha=QString::fromLatin1(record.mid(5));
        else if(record.startsWith("branch refs/heads/")) current.branch=QString::fromUtf8(record.mid(18));
        else if(record=="detached") current.detached=true;
        else if(record.startsWith("locked")) current.locked=true;
        else if(record.startsWith("prunable")) current.prunable=true;
        else if(record=="bare") current.detached=true; // Administrative main entry; linked worktrees remain valid.
    }
    flush(); return !worktrees->isEmpty();
}
bool parseDiff(const QByteArray &numstat, const QByteArray &names, bool staged, QList<GitDiffEntry> *entries) {
    entries->clear();
    if((!numstat.isEmpty() && !numstat.endsWith('\0')) || (!names.isEmpty() && !names.endsWith('\0'))) return false;
    QHash<QString,GitDiffEntry> metadata;
    auto tokens=names.split('\0');
    for(qsizetype i=0;i<tokens.size()-1;) {
        const auto code=tokens[i++];
        if(code.isEmpty() || i>=tokens.size()-1) return false;
        GitDiffEntry entry; entry.status=code[0]; entry.staged=staged;
        if(entry.status=='R'||entry.status=='C') {
            entry.oldPath=QString::fromUtf8(tokens[i++]);
            if(i>=tokens.size()-1) return false;
        }
        entry.path=QString::fromUtf8(tokens[i++]); metadata.insert(entry.path,entry);
    }
    tokens=numstat.split('\0');
    for(qsizetype i=0;i<tokens.size()-1;) {
        const auto token=tokens[i++];
        const auto first=token.indexOf('\t'), second=token.indexOf('\t',first+1);
        if(first<0 || second<0) return false;
        auto path=QString::fromUtf8(token.mid(second+1)); QString old;
        if(path.isEmpty()) {
            if(i+1>=tokens.size()-1) return false;
            old=QString::fromUtf8(tokens[i++]); path=QString::fromUtf8(tokens[i++]);
        }
        auto entry=metadata.value(path); entry.path=path; entry.staged=staged;
        if(!old.isEmpty()) entry.oldPath=old;
        const auto added=token.left(first), deleted=token.mid(first+1,second-first-1);
        entry.binary=added=="-" || deleted=="-";
        bool a=true,b=true;
        if(!entry.binary) { entry.added=added.toLongLong(&a); entry.deleted=deleted.toLongLong(&b); }
        if(!a || !b || entry.added<0 || entry.deleted<0) return false;
        entries->append(entry);
    }
    return true;
}
}
