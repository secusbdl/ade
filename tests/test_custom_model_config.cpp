#include "core/CustomModelConfig.h"
#include "core/ProviderConfig.h"
#include <QJsonDocument>
#include <QtTest>
using namespace ade;
namespace {CustomModelConfig model(){CustomModelConfig m;m.modelId="local/custom";m.displayName="Local custom";m.providerInstanceId="instance-a";return m;}}
class CustomModelConfigTest:public QObject {
 Q_OBJECT
private slots:
 void defaultsAndSafeProjection(){auto m=model();QString error;QVERIFY2(m.validate("instance-a",&error),qPrintable(error));QCOMPARE(m.contextWindowSize,128000);QCOMPARE(m.capabilities.value("inputModalities").toObject().value("image"),QJsonValue(false));m.headers={{"Authorization","SYNTHETIC_PRIVATE_HEADER"}};m.providerOptions={{"privateSyntheticOption","SYNTHETIC_PRIVATE_OPTION"}};ProviderConfig p;p.id="instance-a";p.customModels={m};p.customRequest={{"headers",m.headers},{"providerOptions",m.providerOptions}};p.requestOverrides={{"reasoningEffort","high"}};const auto publicBytes=QJsonDocument(p.publicJson()).toJson();QVERIFY(!publicBytes.contains("SYNTHETIC_PRIVATE"));QVERIFY(!publicBytes.contains("customRequest"));QVERIFY(!publicBytes.contains("requestOverrides"));QVERIFY(publicBytes.contains("Local custom"));const auto copy=CustomModelConfig::fromPublicJson(m.publicJson(),&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(copy.publicJson(),m.publicJson());QVERIFY(copy.headers.isEmpty());QVERIFY(copy.providerOptions.isEmpty());}
 void invalidMetadata_data(){QTest::addColumn<QString>("field");QTest::addColumn<QJsonValue>("value");QTest::newRow("empty-id")<<QString("modelId")<<QJsonValue("");QTest::newRow("built-in")<<QString("modelId")<<QJsonValue("gpt-6.1-sol");QTest::newRow("alias")<<QString("modelId")<<QJsonValue("default");QTest::newRow("bad-context-type")<<QString("contextWindowSize")<<QJsonValue("128000");QTest::newRow("fraction")<<QString("contextWindowSize")<<QJsonValue(1.5);QTest::newRow("zero")<<QString("contextWindowSize")<<QJsonValue(0);QTest::newRow("limit")<<QString("contextWindowSize")<<QJsonValue(100000001);QTest::newRow("bad-thinking")<<QString("thinkingEnabled")<<QJsonValue("false");QTest::newRow("bad-caps")<<QString("capabilities")<<QJsonValue(QJsonArray{});QTest::newRow("bad-id-control")<<QString("modelId")<<QJsonValue(QString("bad")+QChar(1));}
 void invalidMetadata(){QFETCH(QString,field);QFETCH(QJsonValue,value);auto json=model().publicJson();json[field]=value;QString error;CustomModelConfig::fromPublicJson(json,&error);QVERIFY2(!error.isEmpty(),qPrintable(field));}
 void ownerAndPrivateFailureAreAtomic(){auto m=model();QString error;QVERIFY(!m.validate("instance-b",&error));m.headers={{"x-fixture","SYNTHETIC_OLD"}};const auto before=m.privateJson();QVERIFY(!m.setPrivateJson({{"providerOptions",QJsonObject{}},{"headers",QJsonObject{{"x",42}}}},&error));QCOMPARE(m.privateJson(),before);QVERIFY(!error.contains("SYNTHETIC_OLD"));}
 void privateBounds(){auto m=model();QString error;m.providerOptions={{"value",QString(65537,'x')}};QVERIFY(!m.validate("instance-a",&error));m.providerOptions={};for(int i=0;i<65;++i)m.headers[QString("x-%1").arg(i)]="x";QVERIFY(!m.validate("instance-a",&error));m.headers={};QJsonObject nested{{"value",1}};for(int i=0;i<15;++i)nested={{"child",nested}};m.providerOptions=nested;QVERIFY(!m.validate("instance-a",&error));}
};
QTEST_GUILESS_MAIN(CustomModelConfigTest)
#include "test_custom_model_config.moc"
