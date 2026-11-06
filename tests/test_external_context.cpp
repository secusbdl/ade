#include "core/ContextLoader.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {bool write(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();}}
class ExternalContextTest:public QObject {
    Q_OBJECT
private slots:
    void preservedInstructionsAndExplicitSkillWithoutNativeRoster(){
        QTemporaryDir workspace,plugins;
        QVERIFY(write(workspace.filePath("AGENTS.md"),"Project rule: preserve public APIs.\n"));
        const QByteArray body="---\nname: invoked\ndescription: Explicit fixture\n---\nKeep this skill body exactly.\n";
        QVERIFY(write(workspace.filePath(".agents/skills/invoked/SKILL.md"),body));
        QVERIFY(write(plugins.filePath("broker/SKILL.md"),"---\nname: broker\ndescription: Native broker-only plugin\n---\nNative plugin implementation.\n"));
        ContextOptions options;options.workspace=workspace.path();options.pluginsDirectory=plugins.path();options.invokedSkillIds={"skill:invoked"};
        ContextLoader loader;const auto snapshot=loader.load(options);QCOMPARE(snapshot.invokedSkills.size(),1);QCOMPARE(snapshot.invokedSkills.first().content,QString::fromUtf8(body));
        const auto external=ContextLoader::externalHarnessContext(snapshot);
        QVERIFY(external.contains("Project rule: preserve public APIs."));QVERIFY(external.contains(QString::fromUtf8(body)));
        QVERIFY(external.contains("scope=\"ws1\""));QVERIFY(external.contains("context labels, not CLI file arguments"));
        QVERIFY(!external.contains("readContext"));QVERIFY(!external.contains("<available_skills>"));QVERIFY(!external.contains("Native broker-only plugin"));
        QVERIFY(!external.contains("API.openApp"));QVERIFY(!external.contains("API.getCredential"));
        QVERIFY(snapshot.prompt.contains("readContext"));QVERIFY(snapshot.prompt.contains("Native broker-only plugin"));
    }
    void disabledAndUnrespectedInstructionsAreNotReintroduced(){
        QTemporaryDir root;QVERIFY(write(root.filePath("AGENTS.md"),"Disabled instruction"));
        QVERIFY(write(root.filePath(".agents/skills/hidden/SKILL.md"),"---\nname: hidden\ndescription: Disabled skill\n---\nHidden content"));
        ContextOptions options;options.workspace=root.path();options.respectAgentsMd=false;options.disabledSkills={"hidden"};options.invokedSkillIds={"skill:hidden"};
        ContextLoader loader;const auto snapshot=loader.load(options);QVERIFY(snapshot.invokedSkills.isEmpty());const auto external=ContextLoader::externalHarnessContext(snapshot);
        QVERIFY(!external.contains("Disabled instruction"));QVERIFY(!external.contains("Hidden content"));
    }
    void markupAndExplicitCapabilityMentionsRemainData(){
        ContextSnapshot snapshot;snapshot.instructions.append({"ws1/a\"b/AGENTS.md","ws1/a\"b","Keep ]]> literal",true,false});
        snapshot.invokedSkills.append({"skill:x","ws1/.agents/skills/x/SKILL.md","Requested skill discusses readContext and API.getCredential; do not erase it."});
        const auto external=ContextLoader::externalHarnessContext(snapshot);QVERIFY(external.contains("a&quot;b"));QVERIFY(external.contains("]]]]><![CDATA[>"));
        QVERIFY(external.contains("Requested skill discusses readContext and API.getCredential; do not erase it."));QVERIFY(external.contains("may describe capabilities this runtime lacks"));
    }
    void boundedWholeEntriesDiscloseOmission(){
        ContextSnapshot snapshot;for(int i=0;i<30;++i)snapshot.instructions.append({QString::number(i),"conversation",QString(65536,'x'),true,false});
        const auto text=ContextLoader::externalHarnessContext(snapshot);QVERIFY(text.size()<=1024*1024);QVERIFY(text.contains("remaining whole entries omitted"));QVERIFY(text.endsWith("</external_agent_context>"));
        QCOMPARE(text.count("<context_file "),text.count("</context_file>"));
    }
    void oversizedSnapshotValueIsOmittedBeforeEscaping(){
        ContextSnapshot snapshot;snapshot.invokedSkills.append({"skill:large","context",QString(2*1024*1024,'&')});
        const auto text=ContextLoader::externalHarnessContext(snapshot);QVERIFY(text.size()<4096);QVERIFY(text.contains("remaining whole entries omitted"));QVERIFY(!text.contains("<invoked_skill "));
    }
};
QTEST_GUILESS_MAIN(ExternalContextTest)
#include "test_external_context.moc"
