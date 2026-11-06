// This test compiles the actual production implementation once, with Win32
// boundary wrappers local to this translation unit. Production has no hook.
#include "core/ToolExecutor.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QThreadPool>
#include <QTemporaryDir>
#include <atomic>
#include <new>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
namespace {
QString hookAlias,hookOutside,hookFailure;
HANDLE hookHandle=INVALID_HANDLE_VALUE;
bool hookArmed=false,hookSwapped=false;
QString holdPath;
HANDLE holdHandle=INVALID_HANDLE_VALUE;
QSemaphore holdEntered,holdRelease;
std::atomic_bool holdArmed{false},holdTimedOut{false};
bool makeJunction(const QString& path,const QString& target){
    QProcess process;process.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(target)});
    return process.waitForFinished(5000)&&process.exitCode()==0;
}
HANDLE WINAPI testCreateFileW(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE model){
    const auto handle=::CreateFileW(name,access,share,security,disposition,flags,model);
    if(hookArmed&&access==FILE_LIST_DIRECTORY&&QDir::fromNativeSeparators(QString::fromWCharArray(name)).compare(hookAlias,Qt::CaseInsensitive)==0)hookHandle=handle;
    if(holdArmed.load()&&access==FILE_LIST_DIRECTORY&&QDir::fromNativeSeparators(QString::fromWCharArray(name)).compare(holdPath,Qt::CaseInsensitive)==0)holdHandle=handle;
    return handle;
}
DWORD WINAPI testGetFinalPathNameByHandleW(HANDLE handle,LPWSTR path,DWORD capacity,DWORD flags){
    const auto length=::GetFinalPathNameByHandleW(handle,path,capacity,flags);const auto lastError=GetLastError();
    if(holdArmed.load()&&handle==holdHandle&&path&&length>0&&length<capacity){
        holdArmed.store(false);holdEntered.release();
        if(!holdRelease.tryAcquire(1,5000))holdTimedOut.store(true);
    }
    if(hookArmed&&!hookSwapped&&handle==hookHandle&&path&&length>0&&length<capacity){
        hookArmed=false;
        // The buffer already contains the original validated canonical target.
        // Retarget only the fixture alias while that target handle stays open.
        if(!QDir().rmdir(hookAlias))hookFailure="Could not remove the controlled junction while its target was pinned.";
        else if(!makeJunction(hookAlias,hookOutside))hookFailure="Could not retarget the controlled junction.";
        else hookSwapped=true;
    }
    SetLastError(lastError);return length;
}
}
#define CreateFileW testCreateFileW
#define GetFinalPathNameByHandleW testGetFinalPathNameByHandleW
#endif
#ifndef ADE_TOOL_EXECUTOR_SOURCE
#define ADE_TOOL_EXECUTOR_SOURCE "../src/core/ToolExecutor.cpp"
#endif
#ifndef ADE_SCOPED_DIRECTORY_SOURCE
#define ADE_SCOPED_DIRECTORY_SOURCE "../src/core/ScopedDirectory.cpp"
#endif
#include ADE_SCOPED_DIRECTORY_SOURCE
#include ADE_TOOL_EXECUTOR_SOURCE
#ifdef Q_OS_WIN
#undef CreateFileW
#undef GetFinalPathNameByHandleW
#endif

