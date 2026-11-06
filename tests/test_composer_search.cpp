#include "ui/ComposerWorkspaceSearch.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QUrl>
using namespace ade;
namespace {
void write(const QString &path){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);if(f.open(QIODevice::WriteOnly))f.write("fixture\n");}
QStringList paths(const QJsonArray &items){QStringList result;for(const auto &v:items)result.append(v.toObject()["relativePath"].toString());return result;}
}
class ComposerSearchTest:public QObject {
    Q_OBJECT
private slots:
    void searchesAndPinsCurrentPhysicalEntry(){QTemporaryDir tmp;write(tmp.filePath("src/main.cpp"));write(tmp.filePath("README.md"));const auto result=ComposerWorkspaceSearch::search(tmp.path(),"main",{});QVERIFY2(result.error.isEmpty(),qPrintable(result.error));QCOMPARE(result.items.size(),1);QCOMPARE(paths(result.items).first(),QString("src/main.cpp"));QString error;auto pin=ComposerWorkspaceSearch::pin(result.root,"src/main.cpp",result.rootIdentity,&error);QVERIFY2(pin,qPrintable(error));QVERIFY(!pin.directory);QVERIFY(!QFile::rename(pin.absolutePath,tmp.filePath("moved.cpp")));pin={};QVERIFY(QFile::rename(tmp.filePath("src/main.cpp"),tmp.filePath("moved.cpp")));}
    void omitsGeneratedTreesAndIncludesHiddenFiles(){QTemporaryDir tmp;write(tmp.filePath(".hidden"));write(tmp.filePath(".git/config"));write(tmp.filePath("node_modules/module/file"));write(tmp.filePath("untracked.txt"));const auto result=ComposerWorkspaceSearch::search(tmp.path(),{},{});const auto found=paths(result.items);QVERIFY(found.contains(".hidden"));QVERIFY(found.contains("untracked.txt"));QVERIFY(!found.contains(".git/config"));QVERIFY(!found.contains("node_modules/module/file"));}
    void unicodeAndParenthesesAreEncoded(){QTemporaryDir tmp;write(tmp.filePath(QString::fromUtf8("notes/café (1).md")));const auto result=ComposerWorkspaceSearch::search(tmp.path(),"caf",{});QCOMPARE(result.items.size(),1);const auto item=result.items.first().toObject();const auto markup=item["insertText"].toString();QVERIFY(markup.contains("caf%C3%A9"));QVERIFY(markup.contains("%20"));QVERIFY(!markup.contains(" (1)"));QVERIFY(ComposerWorkspaceSearch::pin(result.root,item["relativePath"].toString(),result.rootIdentity));}
    void rejectsHostileRelativePaths_data(){QTest::addColumn<QString>("path");for(const auto &p:QStringList{"../file","C:/file","/file","a\\file","a/../file","./file"})QTest::newRow(qPrintable(p))<<p;}
    void rejectsHostileRelativePaths(){QFETCH(QString,path);QTemporaryDir tmp;QVERIFY(!ComposerWorkspaceSearch::pin(tmp.path(),path,{}));}
    void rootReplacementCannotInheritAResult(){QTemporaryDir tmp;const auto root=tmp.filePath("root");write(root+"/file.txt");const auto result=ComposerWorkspaceSearch::search(root,"file",{});QCOMPARE(result.items.size(),1);QVERIFY(QDir().rename(root,tmp.filePath("old")));write(root+"/file.txt");QVERIFY(!ComposerWorkspaceSearch::pin(root,"file.txt",result.rootIdentity));}
    void junctionIsNotEnumeratedOrSelected(){QTemporaryDir inside,outside;write(outside.filePath("private.txt"));const auto link=inside.filePath("escape");QProcess p;p.start("cmd.exe",{"/D","/C","mklink","/J",QDir::toNativeSeparators(link),QDir::toNativeSeparators(outside.path())});QVERIFY(p.waitForFinished(5000));if(p.exitCode()!=0)QSKIP("Junction creation unavailable");const auto result=ComposerWorkspaceSearch::search(inside.path(),{},{});const bool listed=paths(result.items).contains("escape");const auto pin=ComposerWorkspaceSearch::pin(inside.path(),"escape/private.txt",result.rootIdentity);QVERIFY(QDir().rmdir(link));QVERIFY(!listed);QVERIFY(!pin);}
    void cancellationAndResultLimit(){QTemporaryDir tmp;for(int i=0;i<100;++i)write(tmp.filePath(QString("file-%1.txt").arg(i)));auto cancelled=std::make_shared<std::atomic_bool>(true);const auto stopped=ComposerWorkspaceSearch::search(tmp.path(),{},cancelled);QVERIFY(stopped.items.isEmpty());const auto result=ComposerWorkspaceSearch::search(tmp.path(),{},{});QCOMPARE(result.items.size(),64);}
};
QTEST_GUILESS_MAIN(ComposerSearchTest)
#include "test_composer_search.moc"
