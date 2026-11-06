#include "core/ProviderService.h"
#include "core/ProviderStore.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

using namespace ade;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("ADE Provider Probe");
    QCommandLineParser parser;
    parser.setApplicationDescription("Native provider discovery and bounded validation; no credentials are accepted on the command line.");
    parser.addHelpOption();
    parser.addOption({"import-keys", "Read dotenv provider keys from this local file", "path"});
    parser.addOption({"provider", "Select an imported provider ID", "id"});
    parser.addOption({"base-url", "Explicit API base URL override", "url"});
    parser.addOption({"protocol", "Explicit protocol override", "protocol"});
    parser.addOption({"model", "Select this exact discovered model for validation", "model"});
    parser.addOption({"validate", "After discovery, make one bounded no-tool model validation invocation"});
    parser.addOption({"timeout", "Overall timeout in seconds", "seconds", "60"});
    parser.addOption({"report", "Write a credential-redacted JSON report", "path"});
    parser.process(app);
    if (parser.value("import-keys").isEmpty() || parser.value("provider").isEmpty() || parser.value("report").isEmpty()) {
        QTextStream(stderr) << "--import-keys, --provider, and --report are required.\n";
        return 2;
    }
    QString importError;
    const auto providers = ProviderStore::importKeyFile(parser.value("import-keys"), &importError);
    const auto selected = std::find_if(providers.begin(), providers.end(), [&](const ProviderConfig &provider) {
        return provider.id.compare(parser.value("provider"), Qt::CaseInsensitive) == 0;
    });
    if (selected == providers.end()) { QTextStream(stderr) << "Selected provider was not found in the key file.\n"; return 2; }
    auto provider = *selected;
    if (parser.isSet("base-url")) provider.baseUrl = parser.value("base-url");
    if (parser.isSet("protocol")) provider.protocol = parser.value("protocol");
    if (parser.isSet("model")) provider.model = parser.value("model");
    ProviderService service;
    QElapsedTimer elapsed; elapsed.start();
    QJsonObject discovery{{"success", false}, {"models", QJsonArray{}}};
    QJsonObject validation{{"attempted", false}, {"success", false}};
    bool completed = false;
    const auto sanitize = [&](QString text) {
        for (const auto &entry : providers) if (!entry.apiKey.isEmpty()) text.replace(entry.apiKey, "[REDACTED]");
        return text;
    };
    auto finish = [&](int code, const QString &error = QString()) {
        if (completed) return;
        completed = true; service.cancel();
        QJsonObject report{{"native", true}, {"provider", provider.publicJson()},
            {"discovery", discovery}, {"validation", validation}, {"error", sanitize(error)},
            {"elapsedMs", elapsed.elapsed()}, {"exitCode", code},
            {"scope", "Discovery reports API catalog availability; only an explicitly selected model may receive a bounded no-tool validation request."}};
        const auto reportPath = QFileInfo(parser.value("report")).absoluteFilePath();
        if (!QDir().mkpath(QFileInfo(reportPath).absolutePath())) { QTextStream(stderr) << "Cannot create report directory.\n"; app.exit(3); return; }
        QSaveFile file(reportPath);
        // The report has no credential fields. This final pass also strips any
        // credential a nonconforming provider might echo in model metadata/errors.
        const auto bytes = sanitize(QString::fromUtf8(QJsonDocument(report).toJson())).toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            QTextStream(stderr) << "Cannot save provider report.\n"; app.exit(3); return;
        }
        QTextStream(stdout) << "Provider discovery " << (discovery["success"].toBool() ? "succeeded" : "failed")
            << "; models=" << discovery["models"].toArray().size()
            << "; validation=" << (validation["attempted"].toBool() ? (validation["success"].toBool() ? "passed" : "failed") : "not requested") << '\n';
        app.exit(code);
    };
    QObject::connect(&service, &ProviderService::discoveryFailed, &app, [&](const QString &error) {
        discovery["error"] = sanitize(error); finish(1, error);
    });
    QObject::connect(&service, &ProviderService::modelsReady, &app, [&](const QJsonArray &models) {
        QJsonArray publicModels;
        for (const auto &value : models) {
            const auto object = value.toObject();
            publicModels.append(QJsonObject{{"id", object["id"]}, {"name", object["name"]},
                {"inputTokenLimit", object["inputTokenLimit"]}, {"outputTokenLimit", object["outputTokenLimit"]}});
        }
        discovery = {{"success", true}, {"models", publicModels}};
        if (!parser.isSet("validate")) { finish(0); return; }
        if (!parser.isSet("model") || provider.model.isEmpty()) { finish(4, "Validation requires an explicit --model selected from discovery."); return; }
        bool found = false;
        for (const auto &value : models) if (value.toObject()["id"].toString() == provider.model) found = true;
        if (!found) { finish(4, "Selected model was not returned by this provider's discovery endpoint; no generation request was sent."); return; }
        validation["attempted"] = true; validation["model"] = provider.model;
        service.test(provider);
    });
    QObject::connect(&service, &ProviderService::validationFinished, &app, [&](bool success, const QString &detail) {
        validation["success"] = success; validation["detail"] = sanitize(detail);
        finish(success ? 0 : 1, success ? QString() : detail);
    });
    QTimer::singleShot(qBound(5, parser.value("timeout").toInt(), 120) * 1000, &app, [&] { finish(1, "Overall provider probe deadline exceeded."); });
    QTimer::singleShot(0, &app, [&] { service.discover(provider); });
    return app.exec();
}
