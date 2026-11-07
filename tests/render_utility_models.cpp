#include "ui/AppStyle.h"
#include "ui/AppFonts.h"
#include "ui/SettingsPage.h"
#include "core/ProviderStore.h"
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QScrollArea>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QFile>
using namespace ade;
int main(int argc,char** argv){
 QApplication app(argc,argv);initializeAppStyle(app);const auto fonts=AppFonts::load();QFont body(fonts.body);body.setPixelSize(13);app.setFont(body);if(app.arguments().size()!=2)return 2;const auto output=QDir(app.arguments()[1]).absolutePath();if(!QDir().mkpath(output))return 3;
 QJsonArray evidence;
 for(const auto& theme:{"light","dark"})for(const int width:{620,350}){
  QTemporaryDir dir;ProviderConfig http;http.id="local-http";http.name="Local API";http.model="fixture-model";http.baseUrl="http://127.0.0.1:1/v1";
  ProviderConfig agent;agent.id="codex";agent.name="Codex";agent.model="fixture-codex";agent.kind="codex";agent.protocol="codex-app-server";agent.harnessExecutable="fixture.exe";
  if(!ProviderStore::save(dir.path()+"/providers.json",{http,agent}))return 4;
  SettingsPage page(dir.path());page.setPreference("personalization.appearance",theme);
  page.setPreference("utilityModels",QJsonObject{{"titleGeneration",QJsonArray{QJsonObject{{"providerInstanceId","local-http"},{"modelId","fixture-model"}},QJsonObject{{"providerInstanceId","codex"},{"modelId","fixture-codex"}}}},{"contextCompression",QJsonArray{QJsonObject{{"providerInstanceId","removed-provider"},{"modelId","saved-model"}}}}});
  page.selectSection("models-providers");page.resize(1100,900);page.show();app.processEvents();
  if(width==620&&!page.grab().save(output+"/settings-full-"+theme+".png"))return 5;
  // Render the actual Settings content column at narrow widths independently of
  // the existing fixed-width navigation. This is component geometry evidence.
  auto* content=page.findChild<QWidget*>("settingsContent");if(!content)return 6;
  content->setParent(nullptr);content->setStyleSheet(page.styleSheet());content->setAttribute(Qt::WA_DontShowOnScreen);content->setMinimumWidth(0);content->setMaximumWidth(width);content->resize(width,1300);content->show();app.processEvents();
  const auto name=QString("utility-content-%1-%2.png").arg(theme).arg(width);
  if(!content->grab().save(output+"/"+name))return 7;
  evidence.append(QJsonObject{{"file",name},{"requestedWidth",width},{"actualWidth",content->width()},{"height",content->height()},{"scope","Actual Settings content column, navigation excluded; no narrow full-window parity claim"}});
  delete content;
 }
 QFile report(output+"/index.json");if(!report.open(QIODevice::WriteOnly))return 8;report.write(QJsonDocument(evidence).toJson());return 0;
}
