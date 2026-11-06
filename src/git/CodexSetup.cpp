#include "ProcessRunner.h"
#include <QFile>
#include "vendor/tomlplusplus-3.4.0/toml.hpp"
namespace ade::gitdetail {
QString readCodexScript(const QString &path, QString *error) {
    if(error) error->clear();
    QFile file(path);
    if(!file.exists()) return {};
    if(!file.open(QIODevice::ReadOnly) || file.size()>1024*1024) { if(error) *error="Cannot read Codex setup configuration (maximum 1 MiB)."; return {}; }
    const auto bytes=file.readAll();
    try {
        const auto config=toml::parse(std::string_view(bytes.constData(),size_t(bytes.size())));
#ifdef Q_OS_WIN
        constexpr auto platform="win32";
#elif defined(Q_OS_MACOS)
        constexpr auto platform="darwin";
#else
        constexpr auto platform="linux";
#endif
        const auto specific=config["setup"][platform]["script"].value<std::string>();
        if(specific) { const auto text=QString::fromUtf8(specific->data(),qsizetype(specific->size())).trimmed(); if(!text.isEmpty()) return text; }
        const auto generic=config["setup"]["script"].value<std::string>();
        return generic ? QString::fromUtf8(generic->data(),qsizetype(generic->size())).trimmed() : QString();
    } catch(const toml::parse_error &) { if(error) *error="Invalid TOML in Codex setup configuration."; return {}; }
}
}
