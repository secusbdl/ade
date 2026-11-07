#pragma once
#include "ModelConfig.h"
#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>

namespace ade {
// State rules ported from Stagewise 1.31.1 model-fallback-manager.ts.
class ModelFallback {
public:
    int resolve(const QString &preset, const QList<ModelConfig> &models, qint64 now = QDateTime::currentMSecsSinceEpoch()) {
        if (preset.isEmpty() || models.isEmpty()) { reset(); return 0; }
        QJsonArray entries;
        for (const auto &entry : models) {
            auto value = entry.provider.publicJson();
            value["hasThinkingOverride"] = entry.hasThinkingOverride;
            // Private custom values participate only through an in-memory
            // digest. publicJson deliberately contains no headers/options.
            value["customRuntimeDigest"] = QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(QJsonObject{{"request",entry.provider.customRequest},
                    {"overrides",entry.provider.requestOverrides}}).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256).toHex());
            entries.append(value);
        }
        const auto signature = QJsonDocument(entries).toJson(QJsonDocument::Compact);
        if (m_preset != preset || m_signature != signature) {
            reset(); m_preset = preset; m_signature = signature; return 0;
        }
        if (m_index > 0) {
            if (now - m_touched > 300000 || m_index >= models.size()) { m_index = 0; m_touched = 0; }
            else m_touched = now;
        }
        return m_index;
    }
    bool advance(int count, qint64 now = QDateTime::currentMSecsSinceEpoch()) {
        if (count <= 1) return false;
        if (m_index + 1 >= count) { m_index = 0; m_touched = 0; return false; }
        ++m_index; m_touched = now; return true;
    }
    int index() const { return m_index; }
    void reset() { m_index = 0; m_touched = 0; m_preset.clear(); m_signature.clear(); }
private:
    QString m_preset;
    QByteArray m_signature;
    int m_index = 0;
    qint64 m_touched = 0;
};
}
