#include "ProviderStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#endif

namespace ade {
namespace {
constexpr qint64 MaxConfigBytes = 4 * 1024 * 1024;
QByteArray protect(const QByteArray &plain, bool encrypt, bool *ok) {
    *ok = false;
    if (plain.isEmpty()) { *ok = true; return {}; }
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))};
    DATA_BLOB output{};
    const BOOL success = encrypt
        ? CryptProtectData(&input, L"ADE provider credential", nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!success) return {};
    QByteArray result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    *ok = true;
    return result;
#else
    Q_UNUSED(encrypt)
    return {};
#endif
}
}

QList<ProviderConfig> ProviderStore::load(const QString &path, QString *error) {
    if (error) error->clear();
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    if (file.size() > MaxConfigBytes) {
        if (error) *error = QStringLiteral("Provider configuration exceeds size limit.");
        return {};
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()
        || document.object()["version"].toInt() != 1
        || !document.object()["providers"].isArray()) {
        if (error) *error = QStringLiteral("Invalid provider configuration format.");
        return {};
    }
    QList<ProviderConfig> result;
    for (const auto value : document.object()["providers"].toArray()) {
        if (!value.isObject()) {
            if (error) *error = QStringLiteral("Invalid provider record: expected an object.");
            return {};
        }
        const auto object = value.toObject();
        for (const auto &field : {"id", "name", "baseUrl", "model", "protocol", "encryptedApiKey"}) {
            if (!object[field].isString()) {
                if (error) *error = QStringLiteral("Invalid provider record: missing or incorrectly typed field %1.").arg(field);
                return {};
            }
        }
        for (const auto &field : {"reasoningEffort", "thinkingMode"}) {
            if (object.contains(field) && !object[field].isString()) {
                if (error) *error = QStringLiteral("Invalid provider option field.");
                return {};
            }
        }
        for (const auto &field : {"thinkingBudget", "maxOutputTokens", "contextWindowTokens"}) {
            if (object.contains(field) && (!object[field].isDouble() || object[field].toDouble() < 0
                                          || object[field].toDouble() != object[field].toInt())) {
                if (error) *error = QStringLiteral("Invalid numeric provider option field.");
                return {};
            }
        }
        ProviderConfig provider = ProviderConfig::fromPublicJson(object);
        if (provider.contextWindowTokens > 100000000) {
            if (error) *error = QStringLiteral("Provider context window exceeds its limit.");
            return {};
        }
        if (provider.id.isEmpty() || std::any_of(result.begin(), result.end(), [&provider](const ProviderConfig &existing) { return existing.id == provider.id; })) {
            if (error) *error = QStringLiteral("Provider IDs must be nonempty and unique.");
            return {};
        }
        bool ok = false;
        const auto secret = QByteArray::fromBase64Encoding(object["encryptedApiKey"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!secret) {
            if (error) *error = QStringLiteral("Invalid encrypted credential encoding.");
            return {};
        }
        provider.apiKey = QString::fromUtf8(protect(secret.decoded, false, &ok));
        if (!ok) {
            if (error) *error = QStringLiteral("Cannot decrypt a provider key for this Windows user.");
            return {};
        }
        result.append(provider);
    }
    return result;
}

bool ProviderStore::save(const QString &path, const QList<ProviderConfig> &providers,
                         QString *error) {
    if (error) error->clear();
    QJsonArray array;
    QSet<QString> ids;
    qint64 approximateBytes = 0;
    for (const auto &provider : providers) {
        if (provider.id.isEmpty() || ids.contains(provider.id)) {
            if (error) *error = QStringLiteral("Provider IDs must be nonempty and unique.");
            return false;
        }
        ids.insert(provider.id);
        if (provider.thinkingBudget < 0 || provider.maxOutputTokens < 0 || provider.contextWindowTokens < 0 || provider.contextWindowTokens > 100000000) {
            if (error) *error = QStringLiteral("Provider numeric options must be nonnegative.");
            return false;
        }
        approximateBytes += QJsonDocument(provider.publicJson()).toJson(QJsonDocument::Compact).size() + provider.apiKey.toUtf8().size();
        if (approximateBytes > MaxConfigBytes) {
            if (error) *error = QStringLiteral("Provider configuration exceeds size limit.");
            return false;
        }
        bool ok = false;
        const auto encrypted = protect(provider.apiKey.toUtf8(), true, &ok);
        if (!ok) {
            if (error) *error = QStringLiteral("OS credential encryption is unavailable.");
            return false;
        }
        auto object = provider.publicJson();
        object["encryptedApiKey"] = QString::fromLatin1(encrypted.toBase64());
        array.append(object);
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"providers", array}})
                                 .toJson(QJsonDocument::Indented);
    if (bytes.size() > MaxConfigBytes) {
        if (error) *error = QStringLiteral("Provider configuration exceeds size limit.");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create the provider configuration directory.");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QList<ProviderConfig> ProviderStore::importKeyFile(const QString &path, QString *error) {
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        if (error) *error = QStringLiteral("Cannot read key file, or it exceeds the 1 MiB limit.");
        return {};
    }
    struct Known { QString name, url, model; };
    // Source: pinned Stagewise official-api provider registry and validation model
    // map; Gemini compatibility endpoint is documented by Google. Model values
    // are starting selections, not claims about this account's availability.
    const QMap<QString, Known> known = {
        {"OPENAI", {"OpenAI", "https://api.openai.com/v1", "gpt-4o-mini"}},
        {"GEMINI", {"Google Gemini", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-flash"}},
        {"XAI", {"xAI", "https://api.x.ai/v1", "grok-3-mini"}},
        {"DEEPSEEK", {"DeepSeek", "https://api.deepseek.com/v1", "deepseek-chat"}},
        {"MOONSHOT", {"Moonshot AI", "https://api.moonshot.ai/v1", "kimi-k2.6"}},
        {"QWEN", {"Qwen", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "qwen-turbo"}},
        {"ZAI", {"Z.AI", "https://api.z.ai/api/paas/v4", "glm-4.5-flash"}},
        {"MINIMAX", {"MiniMax", "https://api.minimax.io/v1", "minimax-m2.7"}},
        // Official endpoints recorded in audit/provider-external-endpoints.json.
        // No model is guessed: these require account discovery before generation.
        {"NVIDIA", {"NVIDIA", "https://integrate.api.nvidia.com/v1", ""}},
        {"YOU", {"You.com", "https://api.you.com/v1", ""}},
        {"SAKANA", {"Sakana AI", "https://api.sakana.ai/v1", ""}},
        {"THEROUTER", {"TheRouter", "https://api.therouter.ai/v1", ""}},
        {"PERPLEXITY", {"Perplexity", "https://api.perplexity.ai/v1", ""}}
    };
    const QRegularExpression entry(QStringLiteral("^(?:export\\s+)?([A-Z][A-Z0-9_]*)_API_KEY\\s*[:=]\\s*(.*?)\\s*$"));
    QMap<QString, ProviderConfig> imported;
    QStringList needEndpoints;
    QStringList needModels;
    for (auto line : QString::fromUtf8(file.readAll()).split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("```")) continue;
        const auto match = entry.match(line);
        if (!match.hasMatch()) continue;
        const auto label = match.captured(1);
        QString secret = match.captured(2);
        if (secret.size() >= 2 && ((secret.startsWith('"') && secret.endsWith('"'))
                                  || (secret.startsWith('\'') && secret.endsWith('\''))))
            secret = secret.mid(1, secret.size() - 2);
        if (secret.isEmpty()) continue;
        ProviderConfig provider;
        provider.id = label.toLower();
        provider.name = label;
        provider.apiKey = secret;
        if (known.contains(label)) {
            const auto info = known.value(label);
            provider.name = info.name;
            provider.baseUrl = info.url;
            provider.model = info.model;
            if (label == "PERPLEXITY") provider.protocol = "openai-responses";
            if (provider.model.isEmpty()) needModels.append(label);
        } else needEndpoints.append(label);
        imported.insert(provider.id, provider);
    }
    if (error) {
        if (imported.isEmpty()) *error = QStringLiteral("No PROVIDER_API_KEY entries were found.");
        else if (!needEndpoints.isEmpty()) *error = QStringLiteral("API endpoint and model must be configured for: ")
            + needEndpoints.join(", ") + QStringLiteral(". Their keys were imported but have not been sent anywhere.");
        if (!needModels.isEmpty()) {
            if (!error->isEmpty()) *error += ' ';
            *error += QStringLiteral("Discover and select an available model for: ") + needModels.join(", ") + '.';
        }
    }
    return imported.values();
}

} // namespace ade
