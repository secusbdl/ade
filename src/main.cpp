#include "ui/MainWindow.h"
#include "ui/InstanceCoordinator.h"
#include "ui/AppStyle.h"
#include "ui/ContextPreferences.h"
#include "ui/SettingsPage.h"
#include "ui/ModelSelection.h"
#include "ui/SmokeTranscriptCapture.h"
#include "core/AgentEngine.h"
#include "harness/QuestionMcpStdio.h"
#include "core/ProviderStore.h"
#include "plugins/PluginCatalog.h"
#include "browser/browserwidget.h"
#include "browser/BrowserToolBridge.h"
#include "runtime/AgentToolHost.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <memory>
using namespace ade;
namespace {
bool smokePreferences(const QString& directory, QJsonObject* preferences, QString* error) {
    *preferences = SettingsPage::defaultPreferences();
    QFile file(directory + "/preferences.json");
    if (!file.exists()) return true;
    constexpr qint64 limit = 2 * 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) {
        *error = "Preferences could not be read within their size limit.";
        return false;
    }
    const auto bytes = file.read(limit + 1);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError
        || parseError.error != QJsonParseError::NoError || !document.isObject()
        || document.object()["schemaVersion"] != QJsonValue(1)
        || !document.object()["preferences"].isObject()) {
        *error = "Preferences have an unsupported or invalid format.";
        return false;
    }
    *preferences = document.object()["preferences"].toObject();
    return true;
}
bool configureSmokePlugins(AgentToolHost& host, PluginCatalog& catalog, const QString& directory,
                           const QString& workspace, const QString& prompt, QString* error) {
    QJsonObject preferences;
    if (!smokePreferences(directory, &preferences, error)
        || !catalog.configureAgent(*host.network(), host.agentId(), directory, preferences, error))
        return false;
    const auto plugins = PluginCatalog::context(directory, preferences, error);
    if (!error->isEmpty()) return false;
    auto options = contextOptionsFor(preferences, workspace, QDir::homePath());
    options.pluginsDirectory = plugins.pluginsDirectory;
    options.disabledPluginIds = plugins.disabledPluginIds;
    ContextLoader loader;
    const auto snapshot = loader.load(options);
    QSet<QString> requested;
    const QRegularExpression links("\\[[^\\]]*\\]\\(slash:([^\\)]+)\\)");
    auto matches = links.globalMatch(prompt);
    while (matches.hasNext())
        requested.insert(QUrl::fromPercentEncoding(matches.next().captured(1).toUtf8()));
    const auto command = prompt.trimmed().section(QRegularExpression("\\s+"), 0, 0);
    for (const auto& skill : snapshot.skills)
        if (skill.userInvocable && (requested.contains(skill.id)
            || (command.startsWith('/') && (skill.id == command.mid(1)
                || skill.name.compare(command.mid(1), Qt::CaseInsensitive) == 0))))
            options.invokedSkillIds.append(skill.id);
    host.configureContext(options);
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2 && QByteArray(argv[1])=="--question-mcp-stdio")
        return runQuestionMcpStdio(argc,argv,QuestionForm::toolDefinition()["function"].toObject()["parameters"].toObject());
    QApplication app(argc,argv);app.setApplicationName("ADE");app.setOrganizationName("ADE");app.setApplicationVersion("0.1.0");
    initializeAppStyle(app);
    QCommandLineParser parser;parser.setApplicationDescription("ADE — native Qt agentic development environment");parser.addHelpOption();parser.addVersionOption();
    parser.addOption({"data-dir","Use an isolated data directory","directory"});
    parser.addOption({"import-keys","Import API keys into encrypted provider storage","file"});
    parser.addOption({"workspace","Connect a workspace directory","directory"});
    parser.addOption({"prompt","Send a prompt after startup","text"});
    parser.addOption({"provider","Select provider ID/name for a smoke session","provider"});
    parser.addOption({"model","Override model for a smoke session","model"});
    parser.addOption({"reasoning","Set reasoning effort for a smoke session","effort"});
    parser.addOption({"thinking","Set thinking mode for a smoke session","mode"});
    parser.addOption({"smoke","Run the actual native agent without showing the main window"});
    parser.addOption({"browser-url","Provide a real native browser to the smoke agent","url"});
    parser.addOption({"always-allow","Allow workspace tools for this smoke session"});
    parser.addOption({"timeout","Smoke timeout in seconds","seconds","120"});
    parser.addOption({"report","Write smoke evidence (without credentials)","path"});
    parser.addOption({"attach","Attach a local file to a smoke message (repeatable)","file"});
    parser.addOption({"max-output","Bound output tokens for a smoke session","tokens"});
    parser.addOption({"capture-transcript","Save bounded native transcript renders during a smoke run (new/empty directory)","directory"});
    parser.addPositionalArgument("urls","http(s) URLs to open","[urls...]");
    parser.process(app);
    const auto directory=parser.isSet("data-dir")?QDir(parser.value("data-dir")).absolutePath():QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if(!QDir().mkpath(directory)){QTextStream(stderr)<<"Cannot create application data directory\n";return 2;}
    if(parser.isSet("smoke")) {
        QString error;auto providers=ProviderStore::load(directory+"/providers.json",&error);
        if(parser.isSet("import-keys"))providers=ProviderStore::importKeyFile(parser.value("import-keys"),&error);
        auto selected=std::find_if(providers.begin(),providers.end(),[&](const ProviderConfig& p){return parser.value("provider").isEmpty()||p.id.compare(parser.value("provider"),Qt::CaseInsensitive)==0||p.name.compare(parser.value("provider"),Qt::CaseInsensitive)==0;});
        if(selected==providers.end()){QTextStream(stderr)<<"Requested provider is not configured. "<<error<<"\n";return 2;}
        ModelSelection selection{selected->id,parser.isSet("model")?parser.value("model"):selected->model,{},{},{}};
        if(parser.isSet("reasoning"))selection.overrides["reasoningEffort"]=parser.value("reasoning");
        if(parser.isSet("thinking"))selection.overrides["thinkingMode"]=parser.value("thinking");
        if(parser.isSet("max-output"))selection.overrides["maxOutputTokens"]=qBound(16,parser.value("max-output").toInt(),32768);
        ProviderConfig provider;
        if(!selection.resolve(providers,&provider)){
            QTextStream(stderr)<<"Requested model is unavailable for this provider instance. No request was sent.\n";
            return 2;
        }
        QStringList smokeSecrets;
        for(const auto& configured:providers){
            if(!configured.apiKey.isEmpty())smokeSecrets.append(configured.apiKey);
            for(const auto& definition:configured.customModels)
                for(const auto& value:definition.headers)
                    if(!value.toString().isEmpty())smokeSecrets.append(value.toString());
        }
        AgentEngine engine;engine.setProvider(provider);engine.setWorkspace(parser.value("workspace"));engine.setAlwaysAllow(parser.isSet("always-allow"));
        std::unique_ptr<BrowserWidget> browser;
        BrowserToolBridge browserTools;
        AgentToolHost toolHost(&engine,&browserTools,"smoke-session",directory);toolHost.setWorkspace(parser.value("workspace"));
        PluginCatalog pluginCatalog;
        if (!configureSmokePlugins(toolHost, pluginCatalog, directory, engine.workspace(), parser.value("prompt"), &error)) {
            QTextStream(stderr) << "Cannot configure smoke plugins: " << error << '\n';
            return 2;
        }
        QJsonArray attachments;AttachmentStore attachmentStore(directory);AttachmentMounts mounts;
        if(!engine.workspace().isEmpty())mounts.insert("ws1",engine.workspace());
        for(const auto& path:parser.values("attach")){
            const auto ref=attachmentStore.importFile("smoke-session",QFileInfo(path).absoluteFilePath(),mounts,&error);
            if(!error.isEmpty()){QTextStream(stderr)<<"Cannot attach file: "<<error<<'\n';return 2;}
            attachments.append(ref.toJson());
        }
        const auto attachmentMetadata=toolHost.captureAttachments(attachments,&error);
        if(!error.isEmpty()){QTextStream(stderr)<<"Cannot prepare attachments: "<<error<<'\n';return 2;}
        if(parser.isSet("browser-url")){
            browser=std::make_unique<BrowserWidget>();browser->setProfileDirectory(directory+"/browser");browser->setAttribute(Qt::WA_DontShowOnScreen);browser->resize(1024,768);
            browserTools.setActiveTab(browserTools.registerTab(browser.get()));
            browser->navigate(QUrl(parser.value("browser-url")));browser->show();
        }
        QString response,failure;QJsonArray tools;bool done=false;QElapsedTimer elapsed;elapsed.start();
        std::unique_ptr<SmokeTranscriptCapture> capture;
        if(parser.isSet("capture-transcript"))capture=std::make_unique<SmokeTranscriptCapture>(&engine,parser.value("capture-transcript"),smokeSecrets);
        QObject::connect(&engine,&AgentEngine::textDelta,&app,[&](const QString& text){response+=text;});
        QObject::connect(&engine,&AgentEngine::toolStarted,&app,[&](const QString& id,const QString& name,const QJsonObject&){tools.append(QJsonObject{{"id",id},{"name",name}});});
        QObject::connect(&engine,&AgentEngine::approvalRequired,&app,[&](const QString& id,const QString&,const QJsonObject&){engine.approveTool(id,false);});
        QObject::connect(&engine,&AgentEngine::failed,&app,[&](const QString& message){failure=message;});
        auto finish=[&](bool timeout){
            if(done)return;
            done=true;
            if(timeout){failure="Session timed out";engine.cancel();}
            const auto writeReport=[&]{
            QJsonObject report{{"native",true},{"provider",provider.publicJson()},{"actualModel",engine.actualModel()},
                {"actualModelMeaning",provider.isClaudeHarness()?"requested CLI alias":"selected provider model"},{"modelTelemetry",engine.modelTelemetry()},
                {"success",failure.isEmpty()&&!response.isEmpty()},{"error",failure},{"response",response},
                {"tools",tools},{"messages",engine.messages()},{"elapsedMs",elapsed.elapsed()}};
            if(capture)report["transcriptCapture"]=capture->finish();
            report=redactSmokeEvidence(report,smokeSecrets).toObject();
            if(parser.isSet("report")){
                QSaveFile file(parser.value("report"));
                const auto bytes=QJsonDocument(report).toJson();
                if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()){
                    failure="Cannot save the smoke evidence report.";
                    QTextStream(stderr)<<failure<<'\n';
                }
            }
            QTextStream(stdout)<<"Native session: "<<(failure.isEmpty()?"finished":"failed")<<"; tools="<<tools.size()<<"; responseChars="<<response.size()<<"\n";
            app.exit(failure.isEmpty()&&!response.isEmpty()?0:1);
            };
            if(capture){
                capture->prepareFinish();
                QTimer::singleShot(0,&app,[&app,writeReport]{QTimer::singleShot(0,&app,writeReport);});
            }else writeReport();
        };
        QObject::connect(&engine,&AgentEngine::finished,&app,[&]{finish(false);});
        QTimer::singleShot(qBound(5,parser.value("timeout").toInt(),600)*1000,&app,[&]{finish(true);});
        auto submit=[&]{engine.submitWithAttachments(parser.value("prompt"),attachmentMetadata["attachments"].toArray(),attachmentMetadata["pathReferences"].toObject(),attachmentMetadata["pathMetadata"].toObject());};
        if(browser){auto started=std::make_shared<bool>(false);QObject::connect(browser.get(),&BrowserWidget::loadingChanged,&app,[&,started](bool loading){if(!loading&&!*started){*started=true;submit();}});}else QTimer::singleShot(0,&app,submit);
        return app.exec();
    }
    // One GUI instance per data directory; isolated --data-dir profiles and
    // --smoke runs (above) are independent. Only http(s) URLs are forwarded.
    InstanceCoordinator coordinator(directory);
    const InstanceRequest startup{InstanceCoordinator::filterUrls(parser.positionalArguments()),QDir::currentPath()};
    const bool exclusive=parser.isSet("import-keys")||parser.isSet("prompt")||parser.isSet("workspace");
    const bool quiet=QGuiApplication::platformName()=="offscreen"||QGuiApplication::platformName()=="minimal";
    const auto report=[quiet](const QString& message){
        QTextStream(stderr)<<message<<'\n';
        // A WIN32 executable started from Explorer has no visible stderr.
        if(!quiet)QMessageBox::warning(nullptr,"ADE",message);
    };
    QString instanceError;
    const auto role=coordinator.acquire(startup,5000,&instanceError,exclusive);
    if(role==InstanceCoordinator::Role::Refused){
        report("ADE is already running for this data directory; close it before using --import-keys/--prompt/--workspace.");
        return 3;
    }
    if(role==InstanceCoordinator::Role::Forwarded)return 0;
    // Lock-file errors other than a running owner continue uncoordinated (a
    // logged warning); Failed means an owner holds the lock but never answered.
    if(role==InstanceCoordinator::Role::Failed){report(instanceError);return 4;}
    MainWindow window(directory);
    QObject::connect(&coordinator,&InstanceCoordinator::requestReceived,&window,&MainWindow::handleInstanceRequest);
    // After the window accepts close, later launches wait for the lock instead.
    QObject::connect(&app,&QGuiApplication::lastWindowClosed,&coordinator,&InstanceCoordinator::setClosing);
    QObject::connect(&app,&QCoreApplication::aboutToQuit,&coordinator,&InstanceCoordinator::setClosing);
    if(parser.isSet("import-keys"))window.importKeys(parser.value("import-keys"));
    if(parser.isSet("workspace"))window.connectWorkspace(parser.value("workspace"));
    window.show();
    // This launch's own URLs: no activation request or taskbar alert.
    if(!startup.urls.isEmpty())window.openStartupUrls(startup.urls);
    coordinator.markReady();
    if(parser.isSet("prompt"))QTimer::singleShot(500,&window,[&]{window.submitPrompt(parser.value("prompt"));});
    return app.exec();
}
