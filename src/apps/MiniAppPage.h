#pragma once
#include "AppResourceStore.h"
#include <QJsonValue>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

namespace ade {
class BrowserWidget;

// The application chrome stays native. This page renders an isolated plugin's
// HTML inside a minimal compatibility iframe for upstream parent.postMessage.
class MiniAppPage final: public QWidget {
    Q_OBJECT
public:
    MiniAppPage(const QUrl& appBaseUrl,const QString& appDirectory,
                const QString& profileDirectory,QWidget* parent=nullptr);
    ~MiniAppPage() override;
    BrowserWidget* browser() const {return m_browser;}
    bool configured() const {return m_error.isEmpty();}
    QString configurationError() const {return m_error;}
    QUrl appUrl() const;
    bool isReady() const {return m_ready;}
    bool postMessage(const QJsonValue& data,QString* error=nullptr);
    void reloadApp();
    void setTheme(const QJsonObject& variables);

signals:
    void messageReceived(const QJsonValue& data);
    void readyChanged(bool ready);
    void failed(const QString& error);

private:
    AppResourceResponse resource(const QUrl& url,const QByteArray& method) const;
    QByteArray previewDocument() const;
    void receive(const QUrl& source,const QJsonValue& value);
    void flush();
    BrowserWidget* m_browser=nullptr;
    AppResourceStore m_resources;
    QString m_error,m_nonce;
    QUrl m_previewUrl;
    QList<QJsonValue> m_queue;
    qsizetype m_queuedBytes=0;
    QJsonObject m_theme;
    bool m_ready=false;
};
}
