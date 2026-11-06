#include "ProcessRunner.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <algorithm>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace ade::gitdetail {
QString canonical(const QString &path) {
    if (path.isEmpty() || path.contains(QChar::Null)) return {};
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    HANDLE handle=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE) return {};
    const DWORD size=GetFinalPathNameByHandleW(handle,nullptr,0,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!size) { CloseHandle(handle); return {}; }
    std::vector<wchar_t> buffer(size+1);
    const DWORD written=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    CloseHandle(handle);
    if(!written || written>=buffer.size()) return {};
    auto resolved=QString::fromWCharArray(buffer.data(),written);
    if(resolved.startsWith("\\\\?\\UNC\\")) resolved="\\\\"+resolved.mid(8);
    else if(resolved.startsWith("\\\\?\\")) resolved.remove(0,4);
    return QDir::cleanPath(QDir::fromNativeSeparators(resolved));
#else
    return QDir::fromNativeSeparators(QFileInfo(path).canonicalFilePath());
#endif
}
QString key(const QString &path) {
    QString result=QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    result=result.toCaseFolded();
#endif
    return result;
}
bool inside(const QString &parent, const QString &child, bool allowEqual) {
    if (parent.isEmpty() || child.isEmpty()) return false;
    const auto p=key(parent), c=key(child);
    return (allowEqual && p==c) || c.startsWith(p.endsWith('/') ? p : p+'/');
}
bool safeRelativePath(const QString &path) {
    if (path.isEmpty() || path.contains(QChar::Null) || QDir::isAbsolutePath(path) || path.contains('\\')) return false;
    for (const auto &part:path.split('/')) {
        if (part.isEmpty() || part=="." || part=="..") return false;
#ifdef Q_OS_WIN
        if (part.contains(':') || part.endsWith('.') || part.endsWith(' ')) return false;
        static const QRegularExpression reserved("^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",QRegularExpression::CaseInsensitiveOption);
        if (reserved.match(part).hasMatch()) return false;
#endif
    }
    return true;
}
QString safeChildPath(const QString &root, const QString &relative) {
    if (!safeRelativePath(relative)) return {};
    const QString resolvedRoot=canonical(root);
    if (resolvedRoot.isEmpty()) return {};
    const QString path=QDir(resolvedRoot).filePath(relative);
    QFileInfo ancestor(path);
    while (!ancestor.exists() && !ancestor.isSymLink() && !ancestor.isJunction()) {
        const QString parent=ancestor.absolutePath();
        if (parent==ancestor.absoluteFilePath()) return {};
        ancestor.setFile(parent);
    }
    const QString resolved=canonical(ancestor.absoluteFilePath());
    return inside(resolvedRoot,resolved,true) ? path : QString();
}
QString removeLineEnding(QString text) {
    if (text.endsWith('\n')) text.chop(1);
    if (text.endsWith('\r')) text.chop(1);
    return text;
}
QProcessEnvironment safeEnvironment() {
    auto env=QProcessEnvironment::systemEnvironment();
    for (const auto &name:env.keys()) {
        const auto upper=name.toUpper();
        // Never inherit app provider secrets or Git redirection/config injection.
        if (upper.contains("API_KEY") || upper.endsWith("_TOKEN") || upper.endsWith("_SECRET") || upper.endsWith("_PASSWORD")
            || upper.startsWith("ELECTRON_") || upper.startsWith("GIT_CONFIG_")
            || upper=="GIT_DIR" || upper=="GIT_WORK_TREE" || upper=="GIT_INDEX_FILE" || upper=="GIT_COMMON_DIR"
            || upper=="GIT_OBJECT_DIRECTORY" || upper=="GIT_ALTERNATE_OBJECT_DIRECTORIES"
            || upper=="GIT_TRACE" || upper.startsWith("GIT_TRACE_") || upper=="GIT_EXTERNAL_DIFF"
            || upper=="GIT_PAGER" || upper=="PAGER") env.remove(name);
    }
    env.insert("GIT_TERMINAL_PROMPT","0");
    env.insert("GCM_INTERACTIVE","Never");
    env.insert("GIT_OPTIONAL_LOCKS","0");
    env.insert("LC_ALL","C.UTF-8");
    return env;
}
ProcessResult runProcess(const QString &program, const QStringList &arguments, const QString &cwd,
                         const QProcessEnvironment &environment, const Cancellation &cancel,
                         int timeoutMs, qint64 outputLimit,
                         const std::function<void(const QByteArray &, const QByteArray &)> &progress, bool tailsOnly) {
    ProcessResult result;
    if (cancel && cancel->load()) { result.cancelled=true; result.error="Operation cancelled."; return result; }
    QProcess process;
    process.setProgram(program); process.setArguments(arguments); process.setWorkingDirectory(cwd);
    process.setProcessEnvironment(environment);
    process.setStandardInputFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    // Suspend before user hooks/helpers can execute, assign the job, then resume.
    // This avoids the process-tree escape race of assigning a running child.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags|=CREATE_SUSPENDED|CREATE_NO_WINDOW; });
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
        if(job) CloseHandle(job);
        result.error="Unable to create a child-process job."; return result;
    }