using namespace ade;
class FileToolRaceTests final:public QObject {
    Q_OBJECT
private slots:
    void aliasRetargetAfterValidationNeverExposesOtherDirectory_data(){
        QTest::addColumn<QString>("operation");QTest::addColumn<QJsonObject>("arguments");
        QTest::newRow("shallow-list")<<QString("ls")<<QJsonObject{{"path","alias"},{"depth",0}};
        QTest::newRow("recursive-list")<<QString("ls")<<QJsonObject{{"path","alias"},{"depth",2}};
        QTest::newRow("directory-read")<<QString("read")<<QJsonObject{{"path","alias"}};
        QTest::newRow("directory-preview")<<QString("read")<<QJsonObject{{"path","alias"},{"preview",true}};
    }
    void aliasRetargetAfterValidationNeverExposesOtherDirectory(){
#ifdef Q_OS_WIN
        QFETCH(QString,operation);QFETCH(QJsonObject,arguments);
        QTemporaryDir fixture;const auto workspace=fixture.filePath("workspace"),original=workspace+"/real",outside=fixture.filePath("outside");
        QVERIFY(QDir().mkpath(original+"/child"));QVERIFY(QDir().mkpath(outside+"/child"));
        const auto put=[](const QString& path){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write("fixture")==7;};
        QVERIFY(put(original+"/OWNED_ROOT_MARKER.txt"));QVERIFY(put(original+"/child/OWNED_CHILD_MARKER.txt"));
        QVERIFY(put(outside+"/OUTSIDE_ROOT_MARKER.txt"));QVERIFY(put(outside+"/child/OUTSIDE_CHILD_MARKER.txt"));
        hookAlias=workspace+"/alias";hookOutside=outside;hookFailure.clear();hookHandle=INVALID_HANDLE_VALUE;hookSwapped=false;hookArmed=false;
        QVERIFY(makeJunction(hookAlias,original));const auto cleanup=qScopeGuard([&]{hookArmed=false;QDir().rmdir(hookAlias);});
        ToolExecutor tools;QVERIFY(tools.setWorkspace(workspace));QSignalSpy completed(&tools,&ToolExecutor::completed);
        hookArmed=true;tools.execute("alias-swap",operation,arguments);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,10000);
        QVERIFY2(hookFailure.isEmpty(),qPrintable(hookFailure));QVERIFY2(hookSwapped,"The deterministic retarget must execute at the production validation boundary.");
        const auto output=completed.first()[1].toString();QVERIFY2(!completed.first()[2].toBool(),qPrintable(output));
        QVERIFY2(!output.contains("OUTSIDE_ROOT_MARKER")&&!output.contains("OUTSIDE_CHILD_MARKER"),qPrintable(output));
        QVERIFY2(output.contains("OWNED_ROOT_MARKER"),qPrintable(output));
        if(arguments["depth"].toInt()>0||(operation=="read"&&!arguments["preview"].toBool()))QVERIFY2(output.contains("OWNED_CHILD_MARKER"),qPrintable(output));
#else
        QSKIP("Windows same-handle directory enumeration regression.");
#endif
    }
    void heldWorkerCancellationAndSameIdReplacement_data(){
        QTest::addColumn<bool>("replacement");
        QTest::newRow("cancel-only")<<false;
        QTest::newRow("same-id-replacement")<<true;
    }
    void heldWorkerCancellationAndSameIdReplacement(){
#ifdef Q_OS_WIN
        QFETCH(bool,replacement);QTemporaryDir fixture;QVERIFY(QDir().mkpath(fixture.filePath("held")));
        QFile file(fixture.filePath("replacement.txt"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("REPLACEMENT_RESULT");file.close();
        const auto oldThreads=QThreadPool::globalInstance()->maxThreadCount();QThreadPool::globalInstance()->setMaxThreadCount(qMax(2,oldThreads));
        const auto cleanup=qScopeGuard([&]{holdArmed.store(false);holdRelease.release();QThreadPool::globalInstance()->waitForDone(6000);while(holdRelease.tryAcquire()){}while(holdEntered.tryAcquire()){}QThreadPool::globalInstance()->setMaxThreadCount(oldThreads);});
        holdPath=fixture.filePath("held");holdHandle=INVALID_HANDLE_VALUE;holdTimedOut.store(false);holdArmed.store(true);
        ToolExecutor tools;QVERIFY(tools.setWorkspace(fixture.path()));QSignalSpy completed(&tools,&ToolExecutor::completed);
        tools.execute("reused","ls",{{"path","held"}});
        QTRY_VERIFY_WITH_TIMEOUT(holdEntered.available()>0,3000);holdEntered.acquire();
        tools.cancel();QCOMPARE(completed.count(),1);QVERIFY(completed.first()[2].toBool());
        if(replacement){
            tools.execute("reused","read",{{"path","replacement.txt"}});
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(),2,3000);QVERIFY(!completed.last()[2].toBool());QVERIFY(completed.last()[1].toString().contains("REPLACEMENT_RESULT"));
        }
        holdRelease.release();QVERIFY(QThreadPool::globalInstance()->waitForDone(3000));QCoreApplication::processEvents();
        QVERIFY(!holdTimedOut.load());QCOMPARE(completed.count(),replacement?2:1);
#else
        QSKIP("Windows held-directory worker fixture.");
#endif
    }
    void destroyedOwnerStorageCanBeProtectedWhileWorkerCompletes(){
#ifdef Q_OS_WIN
        QTemporaryDir fixture;QVERIFY(QDir().mkpath(fixture.filePath("held")));
        SYSTEM_INFO info{};GetSystemInfo(&info);QVERIFY(sizeof(ToolExecutor)<=info.dwPageSize);
        void* storage=VirtualAlloc(nullptr,info.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);QVERIFY(storage);
        auto* tools=new(storage)ToolExecutor;bool destroyed=false;int completions=0;
        const auto cleanup=qScopeGuard([&]{
            holdArmed.store(false);holdRelease.release();QThreadPool::globalInstance()->waitForDone(6000);
            if(!destroyed)tools->~ToolExecutor();
            VirtualFree(storage,0,MEM_RELEASE);
            while(holdRelease.tryAcquire()){}while(holdEntered.tryAcquire()){}
        });
        QPointer<ToolExecutor> alive(tools);QVERIFY(tools->setWorkspace(fixture.path()));
        connect(tools,&ToolExecutor::completed,this,[&]{++completions;});
        holdPath=fixture.filePath("held");holdHandle=INVALID_HANDLE_VALUE;holdTimedOut.store(false);holdArmed.store(true);
        tools->execute("destroyed","ls",{{"path","held"}});
        QTRY_VERIFY_WITH_TIMEOUT(holdEntered.available()>0,3000);holdEntered.acquire();
        QElapsedTimer elapsed;elapsed.start();tools->~ToolExecutor();destroyed=true;QVERIFY(!alive);QVERIFY(elapsed.elapsed()<1000);
        DWORD previous=0;QVERIFY(VirtualProtect(storage,info.dwPageSize,PAGE_NOACCESS,&previous));
        holdRelease.release();QVERIFY(QThreadPool::globalInstance()->waitForDone(3000));QCoreApplication::processEvents();
        QVERIFY(!holdTimedOut.load());QCOMPARE(completions,0);
#else
        QSKIP("Windows protected-storage worker fixture.");
#endif
    }
};
QTEST_GUILESS_MAIN(FileToolRaceTests)
#include "test_file_tools_races.moc"
