#include "core/ProviderStore.h"
#include "core/CustomModelInventory.h"
#include "ui/ProviderRepository.h"
#include "ui/ModelSelection.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
ProviderConfig provider(QString id="one") {ProviderConfig p;p.id=id;p.name="Same provider";p.baseUrl="http://127.0.0.1:1/v1";p.model="legacy";return p;}
CustomModelConfig model(QString owner="one",QString id="local-private-model") {CustomModelConfig m;m.providerInstanceId=owner;m.modelId=id;m.displayName="Private custom";m.headers={{"x-fixture-private","SYNTHETIC_CUSTOM_HEADER_ONLY"}};return m;}
QByteArray read(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
bool write(const QString& path,const QJsonObject& object){QFile f(path);if(!f.open(QIODevice::WriteOnly))return false;const auto b=QJsonDocument(object).toJson();return f.write(b)==b.size();}
}
class CustomModelStorageTest:public QObject {
 Q_OBJECT
private slots:
 void encryptedRoundTripAndPublicBoundary(){
  QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("providers.json");auto p=provider();p.customModels={model()};p.customRequest={{"headers",p.customModels[0].headers}};p.requestOverrides={{"thinkingMode","disabled"}};
  QString error;QVERIFY2(ProviderStore::save(path,{p},&error),qPrintable(error));const auto bytes=read(path);QVERIFY(!bytes.contains("SYNTHETIC_CUSTOM_HEADER_ONLY"));QVERIFY(!bytes.contains("customRequest"));QVERIFY(!bytes.contains("requestOverrides"));
  const auto loaded=ProviderStore::load(path,&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(loaded.size(),1);QCOMPARE(loaded[0].customModels.size(),1);QCOMPARE(loaded[0].customModels[0].headers,p.customModels[0].headers);QCOMPARE(loaded[0].customModels[0].contextWindowSize,128000);QVERIFY(loaded[0].customRequest.isEmpty());QVERIFY(loaded[0].requestOverrides.isEmpty());QVERIFY(!QJsonDocument(loaded[0].publicJson()).toJson().contains("SYNTHETIC_CUSTOM_HEADER_ONLY"));
 }
 void corruptionIsPreserved_data(){QTest::addColumn<QString>("kind");for(const auto& k:{"cipher","plaintext","owner","capabilities","duplicate","retired_type","retired_duplicate","future_version"})QTest::newRow(k)<<QString(k);}
 void corruptionIsPreserved(){
  QFETCH(QString,kind);QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto p=provider();p.customModels={model()};QString error;QVERIFY(ProviderStore::save(path,{p},&error));auto doc=QJsonDocument::fromJson(read(path)).object();auto providers=doc.value("providers").toArray();auto item=providers[0].toObject();auto models=item.value("customModels").toArray();auto custom=models[0].toObject();
  if(kind=="cipher")custom["encryptedConfiguration"]="not-base64!";
  else if(kind=="plaintext")custom["headers"]=QJsonObject{{"x-fixture","SYNTHETIC"}};
  else if(kind=="owner")custom["providerInstanceId"]="foreign";
  else if(kind=="capabilities")custom["capabilities"]=QJsonObject{{"toolCalling","yes"}};
  else if(kind=="retired_type")item["retiredCustomModelIds"]=QJsonArray{17};
  else if(kind=="retired_duplicate")item["retiredCustomModelIds"]=QJsonArray{"gone","gone"};
  if(kind=="future_version")doc["version"]=999;
  models[0]=custom;if(kind=="duplicate")models.append(custom);item["customModels"]=models;providers[0]=item;doc["providers"]=providers;QVERIFY(write(path,doc));const auto corrupt=read(path);
  ProviderRepository repository(path);QVERIFY(repository.load(&error).isEmpty());QVERIFY(!error.isEmpty());QVERIFY(!repository.save({p},&error));QCOMPARE(read(path),corrupt);
 }
 void invalidSaveDoesNotReplaceOriginal_data(){QTest::addColumn<QString>("kind");for(const auto& k:{"owner","duplicate","unsupported_option","control_header","retired_capacity"})QTest::newRow(k)<<QString(k);}
 void invalidSaveDoesNotReplaceOriginal(){
  QFETCH(QString,kind);QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto p=provider();p.customModels={model()};QString error;QVERIFY(ProviderStore::save(path,{p},&error));const auto original=read(path);
  if(kind=="owner")p.customModels[0].providerInstanceId="foreign";
  else if(kind=="duplicate")p.customModels.append(p.customModels[0]);
  else if(kind=="unsupported_option")p.customModels[0].providerOptions={{"not_a_supported_provider_option",true}};
  else if(kind=="control_header")p.customModels[0].headers={{"x-fixture",QString("bad")+QChar(1)+"header"}};
  else for(int i=0;i<257;++i)p.retiredCustomModelIds.append(QString("gone-%1").arg(i));
  QVERIFY(!ProviderStore::save(path,{p},&error));QVERIFY(!error.isEmpty());QCOMPARE(read(path),original);
 }
 void exactInstanceCasAndRemovalRestart(){
  QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto a=provider(),b=provider("two");b.customModels={model("two")};QString error;QVERIFY(ProviderStore::save(path,{a,b},&error));ProviderRepository first(path),second(path);auto left=first.load(&error),right=second.load(&error);left[1].customModels[0].displayName="First committed";QVERIFY(first.save(left,&error));right[1].customModels[0].displayName="Stale";QVERIFY(!second.save(right,&error));QCOMPARE(ProviderStore::load(path,&error)[1].customModels[0].displayName,QString("First committed"));
  ProviderRepository remove(path);auto next=remove.load(&error);QVERIFY(removeCustomModel(next,"two","local-private-model",&error));QVERIFY(remove.save(next,&error));const auto reopened=ProviderStore::load(path,&error);QVERIFY(reopened[1].customModels.isEmpty());QVERIFY(reopened[1].retiredCustomModelIds.contains("local-private-model"));ProviderConfig resolved;QVERIFY(!(ModelSelection{"two","local-private-model",{},{},{}}).resolve(reopened,&resolved));
 }
 void importPreservesPrivateDefinitionsAndRetiredIdentity(){auto old=provider();old.customModels={model()};old.retiredCustomModelIds={"old-custom"};auto imported=old;imported.customModels.clear();imported.retiredCustomModelIds.clear();imported.apiKey="SYNTHETIC_REPLACEMENT_KEY";const auto merged=ProviderStore::mergeImported({old},{imported});QCOMPARE(merged[0].customModels[0].headers,old.customModels[0].headers);QCOMPARE(merged[0].retiredCustomModelIds,old.retiredCustomModelIds);QCOMPARE(merged[0].apiKey,imported.apiKey);}
 void customDefinitionsAndRetiredIdsRequireV2(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto p=provider();p.customModels={model()};QString error;QVERIFY(ProviderStore::save(path,{p},&error));QCOMPARE(QJsonDocument::fromJson(read(path)).object().value("version").toInt(),2);QList<ProviderConfig> providers{p};QVERIFY(removeCustomModel(providers,p.id,p.customModels[0].modelId,&error));QVERIFY(ProviderStore::save(path,providers,&error));QCOMPARE(QJsonDocument::fromJson(read(path)).object().value("version").toInt(),2);const auto reopened=ProviderStore::load(path,&error);QVERIFY(error.isEmpty());QCOMPARE(reopened[0].retiredCustomModelIds,providers[0].retiredCustomModelIds);}
 void legacyReadDoesNotMigrateAndOrdinarySaveRemainsV1(){QTemporaryDir dir;const auto path=dir.filePath("providers.json");QString error;QVERIFY(ProviderStore::save(path,{provider()},&error));const auto original=read(path);QCOMPARE(QJsonDocument::fromJson(original).object().value("version").toInt(),1);ProviderRepository repository(path);const auto p=repository.load(&error);QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(read(path),original);QVERIFY(repository.save(p,&error));QCOMPARE(QJsonDocument::fromJson(read(path)).object().value("version").toInt(),1);}
};
QTEST_GUILESS_MAIN(CustomModelStorageTest)
#include "test_custom_model_storage.moc"
