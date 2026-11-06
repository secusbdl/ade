#include "ui/CommandFileSearch.h"
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {void put(const QString& path,const QByteArray& value){QDir().mkpath(QFileInfo(path).path());QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));QCOMPARE(f.write(value),value.size());}bool git(const QString& root,const QStringList& args){QProcess p;p.setWorkingDirectory(root);p.start("git",args);return p.waitForFinished(5000)&&p.exitCode()==0;}bool has(const QJsonArray& values,const QString& path){for(const auto& value:values)if(value.toObject()["relativePath"].toString()==path)return true;return false;}}
class FileSearchTest:public QObject{
Q_OBJECT
private slots:
void ignoresAndUntracked(){QTemporaryDir dir;QVERIFY(dir.isValid());QVERIFY(git(dir.path(),{"init","-q"}));put(dir.filePath(".gitignore"),"ignored.txt\n");put(dir.filePath("source.cpp"),"source");put(dir.filePath("ignored.txt"),"ignored");auto result=CommandFileSearch::search({dir.path(),"",false,false},{});QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QVERIFY(has(result.items,"source.cpp"));QVERIFY(!has(result.items,"ignored.txt"));result=CommandFileSearch::search({dir.path(),"ignored",false,true},{});QVERIFY(has(result.items,"ignored.txt"));}
void contentLineUtf16AndBinary(){QTemporaryDir dir;QVERIFY(git(dir.path(),{"init","-q"}));put(dir.filePath("one.cpp"),"first\nneedle two\nthird\n");put(dir.filePath("two.txt"),QByteArray::fromHex("fffe6e006500650064006c0065000a00"));put(dir.filePath("binary.bin"),QByteArray("needle\0rest",11));const auto result=CommandFileSearch::search({dir.path(),"needle",true,false},{});QVERIFY(has(result.items,"one.cpp"));QVERIFY(has(result.items,"two.txt"));QVERIFY(!has(result.items,"binary.bin"));for(const auto& entry:result.items)if(entry.toObject()["relativePath"]=="one.cpp")QCOMPARE(entry.toObject()["line"].toInt(),2);}
void folderAndHostileFilename(){QTemporaryDir dir;QVERIFY(git(dir.path(),{"init","-q"}));put(dir.filePath("nested/file.txt"),"ok");put(dir.filePath("--danger (1).txt"),"ok");const auto result=CommandFileSearch::search({dir.path(),"",false,false},{});QVERIFY(has(result.items,"nested"));QVERIFY(has(result.items,"--danger (1).txt"));for(const auto& value:result.items)QVERIFY(!value.toObject()["workspaceIdentity"].toString().isEmpty());}
void cancellation(){QTemporaryDir dir;QVERIFY(git(dir.path(),{"init","-q"}));put(dir.filePath("source.cpp"),"source");auto canceled=std::make_shared<std::atomic_bool>(true);QVERIFY(CommandFileSearch::search({dir.path(),"",false,false},canceled).items.isEmpty());}
void nonGitFallbackIsExplicit(){QTemporaryDir dir;put(dir.filePath("source.cpp"),"source");const auto result=CommandFileSearch::search({dir.path(),"source",false,false},{});QVERIFY(has(result.items,"source.cpp"));QVERIFY(result.limited);QVERIFY(result.error.contains("Non-Git"));}
void outsideJunctionIsNotRead(){QTemporaryDir dir,outside;QVERIFY(git(dir.path(),{"init","-q"}));put(outside.filePath("secret.txt"),"sensitive sentinel");QProcess p;p.start("cmd",{"/d","/c","mklink","/J",QDir::toNativeSeparators(dir.filePath("alias")),QDir::toNativeSeparators(outside.path())});QVERIFY(p.waitForFinished(5000));QCOMPARE(p.exitCode(),0);const auto result=CommandFileSearch::search({dir.path(),"sentinel",true,true},{});QVERIFY(result.items.isEmpty());QVERIFY(QDir().rmdir(dir.filePath("alias")));}
};
QTEST_GUILESS_MAIN(FileSearchTest)
#include "test_command_file_search.moc"
