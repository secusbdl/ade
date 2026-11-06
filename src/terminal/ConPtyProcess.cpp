#include "ConPtyProcess.h"
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QMetaObject>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ade {
namespace {
QString winError(const QString &operation, DWORD code=GetLastError()) {
    wchar_t *message=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,reinterpret_cast<wchar_t*>(&message),0,nullptr);
    const QString detail=message?QString::fromWCharArray(message).trimmed():QString::number(code);
    if(message)LocalFree(message);
    return operation+": "+detail+" ("+QString::number(code)+")";
}
QString quote(const QString &arg) {
    QString out="\""; int slashes=0;
    for(const QChar c:arg) {
        if(c=='\\'){++slashes;continue;}
        if(c=='\"')out+=QString(slashes*2+1,'\\'); else out+=QString(slashes,'\\');
        slashes=0;out+=c;
    }
    return out+QString(slashes*2,'\\')+'\"';
}
void closeHandle(HANDLE &handle){if(handle&&handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);handle=nullptr;}
}
struct ConPtyProcess::Impl {
    ConPtyProcess *owner;
    HANDLE input=nullptr,output=nullptr,process=nullptr,job=nullptr;
    HPCON console=nullptr;
    std::atomic<bool> running{false},closing{false};
    std::atomic<DWORD> writerId{0};
    std::atomic<DWORD> readerId{0};
    std::atomic<bool> writerDone{true},readerDone{true},abortRead{false};
    quint64 generation=0;
    quint32 pid=0; QString cwd;
    std::thread reader,writer,monitor;
    std::mutex inputMutex,outputMutex,consoleMutex;
    std::condition_variable inputWake,outputWake;
    QByteArray inputBytes,outputBytes;
    bool outputScheduled=false;
    static constexpr qsizetype QueueLimit=4*1024*1024;
    explicit Impl(ConPtyProcess *p):owner(p){}
    void drain() {
        QByteArray bytes;
        {std::lock_guard lock(outputMutex);bytes.swap(outputBytes);outputScheduled=false;}
        outputWake.notify_all();
        if(!bytes.isEmpty())emit owner->outputReady(bytes);
    }
    void readLoop() {
        readerId=GetCurrentThreadId();
        const auto currentGeneration=generation;
        char buffer[16384];DWORD count=0;
        while(!abortRead.load()&&ReadFile(output,buffer,sizeof(buffer),&count,nullptr)&&count) {
            std::unique_lock lock(outputMutex);
            outputWake.wait(lock,[&]{return outputBytes.size()<QueueLimit||closing.load();});
            // During forced teardown continue draining ConPTY to avoid an OS
            // pipe deadlock; normal operation applies bounded backpressure.
            if(outputBytes.size()>=QueueLimit)continue;
            outputBytes.append(buffer,count);
            if(!outputScheduled){outputScheduled=true;QMetaObject::invokeMethod(owner,[this,currentGeneration]{if(generation==currentGeneration)drain();},Qt::QueuedConnection);}
        }
        readerDone=true;
    }
    void writeLoop() {
        writerId=GetCurrentThreadId();
        struct Done {std::atomic<bool>& flag;~Done(){flag=true;}} done{writerDone};
        for(;;) {
            QByteArray bytes;
            {std::unique_lock lock(inputMutex);inputWake.wait(lock,[&]{return closing.load()||!inputBytes.isEmpty();});
                if(closing.load())return;
                bytes.swap(inputBytes);}
            qsizetype offset=0;
            while(offset<bytes.size()&&!closing.load()) {
                DWORD written=0;
                if(!WriteFile(input,bytes.constData()+offset,DWORD(qMin<qsizetype>(16384,bytes.size()-offset)),&written,nullptr)||!written)return;
                offset+=written;
            }
        }
    }
    static void cancelAndJoin(std::thread &thread,std::atomic<DWORD> &id,std::atomic<bool> &done){
        // A one-shot cancellation races a writer entering synchronous I/O.
        // Repeat until its loop observes closing, then join the actual thread.
        while(!done.load()){
            HANDLE handle=OpenThread(THREAD_TERMINATE,FALSE,id.load());
            if(handle){CancelSynchronousIo(handle);CloseHandle(handle);}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(thread.joinable())thread.join();
    }
    void monitorLoop() {
        const auto currentGeneration=generation;
        WaitForSingleObject(process,INFINITE);
        DWORD code=0;GetExitCodeProcess(process,&code);
        running=false;closing=true;inputWake.notify_all();outputWake.notify_all();
        // Descendants remain in our job even if the initial shell exited.
        TerminateJobObject(job,code);
        if(writer.joinable())cancelAndJoin(writer,writerId,writerDone);
        closeHandle(input);
        {std::lock_guard lock(consoleMutex);if(console){ClosePseudoConsole(console);console=nullptr;}}
        // Windows 11 24H2 closes asynchronously. Drain its final frame before
        // cancellation; older OS releases finish draining inside Close above.
        for(int i=0;i<1000&&!readerDone.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        abortRead=true;
        if(reader.joinable())cancelAndJoin(reader,readerId,readerDone);
        closeHandle(output);
        QMetaObject::invokeMethod(owner,[this,code,currentGeneration]{if(generation==currentGeneration){drain();emit owner->exited(code);}},Qt::QueuedConnection);
    }
    void release() {
        if(monitor.joinable())monitor.join();
        closeHandle(process);closeHandle(job);closeHandle(input);closeHandle(output);
        if(console){ClosePseudoConsole(console);console=nullptr;}
    }
};

ConPtyProcess::ConPtyProcess(QObject *parent):QObject(parent),d(std::make_unique<Impl>(this)){}
ConPtyProcess::~ConPtyProcess(){stop();}
QStringList ConPtyProcess::defaultShell(){
    auto executable=QStandardPaths::findExecutable("pwsh.exe");
    if(executable.isEmpty())executable=QStandardPaths::findExecutable("powershell.exe");
    if(!executable.isEmpty()){
        const QString integration=QStringLiteral(
            "if ($ExecutionContext.SessionState.LanguageMode -eq 'FullLanguage') { "
            "$global:ADETerminalOriginalPrompt=$function:prompt; "
            "function global:prompt { $result=& $global:ADETerminalOriginalPrompt; "
            "try { [Console]::Write([string][char]27+']7;'+([Uri]::new((Get-Location).ProviderPath)).AbsoluteUri+[char]7) } catch {}; $result } }");
        return {executable,"-NoLogo","-NoExit","-Command",integration};
    }
    return {qEnvironmentVariable("ComSpec",QDir::toNativeSeparators(qEnvironmentVariable("SystemRoot")+"/System32/cmd.exe")),"/Q"};
}
bool ConPtyProcess::start(const QString &workingDirectory,const QStringList &requested,QSize cells,QString *error){
    auto fail=[&](const QString &message){if(error)*error=message;emit errorOccurred(message);return false;};
    if(d->running)return fail("The terminal is already running.");
    stop();
    if(cells.width()<2||cells.height()<1||cells.width()>500||cells.height()>300)return fail("Invalid terminal dimensions.");
    QStringList command=requested.isEmpty()?defaultShell():requested;
    if(command.isEmpty()||command.first().isEmpty())return fail("A shell executable is required.");
    QString executable=command.first();
    if(!QFileInfo(executable).isAbsolute())executable=QStandardPaths::findExecutable(executable);
    if(executable.isEmpty()||!QFileInfo(executable).isFile())return fail("Terminal executable was not found.");
    d->cwd=QFileInfo(workingDirectory).isDir()?QFileInfo(workingDirectory).absoluteFilePath():QDir::homePath();
    HANDLE inputRead=nullptr,outputWrite=nullptr;
    if(!CreatePipe(&inputRead,&d->input,nullptr,0)||!CreatePipe(&d->output,&outputWrite,nullptr,0)){
        const auto message=winError("Create terminal pipes");closeHandle(inputRead);closeHandle(outputWrite);d->release();return fail(message);}
    HRESULT result=CreatePseudoConsole({SHORT(cells.width()),SHORT(cells.height())},inputRead,outputWrite,0,&d->console);
    closeHandle(inputRead);closeHandle(outputWrite);
    if(FAILED(result)){d->release();return fail(winError("Create pseudoconsole",DWORD(result)));}
    d->job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!d->job||!SetInformationJobObject(d->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){const auto message=winError("Create terminal job");d->release();return fail(message);}
    SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<unsigned char> storage(size);
    auto *attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&size)){const auto message=winError("Initialize process attributes");d->release();return fail(message);}
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,d->console,sizeof(HPCON),nullptr,nullptr)){
        const auto message=winError("Attach pseudoconsole");DeleteProcThreadAttributeList(attributes);d->release();return fail(message);}
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=attributes;
    // Null standard handles let ConPTY populate its own console endpoints.
    // Otherwise redirected parent handles can bypass the pseudoconsole.
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    QString commandLine=quote(QDir::toNativeSeparators(executable));for(qsizetype i=1;i<command.size();++i)commandLine+=' '+quote(command[i]);
    std::wstring mutableCommand=commandLine.toStdWString();
    auto environment=QProcessEnvironment::systemEnvironment();environment.insert("TERM","xterm-256color");environment.insert("COLORTERM","truecolor");
    auto entries=environment.toStringList();entries.sort(Qt::CaseInsensitive);QString block;for(const auto &entry:entries){block+=entry;block+=QChar(0);}block+=QChar(0);
    PROCESS_INFORMATION process{};
    const bool created=CreateProcessW(reinterpret_cast<LPCWSTR>(executable.utf16()),mutableCommand.data(),nullptr,nullptr,FALSE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_SUSPENDED,
        const_cast<ushort*>(block.utf16()),reinterpret_cast<LPCWSTR>(d->cwd.utf16()),&startup.StartupInfo,&process);
    const DWORD createError=GetLastError();DeleteProcThreadAttributeList(attributes);
    if(!created){d->release();return fail(winError("Start terminal process",createError));}
    d->process=process.hProcess;d->pid=process.dwProcessId;
    if(!AssignProcessToJobObject(d->job,process.hProcess)){
        const auto message=winError("Isolate terminal process tree");TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);d->release();return fail(message);}
    {std::lock_guard lock(d->inputMutex);d->inputBytes.clear();}
    {std::lock_guard lock(d->outputMutex);d->outputBytes.clear();d->outputScheduled=false;}
    ++d->generation;d->writerId=0;d->readerId=0;d->closing=false;d->running=true;d->abortRead=false;
    try{
        d->readerDone=false;d->reader=std::thread([this]{d->readLoop();});
        d->writerDone=false;d->writer=std::thread([this]{d->writeLoop();});
        d->monitor=std::thread([this]{d->monitorLoop();});
    }catch(const std::exception &exception){
        TerminateJobObject(d->job,1);CloseHandle(process.hThread);
        if(!d->reader.joinable())d->readerDone=true;
        if(!d->writer.joinable())d->writerDone=true;
        d->monitorLoop();d->release();++d->generation;
        return fail("Could not start terminal I/O threads: "+QString::fromUtf8(exception.what()));
    }
    const DWORD resumed=ResumeThread(process.hThread),resumeError=GetLastError();CloseHandle(process.hThread);
    if(resumed==DWORD(-1)){stop();return fail(winError("Resume terminal process",resumeError));}
    return true;
}
bool ConPtyProcess::write(const QByteArray &bytes){
    if(!d->running||bytes.isEmpty())return false;
    {std::lock_guard lock(d->inputMutex);if(d->closing||d->inputBytes.size()+bytes.size()>Impl::QueueLimit)return false;d->inputBytes+=bytes;}
    d->inputWake.notify_one();return true;
}
bool ConPtyProcess::resize(QSize cells,QString *error){
    if(cells.width()<2||cells.height()<1||cells.width()>500||cells.height()>300){if(error)*error="Invalid terminal dimensions.";return false;}
    std::lock_guard lock(d->consoleMutex);if(!d->console)return false;
    const auto result=ResizePseudoConsole(d->console,{SHORT(cells.width()),SHORT(cells.height())});
    if(FAILED(result)){if(error)*error=winError("Resize pseudoconsole",DWORD(result));return false;}return true;
}
void ConPtyProcess::stop(){
    d->closing=true;d->inputWake.notify_all();d->outputWake.notify_all();
    if(d->job)TerminateJobObject(d->job,1);
    d->release();d->running=false;
}
bool ConPtyProcess::isRunning()const{return d->running;}
quint32 ConPtyProcess::processId()const{return d->pid;}
QString ConPtyProcess::workingDirectory()const{return d->cwd;}
}
