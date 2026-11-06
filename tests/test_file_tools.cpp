#include "core/ToolExecutor.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace ade;
namespace {
bool put(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
struct Reply { QString output;bool error=true;bool arrived=false; };
Reply invoke(ToolExecutor& tools,const QString& name,const QJsonObject& args){
    QSignalSpy completed(&tools,&ToolExecutor::completed);tools.execute("fixture",name,args);
    if(completed.isEmpty()&&!completed.wait(10000))return {};
    const auto row=completed.first();return {row[1].toString(),row[2].toBool(),true};
}
}
class FileToolTests final:public QObject {
    Q_OBJECT
private slots:
    void structuredRangesAndRawRemainDistinct(){
        QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(put(temp.filePath("lines.txt"),"alpha\r\nbeta\r\ngamma\r\n"));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(temp.path()));
        const auto range=invoke(tools,"read",{{"path","ws1/lines.txt"},{"start_line",2},{"end_line",3}});QVERIFY(range.arrived);QVERIFY(!range.error);
        QVERIFY(range.output.startsWith("<file path=\"ws1/lines.txt\""));QVERIFY(range.output.contains("startLine=\"2\""));QVERIFY(range.output.contains("endLine=\"3\""));
        QVERIFY(range.output.contains("2|beta\r\n3|gamma\r"));QVERIFY(!range.output.contains("alpha"));QVERIFY(range.output.contains("modified:"));QVERIFY(range.output.endsWith("</content>\n</file>"));
        const auto raw=invoke(tools,"read",{{"path","ws1/lines.txt"},{"raw",true}});QVERIFY(!raw.error);QCOMPARE(raw.output,QString("alpha\r\nbeta\r\ngamma\r\n"));
    }
    void previewsPromoteSmallFilesAndOutlineLargeMarkdown(){
        QTemporaryDir temp;QVERIFY(put(temp.filePath("small.md"),"# Small\nbody"));
        QByteArray markdown="# Beginning\n";for(int i=0;i<155;++i)markdown+="context line\n";markdown+="## Final section\nTAIL_MARKER\n";
        QVERIFY(put(temp.filePath("large.md"),markdown));ToolExecutor tools;QVERIFY(tools.setWorkspace(temp.path()));
        const auto small=invoke(tools,"read",{{"path","small.md"},{"preview",true}});QVERIFY(!small.error);QVERIFY(small.output.contains("<content>"));QVERIFY(small.output.contains("2|body"));QVERIFY(!small.output.contains("<preview>"));
        const auto large=invoke(tools,"read",{{"path","large.md"},{"preview",true}});QVERIFY(!large.error);QVERIFY(large.output.contains("<preview>"));QVERIFY(large.output.contains("Final section"));QVERIFY(!large.output.contains("TAIL_MARKER"));
        const auto pages=invoke(tools,"read",{{"path","small.md"},{"start_page",1},{"end_page",2}});QVERIFY(!pages.error);QVERIFY(pages.output.contains("2|body"));
    }
    void invalidRequestsAreRejected_data(){
        QTest::addColumn<QString>("name");QTest::addColumn<QJsonObject>("args");
        QTest::newRow("missing")<<QString("read")<<QJsonObject{};
        QTest::newRow("wrong-path-type")<<QString("ls")<<QJsonObject{{"path",5}};
        QTest::newRow("line-zero")<<QString("read")<<QJsonObject{{"path","file.txt"},{"start_line",0}};
        QTest::newRow("line-fraction")<<QString("read")<<QJsonObject{{"path","file.txt"},{"start_line",1.5}};
        QTest::newRow("reversed")<<QString("read")<<QJsonObject{{"path","file.txt"},{"start_line",3},{"end_line",2}};
        QTest::newRow("preview-type")<<QString("read")<<QJsonObject{{"path","file.txt"},{"preview","true"}};
        QTest::newRow("page-zero")<<QString("read")<<QJsonObject{{"path","file.txt"},{"end_page",0}};
        QTest::newRow("negative-depth")<<QString("ls")<<QJsonObject{{"path","."},{"depth",-1}};
        QTest::newRow("depth-type")<<QString("ls")<<QJsonObject{{"path","."},{"depth","2"}};
        QTest::newRow("excess-depth")<<QString("ls")<<QJsonObject{{"path","."},{"depth",33}};
        QTest::newRow("ls-file")<<QString("ls")<<QJsonObject{{"path","file.txt"}};
        QTest::newRow("escape")<<QString("read")<<QJsonObject{{"path","../outside.txt"}};
    }
    void invalidRequestsAreRejected(){
        QFETCH(QString,name);QFETCH(QJsonObject,args);QTemporaryDir temp;QVERIFY(put(temp.filePath("file.txt"),"first\nsecond\nthird"));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(temp.path()));const auto result=invoke(tools,name,args);QVERIFY(result.arrived);QVERIFY(result.error);QVERIFY(!result.output.isEmpty());
    }
    void directoryDepthAndHiddenEntries(){
        QTemporaryDir temp;QVERIFY(QDir().mkpath(temp.filePath("branch/nested")));QVERIFY(QDir().mkpath(temp.filePath(".git")));
        QVERIFY(put(temp.filePath("branch/child.txt"),"child"));QVERIFY(put(temp.filePath("branch/nested/deep.txt"),"deep"));QVERIFY(put(temp.filePath(".hidden"),"hidden"));QVERIFY(put(temp.filePath("root.txt"),"root"));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(temp.path()));const auto shallow=invoke(tools,"ls",{{"path","ws1"}});QVERIFY(!shallow.error);
        QVERIFY(shallow.output.startsWith("<dir path=\"ws1\">"));QVERIFY(shallow.output.contains("depth:1"));QVERIFY(shallow.output.contains(".git/"));QVERIFY(shallow.output.contains(".hidden"));QVERIFY(!shallow.output.contains("child.txt"));
        const auto fractional=invoke(tools,"ls",{{"path","ws1"},{"depth",0.5}});QVERIFY(!fractional.error);QVERIFY(fractional.output.contains("child.txt"));QVERIFY(!fractional.output.contains("deep.txt"));QVERIFY(fractional.output.contains("depth:1.5"));
        const auto full=invoke(tools,"read",{{"path","ws1"}});QVERIFY(!full.error);QVERIFY(full.output.contains("deep.txt"));QVERIFY(full.output.endsWith("</content>\n</dir>"));
    }
    void outputBudgetAndBinaryPlaceholder(){
        QTemporaryDir temp;QByteArray large;for(int i=0;i<900;++i)large+=QByteArray(80,'x')+'\n';QVERIFY(put(temp.filePath("large.txt"),large));
        const QByteArray binary("\x00\x01\xff\x00",4);QVERIFY(put(temp.filePath("raw.bin"),binary));ToolExecutor tools;QVERIFY(tools.setWorkspace(temp.path()));
        const auto full=invoke(tools,"read",{{"path","large.txt"}});QVERIFY(!full.error);QVERIFY(full.output.contains("truncated=\"true\""));QVERIFY(full.output.contains("token budget reached"));QVERIFY(full.output.endsWith("</content>\n</file>"));QVERIFY(full.output.size()<65536);
        const auto continued=invoke(tools,"read",{{"path","large.txt"},{"start_line",850},{"end_line",852}});QVERIFY(!continued.error);QVERIFY(continued.output.contains("850|"));QVERIFY(continued.output.contains("852|"));QVERIFY(!continued.output.contains("853|"));
        const auto placeholder=invoke(tools,"read",{{"path","raw.bin"}});QVERIFY(!placeholder.error);QVERIFY(placeholder.output.contains("Unsupported binary file (.bin)"));
        const auto raw=invoke(tools,"readBytes",{{"path","raw.bin"}});QVERIFY(!raw.error);QCOMPARE(QByteArray::fromBase64(raw.output.toLatin1()),binary);
    }
    void appsWithoutWorkspaceAndQueuedCancellation(){
        QTemporaryDir apps;QVERIFY(put(apps.filePath("index.html"),"<h1>APP_MOUNT</h1>"));ToolExecutor tools;QVERIFY(tools.setAppsDirectory(apps.path()));
        const auto result=invoke(tools,"read",{{"path","apps/index.html"}});QVERIFY(!result.error);QVERIFY(result.output.contains("APP_MOUNT"));
        QSignalSpy completed(&tools,&ToolExecutor::completed);tools.execute("cancelled","read",{{"path","apps/index.html"}});tools.cancel();QCoreApplication::processEvents();
        QCOMPARE(completed.size(),1);QVERIFY(completed.first()[2].toBool());QVERIFY(!completed.first()[1].toString().contains("APP_MOUNT"));
    }
    void reservedMountsNeverFallThroughToWorkspace(){
        QTemporaryDir workspace;QVERIFY(QDir().mkpath(workspace.filePath("att")));QVERIFY(put(workspace.filePath("att/blob.txt"),"WORKSPACE_FOLDER_CONTENT"));
        ToolExecutor tools;QVERIFY(tools.setWorkspace(workspace.path()));
        for(const auto& mount:{"att","plugins","logs","shells","memory","w2","ws2"}){
            const auto read=invoke(tools,"read",{{"path",QString(mount)+"/blob.txt"}});QVERIFY(read.arrived);QVERIFY(read.error);
            const auto write=invoke(tools,"write",{{"path",QString(mount)+"/blob.txt"},{"content","must not be written"}});QVERIFY(write.arrived);QVERIFY(write.error);
        }
        const auto explicitWorkspace=invoke(tools,"read",{{"path","ws1/att/blob.txt"}});QVERIFY(!explicitWorkspace.error);QVERIFY(explicitWorkspace.output.contains("WORKSPACE_FOLDER_CONTENT"));
    }
};
QTEST_GUILESS_MAIN(FileToolTests)
#include "test_file_tools.moc"