#endif
    QElapsedTimer timer; timer.start();
    process.start();
    if (!process.waitForStarted(std::max(1,std::min(timeoutMs,3000)))) {
        result.error=process.errorString();
        process.kill(); process.waitForFinished(1000);
#ifdef Q_OS_WIN
        CloseHandle(job);
#endif
        return result;
    }
#ifdef Q_OS_WIN
    const DWORD pid=DWORD(process.processId());
    HANDLE handle=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    bool attached=handle && AssignProcessToJobObject(job,handle);
    if(handle) CloseHandle(handle);
    bool resumed=false;
    if(attached) {
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if(snapshot!=INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{}; entry.dwSize=sizeof(entry);
            for(BOOL ok=Thread32First(snapshot,&entry);ok;ok=Thread32Next(snapshot,&entry)) {
                if(entry.th32OwnerProcessID!=pid) continue;
                HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME,FALSE,entry.th32ThreadID);
                if(thread) { resumed=ResumeThread(thread)!=DWORD(-1) || resumed; CloseHandle(thread); }
            }
            CloseHandle(snapshot);
        }
    }
    if(!attached || !resumed) {
        result.error="Unable to attach or resume the isolated child process.";
        CloseHandle(job); process.kill(); process.waitForFinished(1000); return result;
    }
#endif
    qint64 seen=0;
    auto drain=[&] {
        const auto out=process.readAllStandardOutput(), err=process.readAllStandardError();
        seen+=out.size()+err.size();
        if(progress && (!out.isEmpty() || !err.isEmpty())) progress(out,err);
        if(tailsOnly) { result.out=(result.out+out).right(outputLimit); result.err=(result.err+err).right(outputLimit); }
        else {
            if(seen>outputLimit) { result.limited=true; result.error="Process output exceeded the configured limit."; }
            else { result.out+=out; result.err+=err; }
        }
    };
    while(process.state()!=QProcess::NotRunning) {
        process.waitForFinished(25); drain();
        if(process.state()==QProcess::NotRunning) break;
        if(cancel && cancel->load()) { result.cancelled=true; result.error="Operation cancelled."; }
        if(timer.elapsed()>=timeoutMs) { result.timedOut=true; result.error="Process timed out."; }
        if(!result.error.isEmpty()) {
#ifdef Q_OS_WIN
            TerminateJobObject(job,1);
#endif
            process.kill(); process.waitForFinished(1000); break;
        }
    }
    drain(); result.exitCode=process.exitCode();
    if(process.exitStatus()==QProcess::CrashExit && result.error.isEmpty()) result.error="Process terminated abnormally.";
#ifdef Q_OS_WIN
    CloseHandle(job); // Also tear down any descendants left after their parent exited.
#endif
    return result;
}
}
