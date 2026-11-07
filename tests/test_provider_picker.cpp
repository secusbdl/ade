#include "ui/AddProviderDialog.h"
#include "ui/ProviderCatalog.h"
#include "ui/AppStyle.h"
#include "core/ProviderProtocol.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
using namespace ade;
namespace {
QByteArray read(const QString& path){QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}
void expose(AddProviderDialog& dialog,int width=512){dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.resize(width,455);dialog.show();QTest::qWait(30);}
}
class ProviderPickerTest:public QObject {
 Q_OBJECT
private slots:
 void catalogContainsOnlyImplementedRoutes(){
    QSet<QString> ids;
    for(const auto& entry:ProviderCatalog::all()) {
        QVERIFY(!ids.contains(entry.id));ids.insert(entry.id);
        QVERIFY(!entry.name.isEmpty());QVERIFY(ProviderCatalog::groups().contains(entry.group));
        const auto p=ProviderCatalog::instantiate(entry.id);
        QVERIFY(!p.id.isEmpty());QVERIFY(p.apiKey.isEmpty());QVERIFY(p.harnessExecutable.isEmpty());
        if(p.isOpenCodeHarness()) {
            QCOMPARE(entry.id,QString("opencode"));QCOMPARE(p.model,QString("default"));
            QCOMPARE(p.protocol,QString("opencode-acp"));QVERIFY(p.baseUrl.isEmpty());
        } else if(p.isClaudeHarness()) {
            QCOMPARE(entry.id,QString("claude-code"));QCOMPARE(p.model,QString("default"));
            QCOMPARE(p.protocol,QString("claude-control"));QVERIFY(p.baseUrl.isEmpty());
        } else if(p.isCursorHarness()) {
            // Optional native Cursor ACP adapter (provider-parity-next-stage), not an upstream route.
            QCOMPARE(entry.id,QString("cursor"));QCOMPARE(p.model,QString("default"));
            QCOMPARE(p.protocol,QString("cursor-acp"));QVERIFY(p.baseUrl.isEmpty());
        } else QVERIFY(p.model.isEmpty());
        QVERIFY(p.isExternalHarness()||ProviderProtocol::supports(p));
    }
    QCOMPARE(ids.size(),24);QVERIFY(ids.contains("claude-code"));QVERIFY(ids.contains("opencode"));QVERIFY(ids.contains("cursor"));QVERIFY(!ids.contains("codex-stagewise"));QVERIFY(ProviderCatalog::instantiate("missing").id.isEmpty());
 }
 void requestWire_data(){QTest::addColumn<QString>("id");for(const auto& e:ProviderCatalog::all())if(e.kind=="api"&&e.id!="custom")QTest::newRow(qPrintable(e.id))<<e.id;}
 void requestWire(){
    QFETCH(QString,id);auto p=ProviderCatalog::instantiate(id);p.model="fixture-model";if(id!="ollama")p.apiKey="fixture-not-a-live-key";
    const auto request=ProviderProtocol::prepare(p,{QJsonObject{{"role","user"},{"content","Hello"}}},"System",{});
    const auto url=request.request.url();const QUrl base(p.baseUrl);QCOMPARE(url.scheme(),base.scheme());QCOMPARE(url.host(),base.host());
    if(p.protocol=="anthropic"){QCOMPARE(url.path(),base.path()+"/messages");QCOMPARE(request.request.rawHeader("x-api-key"),p.apiKey.toUtf8());QCOMPARE(request.request.rawHeader("anthropic-version"),QByteArray("2023-06-01"));QVERIFY(request.request.rawHeader("Authorization").isEmpty());}
    else if(p.protocol=="google"){QCOMPARE(url.path(),base.path()+"/models/fixture-model:streamGenerateContent");QCOMPARE(request.request.rawHeader("x-goog-api-key"),p.apiKey.toUtf8());QVERIFY(request.request.rawHeader("Authorization").isEmpty());}
    else {QCOMPARE(url.path(),base.path()+(p.protocol=="openai-responses"?"/responses":"/chat/completions"));QCOMPARE(request.request.rawHeader("Authorization"),id=="ollama"?QByteArray{}:QByteArray("Bearer fixture-not-a-live-key"));QVERIFY(request.request.rawHeader("x-api-key").isEmpty());}
    QVERIFY(!url.toString().contains(p.apiKey.isEmpty()?"not-present":p.apiKey));QVERIFY(!QJsonDocument(request.body).toJson().contains("fixture-not-a-live-key"));
    if(p.protocol=="google")QCOMPARE(QUrlQuery(url).queryItemValue("alt"),QString("sse"));else QVERIFY(request.body.value("stream").toBool());
 }
 void uniqueInstancesDoNotCopyCredentials(){auto first=ProviderCatalog::instantiate("deepseek-api");first.apiKey="fixture-secret";first.model="model";const auto second=ProviderCatalog::instantiate("deepseek-api");QVERIFY(first.id!=second.id);QVERIFY(second.apiKey.isEmpty());QVERIFY(second.model.isEmpty());}
 void provenanceDistinguishesSharedEndpointsAndDoesNotGuessLegacy(){
    auto p=ProviderCatalog::instantiate("minimax-plan");QVERIFY(ProviderCatalog::matches(p,*ProviderCatalog::find("minimax-plan")));QVERIFY(!ProviderCatalog::matches(p,*ProviderCatalog::find("minimax-api")));p.templateId.clear();QVERIFY(!ProviderCatalog::matches(p,*ProviderCatalog::find("minimax-plan")));p.templateId="minimax-plan";p.baseUrl="https://example.invalid/v1";ProviderCatalog::clearChangedProvenance(p);QVERIFY(p.templateId.isEmpty());
 }
 void configurableTokenPlanKeepsExplicitTypeOnlyForValidRoute(){auto p=ProviderCatalog::instantiate("qwen-token-plan");p.baseUrl="https://regional.example.invalid/compatible-mode/v1";ProviderCatalog::clearChangedProvenance(p);QCOMPARE(p.templateId,QString("qwen-token-plan"));p.baseUrl="https://user:password@regional.example.invalid/v1";ProviderCatalog::clearChangedProvenance(p);QVERIFY(p.templateId.isEmpty());}
 void templateIdRoundTripAndInvalidPreservesFile(){
    QTemporaryDir dir;const auto path=dir.filePath("providers.json");auto p=ProviderCatalog::instantiate("qwen-token-plan");p.apiKey="fixture-only";QString error;QVERIFY2(ProviderStore::save(path,{p},&error),qPrintable(error));const auto loaded=ProviderStore::load(path,&error);QCOMPARE(loaded.size(),1);QCOMPARE(loaded.first().templateId,p.templateId);QCOMPARE(loaded.first().apiKey,p.apiKey);const auto original=read(path);QVERIFY(!original.contains("fixture-only"));for(const auto& invalid:QStringList{QString(81,'a'),"../file","Upper",QString("a")+QChar(1)}){p.templateId=invalid;QVERIFY(!ProviderStore::save(path,{p},&error));QCOMPARE(read(path),original);}
    auto object=QJsonDocument::fromJson(original).object();auto records=object["providers"].toArray();auto record=records.first().toObject();record.remove("templateId");records[0]=record;object["providers"]=records;QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(object).toJson());file.close();const auto legacy=ProviderStore::load(path,&error);QCOMPARE(legacy.size(),1);QVERIFY(legacy.first().templateId.isEmpty());QCOMPARE(legacy.first().apiKey,QString("fixture-only"));record["templateId"]=42;records[0]=record;object["providers"]=records;QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(object).toJson());file.close();QVERIFY(ProviderStore::load(path,&error).isEmpty());QVERIFY(!error.isEmpty());
 }
 void filterAndSelectExactEntry(){AddProviderDialog dialog;expose(dialog);auto* search=dialog.findChild<QLineEdit*>("providerSearch");QVERIFY(search);search->setText("QwEn");QTRY_COMPARE(dialog.visibleTemplateIds(),QStringList({"qwen-plan","qwen-token-plan","alibaba-api"}));auto* button=dialog.findChild<QPushButton*>("providerChoice_qwen-token-plan");QVERIFY(button&&button->isVisible());button->click();QCOMPARE(dialog.result(),int(QDialog::Accepted));QCOMPARE(dialog.selectedTemplateId(),QString("qwen-token-plan"));}
 void emptySearchPlainTextAndCancel(){AddProviderDialog dialog;expose(dialog);auto* search=dialog.findChild<QLineEdit*>("providerSearch");search->setText("<img src='file:///x'>");auto* empty=dialog.findChild<QLabel*>("providerSearchEmpty");QVERIFY(empty&&empty->isVisible());QCOMPARE(empty->textFormat(),Qt::PlainText);QVERIFY(dialog.visibleTemplateIds().isEmpty());dialog.findChild<QPushButton*>("cancelAddProvider")->click();QCOMPARE(dialog.result(),int(QDialog::Rejected));QVERIFY(dialog.selectedTemplateId().isEmpty());}
 void searchEnterDoesNotChooseOrCancelAndFocusedCardActivates(){AddProviderDialog dialog;expose(dialog);auto* search=dialog.findChild<QLineEdit*>("providerSearch");search->setText("API");QTest::keyClick(search,Qt::Key_Return);QVERIFY(dialog.isVisible());QVERIFY(dialog.selectedTemplateId().isEmpty());auto* card=dialog.findChild<QPushButton*>("providerChoice_google-api");card->setFocus();QTest::keyClick(card,Qt::Key_Return);QCOMPARE(dialog.selectedTemplateId(),QString("google-api"));}
 void oneColumnAt350AndTwoAt512(){AddProviderDialog dialog;expose(dialog,350);auto* a=dialog.findChild<QPushButton*>("providerChoice_glm-coding-plan");auto* b=dialog.findChild<QPushButton*>("providerChoice_kimi-plan");QTRY_VERIFY(a->y()<b->y());QCOMPARE(a->x(),b->x());auto* scroll=dialog.findChild<QScrollArea*>("providerChoicesScroll");QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);dialog.resize(512,455);QTRY_COMPARE(a->y(),b->y());QVERIFY(a->x()<b->x());QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);}
 void customEndpointCardUsesFullSectionWidth_data(){QTest::addColumn<int>("width");QTest::addColumn<bool>("filtered");for(int width:{350,512})for(bool filtered:{false,true})QTest::newRow(qPrintable(QString("%1-%2").arg(width).arg(filtered?"filtered":"all")))<<width<<filtered;}
 void customEndpointCardUsesFullSectionWidth(){
    QFETCH(int,width);QFETCH(bool,filtered);AddProviderDialog dialog;expose(dialog,width);if(filtered)dialog.findChild<QLineEdit*>("providerSearch")->setText("custom");
    auto* card=dialog.findChild<QPushButton*>("providerChoice_custom");QVERIFY(card&&!card->isHidden());
    QTRY_VERIFY2(card->width()>=card->parentWidget()->width()-2,qPrintable(QString("Custom card width %1; section width %2.").arg(card->width()).arg(card->parentWidget()->width())));
    QCOMPARE(card->x(),0);auto* scroll=dialog.findChild<QScrollArea*>("providerChoicesScroll");QCOMPARE(scroll->horizontalScrollBar()->maximum(),0);
 }
 void choosingCanDestroyDialog(){auto* dialog=new AddProviderDialog;QPointer<AddProviderDialog> guard(dialog);expose(*dialog);connect(dialog,&QDialog::accepted,dialog,[dialog]{delete dialog;});dialog->findChild<QPushButton*>("providerChoice_custom")->click();QVERIFY(!guard);}
 void lightDarkLightRenders(){
    auto* parent=new QWidget;QPalette palette=parent->palette();palette.setColor(QPalette::Base,QColor("#fdfdfd"));parent->setPalette(palette);AddProviderDialog dialog({},parent);expose(dialog);auto capture=dialog.grab().toImage();QCOMPARE(capture.pixelColor(3,3),QColor("#fdfdfd"));palette.setColor(QPalette::Base,QColor("#21242b"));dialog.setPalette(palette);QTest::qWait(20);capture=dialog.grab().toImage();QCOMPARE(capture.pixelColor(3,3),QColor("#21242b"));palette.setColor(QPalette::Base,QColor("#fdfdfd"));dialog.setPalette(palette);QTest::qWait(20);QCOMPARE(dialog.grab().toImage().pixelColor(3,3),QColor("#fdfdfd"));dialog.setParent(nullptr);delete parent;
 }
 void initialDarkOwnerAndOwnerPaletteUpdates(){
    QWidget owner;auto palette=owner.palette();palette.setColor(QPalette::Base,QColor("#21242b"));owner.setPalette(palette);AddProviderDialog dialog({},&owner);expose(dialog,350);QCOMPARE(dialog.grab().toImage().pixelColor(3,3),QColor("#21242b"));palette.setColor(QPalette::Base,QColor("#fdfdfd"));owner.setPalette(palette);QTest::qWait(20);QCOMPARE(dialog.grab().toImage().pixelColor(3,3),QColor("#fdfdfd"));
 }
};
int main(int argc,char** argv){QApplication app(argc,argv);initializeAppStyle(app);ProviderPickerTest test;return QTest::qExec(&test,argc,argv);}
#include "test_provider_picker.moc"
