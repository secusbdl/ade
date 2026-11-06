#pragma once
#include <QJsonArray>
#include <QString>
#include <atomic>
#include <memory>
namespace ade {
struct ComposerSearchResult {
    QString root,rootIdentity,error;
    QJsonArray items;
    bool limited=false;
};
struct ComposerPathPin {
    QString absolutePath,rootIdentity;
    bool directory=false;
    std::shared_ptr<void> handles;
    explicit operator bool() const {return bool(handles);}
};
// Native Windows, handle-enumerated search. Immutable values only cross threads.
// No contents are read; reparse points and common generated trees are omitted.
class ComposerWorkspaceSearch {
public:
    static ComposerSearchResult search(const QString &canonicalRoot,const QString &query,
        const std::shared_ptr<std::atomic_bool> &cancelled);
    static ComposerPathPin pin(const QString &canonicalRoot,const QString &relative,
        const QString &expectedRootIdentity,QString *error=nullptr);
};
}
