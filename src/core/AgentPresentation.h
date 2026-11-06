#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

namespace ade {
// Presentation data is deliberately independent of the provider-native payload.
// All mutations happen during engine events; snapshot()/history() only read.
class AgentPresentation {
public:
    static constexpr int MaximumParts = 20000;
    void begin();
    bool empty() const { return m_parts.isEmpty(); }
    QString id() const { return m_id; }
    bool limitExceeded() const { return m_limitExceeded; }
    void text(const QString& type,const QString& delta,const QString& key={});
    void setText(const QString& type,const QString& text,const QString& key);
    bool hasKey(const QString& key) const { return m_keys.contains(key); }
    void tool(int index,const QString& callId,const QString& name,const QString& arguments);
    void closePart(const QString& key,const QString& state="done");
    void toolState(const QString& callId,const QString& state);
    QString toolOutput(const QString& callId) const;
    bool setToolOutput(const QString& callId,const QString& output);
    void finish(const QString& state="done");
    void error(const QString& text,const QString& state="error");
    QJsonObject metadata(bool uiOnly=false) const;
    QJsonObject row(bool provisional) const;
    static QJsonArray history(const QJsonArray& messages);
    static bool isUiOnly(const QJsonObject& message);
    static QJsonObject withMetadata(QJsonObject message,const QJsonObject& display);
private:
    int findPart(const QString& key) const;
    bool allowNewPart();
    void closeAt(int index,const QString& state);
    QJsonArray parts() const;
    QString m_id;
    QJsonArray m_parts;
    QHash<QString,int> m_keys;
    QHash<int,qint64> m_started;
    QElapsedTimer m_clock;
    bool m_limitExceeded = false;
};
}
