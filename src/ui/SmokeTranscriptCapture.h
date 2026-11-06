#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <memory>

namespace ade {
class AgentEngine;
class ConversationView;
// Optional evidence for explicitly requested live smoke runs. Renders the real
// transcript widget without taking desktop focus; it is not a full-window GUI test.
QJsonValue redactSmokeEvidence(const QJsonValue& value,const QStringList& secrets);
class SmokeTranscriptCapture final:public QObject {
public:
    SmokeTranscriptCapture(AgentEngine* engine,QString directory,QStringList secrets);
    ~SmokeTranscriptCapture() override;
    void prepareFinish();
    QJsonObject finish();
private:
    void render();
    void queueFrame(const QString& kind);
    void flushFrames();
    void saveFrame(const QString& kind);
    QPointer<AgentEngine> m_engine;
    std::unique_ptr<ConversationView> m_view;
    QTimer m_timer;
    QString m_directory,m_error;
    QStringList m_secrets,m_recordedKinds,m_pendingKinds;
    QJsonArray m_frames;
    quint64 m_notifications=0,m_renders=0,m_textDeltas=0,m_reasoningDeltas=0;
    qint64 m_totalRenderNs=0,m_maxRenderNs=0;
    bool m_finished=false,m_capturePending=false,m_finalCaptured=false;
};
}
