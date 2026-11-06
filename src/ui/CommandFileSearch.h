#pragma once
#include <QJsonArray>
#include <QString>
#include <atomic>
#include <memory>
namespace ade {
struct CommandFileQuery {QString root,query;bool content=false,includeIgnored=false;};
struct CommandFileResult {QJsonArray items;QString error;bool limited=false;};
class CommandFileSearch {
public:
    static CommandFileResult search(const CommandFileQuery& query,const std::shared_ptr<std::atomic_bool>& canceled);
};
}
