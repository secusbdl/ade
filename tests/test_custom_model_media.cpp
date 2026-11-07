#include "attachments/ModelMediaCatalog.h"
#include "ui/ModelSelection.h"
#include <QtTest>
using namespace ade;
class CustomModelMediaTest:public QObject {
 Q_OBJECT
private slots:
 void customImageUsesDeclarationAndNativeBounds(){ProviderConfig p;p.id="owner";p.model="legacy";CustomModelConfig m;m.providerInstanceId=p.id;m.modelId="custom-vision";m.displayName="Vision";auto input=m.capabilities.value("inputModalities").toObject();input["image"]=true;m.capabilities["inputModalities"]=input;p.customModels={m};ProviderConfig runtime;QVERIFY((ModelSelection{p.id,m.modelId,{},{},{}}).resolve({p},&runtime));const auto caps=ModelMediaCatalog::resolve(runtime);QVERIFY(caps.image);QCOMPARE(caps.maxImageBytes,5LL*1024*1024);QCOMPARE(caps.maxPixels,16LL*1024*1024);QVERIFY(caps.imageMimeTypes.contains("image/png"));QVERIFY(!ModelMediaCatalog::resolve(m.modelId).image);input["image"]=false;p.customModels[0].capabilities["inputModalities"]=input;QVERIFY((ModelSelection{p.id,m.modelId,{},{},{}}).resolve({p},&runtime));QVERIFY(!ModelMediaCatalog::resolve(runtime).image);}
 void invalidBindingCannotGrantMedia_data(){QTest::addColumn<QString>("kind");for(const auto& kind:{"owner","model","shape","type"})QTest::newRow(kind)<<QString(kind);}
 void invalidBindingCannotGrantMedia(){QFETCH(QString,kind);ProviderConfig p;p.id="owner";p.model="gpt-6.1-sol";auto caps=defaultCustomModelCapabilities();auto input=caps.value("inputModalities").toObject();input["image"]=true;caps["inputModalities"]=input;p.customRequest={{"providerInstanceId",p.id},{"modelId",p.model},{"capabilities",caps}};if(kind=="owner")p.customRequest["providerInstanceId"]="foreign";else if(kind=="model")p.customRequest["modelId"]="other";else if(kind=="shape")p.customRequest["capabilities"]=QJsonObject{{"image",true}};else {input["image"]="yes";caps["inputModalities"]=input;p.customRequest["capabilities"]=caps;}QVERIFY(!ModelMediaCatalog::resolve(p).image);}
 void regularAndExternalCapabilitiesRemain(){ProviderConfig p;p.model="gpt-6.1-sol";QVERIFY(ModelMediaCatalog::resolve(p).image);p.model="unknown-plain";QVERIFY(!ModelMediaCatalog::resolve(p).image);p.kind="claude-code";QVERIFY(ModelMediaCatalog::resolve(p).image);}
};
QTEST_GUILESS_MAIN(CustomModelMediaTest)
#include "test_custom_model_media.moc"
