#pragma once
#include <QByteArray>
#include <QProcessEnvironment>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

namespace ade::gitdetail {
using Cancellation = std::shared_ptr<std::atomic_bool>;
struct ProcessResult { QByteArray out, err; int exitCode=-1; QString error; bool cancelled=false, timedOut=false, limited=false; bool ok() const { return error.isEmpty() && exitCode==0; } };
QProcessEnvironment safeEnvironment();
ProcessResult runProcess(const QString &program, const QStringList &arguments, const QString &cwd,
                         const QProcessEnvironment &environment, const Cancellation &cancel,
                         int timeoutMs, qint64 outputLimit,
                         const std::function<void(const QByteArray &, const QByteArray &)> &progress={}, bool tailsOnly=false);
QString canonical(const QString &path);
QString key(const QString &path);
bool inside(const QString &parent, const QString &child, bool allowEqual=false);
bool safeRelativePath(const QString &path);
QString safeChildPath(const QString &root, const QString &relative);
QString removeLineEnding(QString text);
QString readCodexScript(const QString &path, QString *error=nullptr);
}
