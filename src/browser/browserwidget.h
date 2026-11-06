#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QUrl>
#include <QWidget>
#include <memory>
#include <functional>

namespace ade {
struct BrowserResourceResponse {
    int status=403;
    QByteArray body;
    QByteArray mimeType="text/plain; charset=utf-8";
    QMap<QByteArray,QByteArray> headers;
};

// Native QWidget host for the Windows WebView2 COM browser. The application UI
// remains Qt; only the page being previewed is rendered by the browser runtime.
class BrowserWidget final : public QWidget {
    Q_OBJECT
public:
    explicit BrowserWidget(QWidget* parent = nullptr);
    ~BrowserWidget() override;

    bool isReady() const;
    QUrl currentUrl() const;
    bool setProfileDirectory(const QString& directory);
    bool setPermissionDefaults(const QJsonObject& defaults);
    bool setPermissionExceptions(const QJsonObject& exceptions);
    QString permissionSetting(const QUrl& origin, const QString& permission) const;
    void setPermissionTimeoutMs(int timeoutMs);
    void setDownloadDecisionTimeoutMs(int timeoutMs);
    // Configure before initialization, only for host-owned local app views.
    bool setLocalContentHandler(std::function<BrowserResourceResponse(const QUrl&,const QByteArray&)> handler,
                               std::function<bool(const QUrl&)> allowTopNavigation);
    bool postWebMessage(const QJsonValue& value);

public slots:
    void navigate(const QUrl& url);
    void back();
    void forward();
    void reload();
    void stop();
    void executeScript(const QString& script);
    void callDevTools(const QString& method, const QString& json = QStringLiteral("{}"));
    void callDevToolsRequest(const QString& requestId, const QString& method,
                            const QString& json = QStringLiteral("{}"));
    void subscribeDevToolsEvent(const QString& eventName);
    void unsubscribeDevToolsEvent(const QString& eventName);
    void requestScreenshot();
    void openDevTools();
    void resolvePermission(const QString& requestId, bool allow);
    void clearBrowsingData(const QString& requestId, const QJsonObject& options);
    bool acceptDownload(const QString& downloadId, const QString& absolutePath, bool overwrite = false);
    void cancelDownload(const QString& downloadId);
    bool pauseDownload(const QString& downloadId);
    bool resumeDownload(const QString& downloadId);

signals:
    void readyChanged(bool ready);
    void urlChanged(const QUrl& url);
    void titleChanged(const QString& title);
    void loadingChanged(bool loading);
    void canGoBackChanged(bool available);
    void canGoForwardChanged(bool available);
    void scriptResult(const QString& resultJson);
    void devToolsResult(const QString& resultJson);
    void devToolsRequestCompleted(const QString& requestId, const QString& resultJson, bool error);
    void devToolsEventReceived(const QString& eventName, const QString& parametersJson);
    void webMessageReceived(const QUrl& source,const QJsonValue& value);
    void screenshotReady(const QByteArray& png);
    void errorOccurred(const QString& message);
    void permissionRequested(const QString& requestId, const QUrl& origin,
                             const QString& permission, bool userInitiated);
    void permissionRequestFinished(const QString& requestId, bool allowed);
    void fullscreenChanged(bool fullscreen);
    void browsingDataCleared(const QString& requestId, bool success, const QString& error);
    void downloadRequested(const QString& downloadId, const QUrl& source, const QString& suggestedName,
                           const QString& mimeType, qint64 totalBytes);
    void downloadStarted(const QString& downloadId, const QString& path);
    void downloadProgress(const QString& downloadId, qint64 receivedBytes, qint64 totalBytes);
    void downloadStateChanged(const QString& downloadId, const QString& state, bool canResume, const QString& error);
    void downloadFinished(const QString& downloadId, const QString& path, bool success, const QString& error);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;

private:
    class Impl;
    std::unique_ptr<Impl> d;
    void initialize();
    void updateBounds();
    void reportError(const QString& operation, long result);
    void requestPermissionDecision(const QUrl& origin, const QString& permission,
                                   bool userInitiated, std::function<void(bool)> complete);
    void cancelPermissionRequests();
    void applyPendingPermissionPolicy();
    void finishDataClear(const QString& requestId, const QString& correlation, bool success, const QString& error);
    void queueDownloadUpdate(const QString& downloadId);
    void updateDownloadState(const QString& downloadId);
    void finishDownload(const QString& downloadId, bool success, const QString& error, const QString& state = QString());
};

} // namespace ade
