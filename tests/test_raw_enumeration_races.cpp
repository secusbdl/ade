// Test-local wrappers preserve the original three exploit schedules and also
// assert the fixed same-handle boundary is exercised. No production test hooks.
#include "core/ToolExecutor.h"
#include "attachments/AttachmentStore.h"
#include "runtime/AttachmentFileBroker.h"
#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <windows.h>

namespace {
QString iterationPath,insideDirectory,outsideDirectory,savedDirectory,failure;
bool attachmentMode=false,iterationArmed=false,swapped=false,restored=false,secondSwap=false,finalRestore=false;
int attachmentRootOpens=0;
HANDLE listingHandle=INVALID_HANDLE_VALUE;
bool handleBoundaryReached=false,pinRenameBlocked=false;
bool junction(const QString& path,const QString& target){QProcess p;p.start("cmd.exe",{"/c","mklink","/J",QDir::toNativeSeparators(path),QDir::toNativeSeparators(target)});return p.waitForFinished(5000)&&p.exitCode()==0;}
bool outsideView(){
    if(attachmentMode){if(!QDir().rename(iterationPath,savedDirectory)){failure="Cannot preserve owned attachment directory";return false;}}
    else if(!QDir().rmdir(iterationPath)){failure="Cannot remove owned alias";return false;}
    if(!junction(iterationPath,outsideDirectory)){failure="Cannot install outside fixture alias";return false;}
    return true;
}
bool insideView(){
    if(!QDir().rmdir(iterationPath)){failure="Cannot remove outside alias";return false;}
    if(attachmentMode){if(!QDir().rename(savedDirectory,iterationPath)){failure="Cannot restore owned attachment directory";return false;}}
    else if(!junction(iterationPath,insideDirectory)){failure="Cannot restore owned alias";return false;}
    return true;
}
QString prepareIterator(const QString& path){if(iterationArmed&&path==iterationPath){iterationArmed=false;swapped=outsideView();}return path;}
// Only the directory iterator boundary is controlled: actual Qt enumeration,
// actual filesystem retargets, and actual production validation still execute.
class BoundaryDirIterator {
    bool controlled;QDirIterator iterator;QFileInfo current;
public:
    using IteratorFlags=QDirIterator::IteratorFlags;
    static constexpr auto Subdirectories=QDirIterator::Subdirectories;
    static constexpr auto NoIteratorFlags=QDirIterator::NoIteratorFlags;
    BoundaryDirIterator(const QString& path,QDir::Filters filters,IteratorFlags flags=NoIteratorFlags)
        :controlled(iterationArmed&&path==iterationPath),iterator(prepareIterator(path),filters,flags){}
    bool hasNext(){return iterator.hasNext();}
    QString next(){
        const auto path=iterator.next();current=iterator.fileInfo();(void)current.isFile();(void)current.isDir();(void)current.isSymLink();(void)current.isJunction();
        if(controlled&&!restored){restored=insideView();attachmentRootOpens=0;}
        return path;
    }
    QFileInfo fileInfo()const{return current;}
};
HANDLE WINAPI boundaryCreateFileW(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,DWORD disposition,DWORD flags,HANDLE model){
    const auto path=QDir::fromNativeSeparators(QString::fromWCharArray(name));
    if(attachmentMode&&restored&&!secondSwap&&access==0&&path==iterationPath){
        // First open is ownedDirectory's root resolution; the second opens the
        // returned root again in AttachmentStore::resolve's local scope.
        if(++attachmentRootOpens==2)secondSwap=outsideView();
    }
    const auto handle=::CreateFileW(name,access,share,security,disposition,flags,model);
    if(iterationArmed&&path==iterationPath&&access==FILE_LIST_DIRECTORY)listingHandle=handle;
    return handle;
}
DWORD WINAPI boundaryGetFinalPathNameByHandleW(HANDLE handle,LPWSTR path,DWORD capacity,DWORD flags){
    const auto result=::GetFinalPathNameByHandleW(handle,path,capacity,flags);const auto lastError=GetLastError();
    if(iterationArmed&&handle==listingHandle&&path&&result&&result<capacity){
        iterationArmed=false;handleBoundaryReached=true;
        if(attachmentMode){
            // The validated owned root is retained without FILE_SHARE_DELETE.
            // A real namespace replacement must fail while listing is active.
            pinRenameBlocked=!QDir().rename(iterationPath,savedDirectory);
            if(!pinRenameBlocked)failure="Attachment root was not pinned against replacement.";
        }else swapped=outsideView();
    }
    if(attachmentMode&&secondSwap&&!finalRestore&&path&&result&&result<capacity){
        const auto resolved=QDir::fromNativeSeparators(QString::fromWCharArray(path,result));
        if(resolved.endsWith("/OUTSIDE_ONLY_NAME.txt"))finalRestore=insideView();
    }
    SetLastError(lastError);return result;
}
void resetHooks(){iterationArmed=false;attachmentMode=false;swapped=false;restored=false;secondSwap=false;finalRestore=false;attachmentRootOpens=0;failure.clear();listingHandle=INVALID_HANDLE_VALUE;handleBoundaryReached=false;pinRenameBlocked=false;}
bool put(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
}
#define QDirIterator BoundaryDirIterator
#define CreateFileW boundaryCreateFileW
#define GetFinalPathNameByHandleW boundaryGetFinalPathNameByHandleW
#include "../src/core/ScopedDirectory.cpp"
#include "../src/core/ToolExecutor.cpp"
#include "../src/attachments/AttachmentStore.cpp"
#include "../src/runtime/AttachmentFileBroker.cpp"
#undef QDirIterator
#undef CreateFileW
#undef GetFinalPathNameByHandleW

using namespace ade;
class RawEnumerationTests final:public QObject {
    Q_OBJECT
private slots:
    void rawAndSearchNeverPublishOutsideNames_data(){
        QTest::addColumn<QString>("operation");
        QTest::newRow("raw-directory")<<QString("readDirectory");QTest::newRow("glob")<<QString("glob");QTest::newRow("grep")<<QString("grepSearch");
    }
    void rawAndSearchNeverPublishOutsideNames(){
        resetHooks();QTemporaryDir fixture;const auto root=fixture.filePath("workspace");insideDirectory=root+"/inside";outsideDirectory=fixture.filePath("outside");iterationPath=root+"/alias";
        QVERIFY(QDir().mkpath(insideDirectory));QVERIFY(QDir().mkpath(outsideDirectory));QVERIFY(put(insideDirectory+"/OWNED.txt","owned"));QVERIFY(put(outsideDirectory+"/OUTSIDE_ONLY_NAME.txt","OUTSIDE_BYTES"));
        QVERIFY(junction(iterationPath,insideDirectory));const auto cleanup=qScopeGuard([&]{iterationArmed=false;QDir().rmdir(iterationPath);});
        ToolExecutor tools;QVERIFY(tools.setWorkspace(root));QSignalSpy results(&tools,&ToolExecutor::completed);QFETCH(QString,operation);
        iterationArmed=true;tools.execute("enumerate",operation,{{"path","alias"},{"recursive",false},{"pattern","**/*.txt"},{"query","OUTSIDE_BYTES"}});QTRY_COMPARE_WITH_TIMEOUT(results.count(),1,5000);
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY2((swapped&&restored)||(handleBoundaryReached&&swapped),"The real enumeration boundary must be exercised.");const auto output=results[0][1].toString();QVERIFY2(!results[0][2].toBool(),qPrintable(output));
        QVERIFY2(!output.contains("OUTSIDE_ONLY_NAME")&&!output.contains("OUTSIDE_BYTES"),qPrintable(output));
    }
    void traversalLimitsAreExplicit_data(){
        QTest::addColumn<QString>("operation");QTest::addColumn<bool>("errorExpected");
        QTest::newRow("raw-depth-errors")<<QString("readDirectory")<<true;
        QTest::newRow("glob-depth-reported")<<QString("glob")<<false;
        QTest::newRow("grep-depth-reported")<<QString("grepSearch")<<false;
    }
    void traversalLimitsAreExplicit(){
        resetHooks();QTemporaryDir fixture;QString deep=fixture.path();
        for(int i=0;i<68;++i)deep+="/d";
        QVERIFY(QDir().mkpath(deep));QVERIFY(put(deep+"/TOO_DEEP.txt","needle"));
        QVERIFY(put(fixture.filePath("OWNED.txt"),"needle"));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(fixture.path()));QSignalSpy results(&tools,&ToolExecutor::completed);
        QFETCH(QString,operation);QFETCH(bool,errorExpected);
        tools.execute("depth",operation,{{"path","."},{"recursive",true},{"pattern","**/*.txt"},{"query","needle"}});
        QTRY_COMPARE_WITH_TIMEOUT(results.size(),1,5000);const auto output=results[0][1].toString();QCOMPARE(results[0][2].toBool(),errorExpected);
        QVERIFY2(!output.contains("TOO_DEEP"),qPrintable(output));
        if(errorExpected)QVERIFY2(output.contains("depth")&&output.contains("no partial"),qPrintable(output));
        else QVERIFY2(output.contains("OWNED.txt")&&output.contains("Search limit reached"),qPrintable(output));
    }
    void rawEntryLimitDoesNotReturnPartialNames(){
        resetHooks();QTemporaryDir fixture;
        for(int i=0;i<4097;++i)QVERIFY(put(fixture.filePath(QString::number(i)),{}));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(fixture.path()));QSignalSpy results(&tools,&ToolExecutor::completed);
        tools.execute("limit","readDirectory",{{"path","."}});QTRY_COMPARE_WITH_TIMEOUT(results.size(),1,5000);
        QVERIFY(results[0][2].toBool());QVERIFY2(results[0][1].toString().contains("no partial"),qPrintable(results[0][1].toString()));
        QVERIFY(QFile::remove(fixture.filePath("4096")));results.clear();
        tools.execute("limit","readDirectory",{{"path","."}});QTRY_COMPARE_WITH_TIMEOUT(results.size(),1,5000);
        QVERIFY2(!results[0][2].toBool(),qPrintable(results[0][1].toString()));
        QCOMPARE(QJsonDocument::fromJson(results[0][1].toString().toUtf8()).array().size(),4096);
    }
    void recursiveOperationsDoNotFollowChildJunctions_data(){
        QTest::addColumn<QString>("operation");
        QTest::newRow("raw")<<QString("readDirectory");QTest::newRow("glob")<<QString("glob");QTest::newRow("grep")<<QString("grepSearch");
    }
    void recursiveOperationsDoNotFollowChildJunctions(){
        resetHooks();QTemporaryDir fixture;const auto root=fixture.filePath("owned"),outside=fixture.filePath("outside"),link=root+"/link";
        QVERIFY(QDir().mkpath(root+"/child"));QVERIFY(QDir().mkpath(outside));QVERIFY(put(root+"/child/OWNED.txt","needle"));QVERIFY(put(outside+"/OUTSIDE.txt","needle"));
        QVERIFY(junction(link,outside));const auto cleanup=qScopeGuard([&]{QDir().rmdir(link);});
        ToolExecutor tools;QVERIFY(tools.setWorkspace(root));QSignalSpy results(&tools,&ToolExecutor::completed);QFETCH(QString,operation);
        tools.execute("links",operation,{{"path","."},{"recursive",true},{"pattern","**/*.txt"},{"query","needle"}});QTRY_COMPARE_WITH_TIMEOUT(results.size(),1,5000);
        const auto output=results[0][1].toString();QVERIFY2(!results[0][2].toBool(),qPrintable(output));QVERIFY2(output.contains("OWNED.txt")&&!output.contains("OUTSIDE.txt"),qPrintable(output));
    }
    void attachmentListingNeverPublishesOutsideNames(){
        resetHooks();QTemporaryDir fixture;const auto data=fixture.filePath("data");AttachmentStore store(data);QString error;const auto owned=store.write("agent","owned.txt","owned",&error);QVERIFY2(!owned.isEmpty(),qPrintable(error));
        iterationPath=store.agentDirectory("agent");insideDirectory=iterationPath;savedDirectory=iterationPath+"-saved";outsideDirectory=fixture.filePath("outside");QVERIFY(QDir().mkpath(outsideDirectory));QVERIFY(put(outsideDirectory+"/OUTSIDE_ONLY_NAME.txt","OUTSIDE_BYTES"));
        AttachmentFileBroker broker("agent",data);QSignalSpy results(&broker,&AttachmentFileBroker::completed);
        const auto cleanup=qScopeGuard([&]{iterationArmed=false;attachmentMode=false;if(QFileInfo::exists(savedDirectory)){QDir().rmdir(iterationPath);QDir().rename(savedDirectory,iterationPath);}});
        attachmentMode=true;iterationArmed=true;broker.execute("list","agent","readDirectory",{{"path","att"}});QTRY_COMPARE_WITH_TIMEOUT(results.count(),1,5000);
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY2((swapped&&restored&&secondSwap&&finalRestore)||(handleBoundaryReached&&pinRenameBlocked),"The real listing boundary must be exercised.");
        const auto output=results[0][1].toString();QVERIFY2(!results[0][2].toBool(),qPrintable(output));QVERIFY2(!output.contains("OUTSIDE_ONLY_NAME"),qPrintable(output));
    }
};
QTEST_GUILESS_MAIN(RawEnumerationTests)
#include "test_raw_enumeration_races.moc"
