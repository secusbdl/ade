#include "attachments/ContextContentLimits.h"
#include "attachments/FileTransformer.h"
#include "core/ModelDiscoveryPolicy.h"
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(QString model="unknown"){ProviderConfig p;p.id="instance";p.model=model;p.baseUrl="https://fixture.invalid/v1";return p;}
bool wholeUtf16(const QString& text){for(qsizetype i=0;i<text.size();++i){if(text[i].isHighSurrogate()){if(i+1>=text.size()||!text[++i].isLowSurrogate())return false;}else if(text[i].isLowSurrogate())return false;}return true;}
}
class ContextBudgetTest:public QObject {
 Q_OBJECT
private slots:
 void derivedCharacters_data(){
  QTest::addColumn<int>("tokens");QTest::addColumn<int>("characters");
  QTest::newRow("unknown")<<0<<40000;
  QTest::newRow("minimum")<<1<<4000;
  QTest::newRow("ten-thousand")<<10000<<4000;
  QTest::newRow("fractional")<<32768<<13107;
  QTest::newRow("128k")<<128000<<51200;
  QTest::newRow("1.05m")<<1050000<<420000;
  QTest::newRow("native-soft-ceiling")<<100000000<<4194304;
  QTest::newRow("invalid-negative")<<-1<<40000;
  QTest::newRow("invalid-overflow")<<INT_MAX<<40000;
 }
 void derivedCharacters(){QFETCH(int,tokens);QFETCH(int,characters);auto p=provider();p.contextWindowTokens=tokens;const auto limits=ContextContentLimits::forProvider(p);QCOMPARE(limits.maxReadChars,characters);QCOMPARE(limits.previewLines,30);QCOMPARE(limits.maxFileBytes,AttachmentLimits{}.maxFileBytes);QCOMPARE(limits.maxAttachments,AttachmentLimits{}.maxAttachments);QCOMPARE(limits.maxRequestImageBytes,AttachmentLimits{}.maxRequestImageBytes);QCOMPARE(limits.maxTextChars,tokens>0&&tokens<=100000000?1048576:65536);}
 void pinnedCatalogIsExactAndAliasesMatch(){QCOMPARE(ContextContentLimits::pinnedContextWindow("qwen3-32b"),128000);QCOMPARE(ContextContentLimits::pinnedContextWindow("gpt-6.1-sol"),1050000);for(const auto& pair:QList<QPair<QString,QString>>{{"default","deepseek-v4-pro"},{"quick","gemini-3.5-flash"},{"smart","glm-5.2"}})QCOMPARE(ContextContentLimits::pinnedContextWindow(pair.first),ContextContentLimits::pinnedContextWindow(pair.second));QCOMPARE(ContextContentLimits::pinnedContextWindow("gpt-6.1-sol-fake"),0);QCOMPARE(ContextContentLimits::pinnedContextWindow("GPT-6.1-SOL"),0);}
 void explicitContextAndCustomBindingDoNotInheritCatalog(){auto p=provider("gpt-6.1-sol");p.contextWindowTokens=10000;QCOMPARE(ContextContentLimits::contextWindow(p),10000);p.contextWindowTokens=0;p.customRequest={{"modelId",p.model}};QCOMPARE(ContextContentLimits::contextWindow(p),0);QCOMPARE(ContextContentLimits::forProvider(p).maxReadChars,40000);p.customRequest={};p.kind="opencode";QCOMPARE(ContextContentLimits::contextWindow(p),0);}
 void completeExactDiscoveryOnly(){auto p=provider();QJsonArray models{QJsonObject{{"id",p.model},{"name","Discovered"},{"inputTokenLimit",24000},{"outputTokenLimit",1000}}};p.discoverySnapshot=makeModelDiscoverySnapshot(p,models,true);QVERIFY(validModelDiscoverySnapshot(p,p.discoverySnapshot));QCOMPARE(ContextContentLimits::contextWindow(p),24000);p.contextWindowTokens=12000;QCOMPARE(ContextContentLimits::contextWindow(p),12000);p.contextWindowTokens=0;auto changed=p;changed.baseUrl="https://other.invalid/v1";QCOMPARE(ContextContentLimits::contextWindow(changed),0);changed=p;changed.discoverySnapshot["complete"]=false;QCOMPARE(ContextContentLimits::contextWindow(changed),0);changed=p;auto invalid=models[0].toObject();invalid["inputTokenLimit"]="24000";changed.discoverySnapshot["models"]=QJsonArray{invalid};QCOMPARE(ContextContentLimits::contextWindow(changed),0);}
 void hugeSingleLineIsCappedWithoutFalseRangeOrBrokenUnicode(){auto p=provider();p.contextWindowTokens=100000000;const auto limits=ContextContentLimits::forProvider(p);FileTransformLimits transform;transform.maxReadChars=limits.maxReadChars;transform.maxOutputChars=limits.maxTextChars;QString line(1048500,'x');for(int i=0;i<100;++i)line+=QString::fromUcs4(U"🙂");const auto result=FileTransformer::fromBytes(line.toUtf8(),{"att/a","large.txt",-1,{}},{},transform);QVERIFY(result.error.isEmpty());QVERIFY(result.truncated);QVERIFY(!result.coverageKnown);QVERIFY(!result.delivered().endLine);const auto text=result.parts[0].toObject()["text"].toString();QVERIFY(text.size()<=1048576);QVERIFY(wholeUtf16(text));QCOMPARE(result.metadata.value("partialContent"),QJsonValue("true"));}
 void contextLimitIsPerRequestAndPreviewRemainsSeparate(){auto small=provider(),large=small;small.contextWindowTokens=10000;large.contextWindowTokens=100000;QByteArray bytes;for(int i=0;i<600;++i)bytes+=QByteArray(99,'a')+'\n';FileTransformLimits a,b;a.maxReadChars=ContextContentLimits::forProvider(small).maxReadChars;b.maxReadChars=ContextContentLimits::forProvider(large).maxReadChars;const FileDescriptor descriptor{"ws1/text.txt","text.txt",bytes.size(),{}};const auto first=FileTransformer::fromBytes(bytes,descriptor,{},a),second=FileTransformer::fromBytes(bytes,descriptor,{},b),again=FileTransformer::fromBytes(bytes,descriptor,{},a);QCOMPARE(first.delivered().endLine,std::optional<int>(40));QCOMPARE(second.delivered().endLine,std::optional<int>(400));QCOMPARE(again.parts,first.parts);FileReadParams preview;preview.preview=true;const auto result=FileTransformer::fromBytes(bytes,descriptor,preview,a);QVERIFY(result.error.isEmpty());QVERIFY(result.parts[0].toObject()["text"].toString().contains("30|"));QVERIFY(!result.parts[0].toObject()["text"].toString().contains("31|"));}
};
QTEST_GUILESS_MAIN(ContextBudgetTest)
#include "test_context_budget.moc"
