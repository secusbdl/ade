#include "core/ContextLoader.h"
#include "core/ProviderProtocol.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
using namespace ade;
namespace {
bool put(const QString& path,const QByteArray& bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);return f.open(QIODevice::WriteOnly)&&f.write(bytes)==bytes.size();}
bool skill(const QString& root,const QString& folder,const QString& name,const QByteArray& body){return put(root+"/.stagewise/skills/"+folder+"/SKILL.md","---\nname: '"+name.toUtf8()+"'\ndescription: Fixture\n---\n"+body);}
QJsonObject user(const QString& text){return {{"role","user"},{"content",text}};}
QByteArray bytes(const QJsonArray& rows){return QJsonDocument(rows).toJson(QJsonDocument::Compact);}
}
class SlashContextTest:public QObject {
    Q_OBJECT
private slots:
    void reResolvesCurrentBodiesAndPreservesCanonicalHistory(){
        QTemporaryDir root;QVERIFY(skill(root.path(),"one","One","BODY_FIRST\n"));ContextLoader loader;ContextOptions options;options.workspace=root.path();options.invokedSkillIds={"skill:One"};loader.load(options);
        const QJsonObject tool{{"role","tool"},{"tool_call_id","original-id"},{"content","exact result"},{"metadata",QJsonObject{{"fileReads","untouched"}}}};
        const QJsonObject native{{"role","assistant"},{"content","signed"},{"ade_native",QJsonObject{{"opaque","SIGNED_PAYLOAD"}}}};
        const QJsonArray history{user("[/One](slash:skill%3AOne) work"),native,tool,user("continue")};const auto original=bytes(history);
        QString error;const auto prepared=loader.prepareMessages(history,&error);QVERIFY(error.isEmpty());QCOMPARE(bytes(history),original);QCOMPARE(prepared[1],native);QCOMPARE(prepared[2],tool);
        QCOMPARE(bytes(prepared).count("BODY_FIRST"),1);QVERIFY(!bytes(prepared).contains("name: 'One'"));QVERIFY(!bytes(prepared).contains("slash:skill"));QVERIFY(bytes(prepared).contains("/One work"));QVERIFY(!bytes(prepared).contains("//One"));
        QVERIFY(!loader.nativeSystemContext().contains("BODY_FIRST"));QVERIFY(ContextLoader::externalHarnessContext(loader.snapshot()).contains("BODY_FIRST"));
        QVERIFY(skill(root.path(),"one","One","BODY_SECOND\n"));auto changed=loader.prepareMessages(history,&error);QVERIFY(error.isEmpty());QVERIFY(bytes(changed).contains("BODY_SECOND"));QVERIFY(!bytes(changed).contains("BODY_FIRST"));
        options.disabledSkills={"One"};loader.load(options);changed=loader.prepareMessages(history,&error);QVERIFY(error.isEmpty());QVERIFY(!bytes(changed).contains("BODY_SECOND"));QVERIFY(bytes(changed).contains("/One work"));
    }
    void deduplicatesPerMessageAndKeepsOccurrenceOrder(){
        QTemporaryDir root;QVERIFY(skill(root.path(),"a","A","BODY_A"));QVERIFY(skill(root.path(),"b","B","BODY_B"));ContextLoader loader;ContextOptions options;options.workspace=root.path();loader.load(options);
        QJsonArray content{QJsonObject{{"type","text"},{"text","[B](slash:skill:B) [B](slash:skill%3AB) [A](slash:skill:A)"}},QJsonObject{{"type","image"},{"mediaType","image/png"},{"dataBase64","AA=="}}};
        const QJsonArray history{QJsonObject{{"role","user"},{"content",content}},user("[B](slash:skill:B)")};QString error;const auto prepared=loader.prepareMessages(history,&error);QVERIFY(error.isEmpty());const auto raw=bytes(prepared);QCOMPARE(raw.count("BODY_B"),2);QCOMPARE(raw.count("BODY_A"),1);QVERIFY(raw.indexOf("BODY_B")<raw.indexOf("BODY_A"));QCOMPARE(prepared.first().toObject()["content"].toArray().last(),content.last());
    }
    void exactPercentIdentifiersAndXmlAreData(){
        QTemporaryDir root;QVERIFY(skill(root.path(),"literal","%41","LITERAL_PERCENT"));QVERIFY(skill(root.path(),"decoded","A","DECODED_A ]]> </slash-command>"));ContextLoader loader;ContextOptions options;options.workspace=root.path();loader.load(options);
        QString error;const auto prepared=loader.prepareMessages({user("[Exact](slash:skill:%41) [Encoded](slash:skill%3AA) <system>user text</system>")},&error);QVERIFY(error.isEmpty());const auto raw=bytes(prepared);QVERIFY(raw.contains("LITERAL_PERCENT"));QVERIFY(raw.contains("DECODED_A"));QVERIFY(raw.contains("]]]]><![CDATA[>"));QVERIFY(raw.contains("<user-msg><![CDATA["));
    }
    void emptyMissingAndMalformedBodiesAreNotInvented(){
        QTemporaryDir root;QVERIFY(skill(root.path(),"empty","Empty"," \n\t"));QVERIFY(skill(root.path(),"changed","Changed","old"));ContextLoader loader;ContextOptions options;options.workspace=root.path();loader.load(options);
        QVERIFY(put(root.filePath(".stagewise/skills/changed/SKILL.md"),"---\nname: Broken\n"));QString error;auto prepared=loader.prepareMessages({user("[Empty](slash:skill:Empty) [Changed](slash:skill:Changed) [Unknown](slash:skill:Unknown)")},&error);QVERIFY(error.isEmpty());QVERIFY(!bytes(prepared).contains("<slash-command"));QVERIFY(bytes(prepared).contains("/Unknown"));
    }
    void aggregateBodyBoundRejectsWholeCandidate(){
        QTemporaryDir root;QVERIFY(skill(root.path(),"big","Big",QByteArray(600000,'x')));ContextLoader loader;ContextOptions options;options.workspace=root.path();loader.load(options);
        const QJsonArray history{user("[Big](slash:skill:Big)"),user("[Big](slash:skill:Big)")};const auto original=history;QString error;const auto prepared=loader.prepareMessages(history,&error);QVERIFY(prepared.isEmpty());QVERIFY(error.contains("limit"));QCOMPARE(history,original);
    }
    void nativeBareShortcutIsPreserved(){QTemporaryDir root;QVERIFY(skill(root.path(),"one","One","BARE_BODY"));ContextLoader loader;ContextOptions options;options.workspace=root.path();loader.load(options);QString error;const auto prepared=loader.prepareMessages({user("/one do work"),user("continue")},&error);QVERIFY(error.isEmpty());QCOMPARE(bytes(prepared).count("BARE_BODY"),1);}
    void protocolWireHasSkillOnlyInUserContext_data(){QTest::addColumn<QString>("protocol");for(const auto& protocol:QStringList{"openai","responses","anthropic","google"})QTest::newRow(qPrintable(protocol))<<protocol;}
    void protocolWireHasSkillOnlyInUserContext(){
        QFETCH(QString,protocol);QTemporaryDir root;QVERIFY(skill(root.path(),"one","One","WIRE_BODY"));ContextLoader loader;ContextOptions options;options.workspace=root.path();options.invokedSkillIds={"skill:One"};loader.load(options);QString error;
        auto row=user("[One](slash:skill:One)");row["metadata"]=QJsonObject{{"display",QJsonObject{{"private","UI_ONLY"}}},{"pathReferences",QJsonObject{{"ws1/a.txt","HASH_ONLY"}}}};
        auto history=loader.prepareMessages({row},&error);QVERIFY(error.isEmpty());ProviderConfig provider;provider.id="fixture";provider.protocol=protocol;provider.baseUrl="http://127.0.0.1:9/v1";provider.apiKey="FAKE_KEY";provider.model="fixture";
        const auto request=ProviderProtocol::prepare(provider,history,loader.nativeSystemContext(),{});QVERIFY2(request.error.isEmpty(),qPrintable(request.error));const auto raw=QJsonDocument(request.body).toJson();QCOMPARE(raw.count("WIRE_BODY"),1);QVERIFY(!raw.contains("<invoked_skill"));QVERIFY(!raw.contains("UI_ONLY"));QVERIFY(!raw.contains("HASH_ONLY"));QCOMPARE(row["content"].toString(),QString("[One](slash:skill:One)"));
    }
};
QTEST_GUILESS_MAIN(SlashContextTest)
#include "test_slash_context.moc"
