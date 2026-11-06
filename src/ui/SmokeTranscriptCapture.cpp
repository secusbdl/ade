#include "SmokeTranscriptCapture.h"
#include "ConversationView.h"
#include "core/AgentEngine.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPixmap>
#include <QJsonArray>
#include <QJsonDocument>

namespace ade {
QJsonValue redactSmokeEvidence(const QJsonValue& value,const QStringList& secrets){
    if(value.isString()){
        auto text=value.toString();for(const auto& secret:secrets)if(!secret.isEmpty())text.replace(secret,"[REDACTED]");return text;
    }
    if(value.isArray()){
        QJsonArray result;for(const auto& item:value.toArray())result.append(redactSmokeEvidence(item,secrets));return result;
    }
    if(value.isObject()){
        QJsonObject result;const auto object=value.toObject();for(auto it=object.begin();it!=object.end();++it)result.insert(redactSmokeEvidence(it.key(),secrets).toString(),redactSmokeEvidence(it.value(),secrets));return result;
    }
    return value;
}
SmokeTranscriptCapture::SmokeTranscriptCapture(AgentEngine* engine,QString directory,QStringList secrets)
    :m_engine(engine),m_directory(std::move(directory)),m_secrets(std::move(secrets)){
    QDir target(m_directory);
    if((target.exists()&&!target.entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot).isEmpty())||!QDir().mkpath(m_directory)){
        m_error="Transcript capture needs a new or empty output directory.";return;
    }
    m_view=std::make_unique<ConversationView>();
    m_view->setAttribute(Qt::WA_DontShowOnScreen);m_view->setAttribute(Qt::WA_ShowWithoutActivating);
    m_view->resize(1000,800);m_view->show();
    m_timer.setSingleShot(true);m_timer.setInterval(16);
    connect(&m_timer,&QTimer::timeout,this,[this]{render();});
    connect(engine,&AgentEngine::displayChanged,this,[this]{if(m_finished)return;++m_notifications;if(!m_timer.isActive())m_timer.start();});
    connect(engine,&AgentEngine::textDelta,this,[this](const QString&){if(!m_finished)++m_textDeltas;});
    connect(engine,&AgentEngine::reasoningDelta,this,[this](const QString&){if(!m_finished)++m_reasoningDeltas;});
}
SmokeTranscriptCapture::~SmokeTranscriptCapture()=default;
void SmokeTranscriptCapture::saveFrame(const QString& kind){
    if(!m_view||m_frames.size()>=8||m_recordedKinds.contains(kind))return;
    m_recordedKinds.append(kind);
    const auto name=QString("%1-%2.png").arg(m_frames.size()+1,2,10,QChar('0')).arg(kind);
    const auto path=QDir(m_directory).filePath(name);
    const auto pixmap=m_view->grab();
    if(!pixmap.save(path)){m_error="Could not save a transcript frame.";return;}
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){m_error="Could not verify a saved transcript frame.";return;}
    m_frames.append(QJsonObject{{"path",name},{"sha256",QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex())},{"width",pixmap.width()},{"height",pixmap.height()},{"devicePixelRatio",pixmap.devicePixelRatio()}});
}
void SmokeTranscriptCapture::queueFrame(const QString& kind){
    if(m_recordedKinds.contains(kind)||m_pendingKinds.contains(kind))return;
    m_pendingKinds.append(kind);
    if(m_capturePending)return;
    m_capturePending=true;
    // ConversationView and its text documents settle geometry through queued
    // layout callbacks. Capture after those callbacks, without processing input
    // reentrantly or taking focus from the desktop.
    QTimer::singleShot(0,this,[this]{QTimer::singleShot(0,this,[this]{m_capturePending=false;flushFrames();});});
}
void SmokeTranscriptCapture::flushFrames(){
    const auto kinds=m_pendingKinds;m_pendingKinds.clear();
    for(const auto& kind:kinds)saveFrame(kind);
}
void SmokeTranscriptCapture::render(){
    if(!m_engine||!m_view)return;
    QElapsedTimer elapsed;elapsed.start();
    const auto rows=redactSmokeEvidence(m_engine->displayMessages(),m_secrets).toArray();
    m_view->setConversation("smoke-session",rows,m_engine->isBusy());
    const auto ns=elapsed.nsecsElapsed();++m_renders;m_totalRenderNs+=ns;m_maxRenderNs=qMax(m_maxRenderNs,ns);
    if(!rows.isEmpty())queueFrame("submitted");
    bool reasoning=false,text=false,tool=false;
    for(const auto& value:rows){const auto row=value.toObject();if(row["role"]!="assistant")continue;for(const auto& entry:row["parts"].toArray()){
        const auto part=entry.toObject();reasoning|=part["type"]=="reasoning"&&!part["text"].toString().isEmpty();
        text|=part["type"]=="text"&&!part["text"].toString().isEmpty();tool|=part["type"]=="tool";
    }}
    if(reasoning)queueFrame("reasoning");
    if(tool)queueFrame("tool");
    if(text)queueFrame("text");
}
void SmokeTranscriptCapture::prepareFinish(){
    if(m_finished)return;
    m_finished=true;m_timer.stop();render();
}
QJsonObject SmokeTranscriptCapture::finish(){
    prepareFinish();
    if(!m_finalCaptured){m_finalCaptured=true;flushFrames();saveFrame("completed");}
    return {{"mode","Hidden native ConversationView rendered from actual AgentEngine events; not a full-window GUI or end-to-end interaction test"},
        {"error",m_error},{"displayNotifications",double(m_notifications)},{"renderUpdates",double(m_renders)},
        {"textDeltas",double(m_textDeltas)},{"reasoningDeltas",double(m_reasoningDeltas)},
        {"meanSetConversationMs",m_renders?m_totalRenderNs/1000000.0/m_renders:0.0},{"maxSetConversationMs",m_maxRenderNs/1000000.0},
        {"timingScope","Projection, redaction and setConversation only; excludes deferred layout, raster capture and compositor"},{"frames",m_frames}};
}
}
