#include "browserwidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHideEvent>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonParseError>
#include <QLabel>
#include <QPointer>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSet>
#include <QUuid>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <cstring>
#include <functional>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <WebView2.h>
#include "WebViewAppOptions.h"

namespace {
template<class T> class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* value) : ptr(value) {}
    ComPtr(const ComPtr& other) : ptr(other.ptr) { if (ptr) ptr->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    ~ComPtr() { reset(); }
    ComPtr& operator=(ComPtr other) noexcept { std::swap(ptr, other.ptr); return *this; }
    T* get() const { return ptr; }
    T* operator->() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
    T** put() { reset(); return &ptr; }
    void reset() { if (auto old = std::exchange(ptr, nullptr)) old->Release(); }
private:
    T* ptr = nullptr;
};

// Avoid an MSVC/WRL dependency: COM's ABI works directly with MinGW.
template<class Interface, const IID* InterfaceId, class... Args>
class Callback final : public Interface {
public:
    explicit Callback(std::function<HRESULT(Args...)> action) : fn(std::move(action)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, *InterfaceId)) {
            *result = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --refs;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return fn(args...); }
private:
    std::atomic<ULONG> refs{1};
    std::function<HRESULT(Args...)> fn;
};

using EnvironmentCallback = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, HRESULT, ICoreWebView2Environment*>;
using ControllerCallback = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
    &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, HRESULT, ICoreWebView2Controller*>;
using SourceCallback = Callback<ICoreWebView2SourceChangedEventHandler,
    &IID_ICoreWebView2SourceChangedEventHandler, ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*>;
using TitleCallback = Callback<ICoreWebView2DocumentTitleChangedEventHandler,
    &IID_ICoreWebView2DocumentTitleChangedEventHandler, ICoreWebView2*, IUnknown*>;
using StartingCallback = Callback<ICoreWebView2NavigationStartingEventHandler,
    &IID_ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>;
using CompletedCallback = Callback<ICoreWebView2NavigationCompletedEventHandler,
    &IID_ICoreWebView2NavigationCompletedEventHandler, ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>;
using HistoryCallback = Callback<ICoreWebView2HistoryChangedEventHandler,
    &IID_ICoreWebView2HistoryChangedEventHandler, ICoreWebView2*, IUnknown*>;
using ScriptCallback = Callback<ICoreWebView2ExecuteScriptCompletedHandler,
    &IID_ICoreWebView2ExecuteScriptCompletedHandler, HRESULT, LPCWSTR>;
using DevToolsCallback = Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler,
    &IID_ICoreWebView2CallDevToolsProtocolMethodCompletedHandler, HRESULT, LPCWSTR>;
using DevToolsEventCallback = Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler,
    &IID_ICoreWebView2DevToolsProtocolEventReceivedEventHandler, ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs*>;
using CaptureCallback = Callback<ICoreWebView2CapturePreviewCompletedHandler,
    &IID_ICoreWebView2CapturePreviewCompletedHandler, HRESULT>;
using NewWindowCallback = Callback<ICoreWebView2NewWindowRequestedEventHandler,
    &IID_ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>;
using ProcessFailedCallback = Callback<ICoreWebView2ProcessFailedEventHandler,
    &IID_ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>;
using PermissionCallback = Callback<ICoreWebView2PermissionRequestedEventHandler,
    &IID_ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*>;
using FullscreenCallback = Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler,
    &IID_ICoreWebView2ContainsFullScreenElementChangedEventHandler, ICoreWebView2*, IUnknown*>;
using ClearDataCallback = Callback<ICoreWebView2ClearBrowsingDataCompletedHandler,
    &IID_ICoreWebView2ClearBrowsingDataCompletedHandler, HRESULT>;
using DownloadStartingCallback = Callback<ICoreWebView2DownloadStartingEventHandler,
    &IID_ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs*>;
using DownloadBytesCallback = Callback<ICoreWebView2BytesReceivedChangedEventHandler,
    &IID_ICoreWebView2BytesReceivedChangedEventHandler, ICoreWebView2DownloadOperation*, IUnknown*>;
using DownloadStateCallback = Callback<ICoreWebView2StateChangedEventHandler,
    &IID_ICoreWebView2StateChangedEventHandler, ICoreWebView2DownloadOperation*, IUnknown*>;
using ResourceCallback = Callback<ICoreWebView2WebResourceRequestedEventHandler,
    &IID_ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>;
using WebMessageCallback = Callback<ICoreWebView2WebMessageReceivedEventHandler,
    &IID_ICoreWebView2WebMessageReceivedEventHandler, ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;

QString permissionKey(COREWEBVIEW2_PERMISSION_KIND kind) {
    switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE: return QStringLiteral("media");
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION: return QStringLiteral("geolocation");
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS: return QStringLiteral("notifications");
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ: return QStringLiteral("clipboard-read");
    case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS: return QStringLiteral("downloads");
    case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES: return QStringLiteral("midi");
    case COREWEBVIEW2_PERMISSION_KIND_PERSISTENT_STORAGE: return QStringLiteral("storage-access");
    case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY: return QStringLiteral("autoplay");
    case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS: return QStringLiteral("local-fonts");
    case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT: return QStringLiteral("window-management");
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS: return QStringLiteral("sensors");
    case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE: return QStringLiteral("file-read-write");
    default: return {};
    }
}

using CreateEnvironmentFn = HRESULT(STDAPICALLTYPE*)(PCWSTR, PCWSTR,
    ICoreWebView2EnvironmentOptions*, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);

CreateEnvironmentFn environmentFactory() {
    // Keep the official loader resident while any asynchronous COM callback can
    // execute. Resolve by absolute path so PATH/current-directory DLLs cannot win.
    static HMODULE loader = [] {
        const auto path = QDir::toNativeSeparators(
            QCoreApplication::applicationDirPath() + QStringLiteral("/WebView2Loader.dll")).toStdWString();
        return LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    if (!loader) return nullptr;
    const FARPROC address = GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions");
    CreateEnvironmentFn factory = nullptr;
    static_assert(sizeof(factory) == sizeof(address));
    std::memcpy(&factory, &address, sizeof(factory));
    return factory;
}

QString takeComString(LPWSTR text) {
    const QString value = text ? QString::fromWCharArray(text) : QString();
    CoTaskMemFree(text);
    return value;
}
} // namespace
#endif

namespace ade {
namespace {
QString canonicalPermission(QString key) {
    if (key == QStringLiteral("camera")) key = QStringLiteral("media");
    else if (key == QStringLiteral("location")) key = QStringLiteral("geolocation");
    else if (key == QStringLiteral("clipboard")) key = QStringLiteral("clipboard-read");
    static const QSet<QString> known{"media", "geolocation", "notifications", "fullscreen", "bluetooth", "hid", "serial", "usb",
        "clipboard-read", "display-capture", "midi", "idle-detection", "speaker-selection", "storage-access",
        "downloads", "autoplay", "local-fonts", "window-management", "sensors", "file-read-write"};
    return known.contains(key) ? key : QString();
}
QString permissionValue(const QJsonValue& value) {
    if (value.isDouble()) {
        if (value.toDouble() == 0) return QStringLiteral("ask");
        if (value.toDouble() == 1) return QStringLiteral("allow");
        if (value.toDouble() == 2) return QStringLiteral("block");
    }
    const QString text = value.toString();
    return text == "ask" || text == "allow" || text == "block" ? text : QString();
}
QString canonicalOrigin(const QUrl& value) {
    const QString scheme = value.scheme().toLower();
    if (!value.isValid() || value.host().isEmpty() || (scheme != "http" && scheme != "https")) return {};
    QUrl origin;
    origin.setScheme(scheme); origin.setHost(value.host().toLower());
    const int port = value.port();
    if (port >= 0 && !((scheme == "http" && port == 80) || (scheme == "https" && port == 443))) origin.setPort(port);
    return origin.toString(QUrl::FullyEncoded);
}
}
class BrowserWidget::Impl {
public:
    std::function<BrowserResourceResponse(const QUrl&,const QByteArray&)> localContent;
    std::function<bool(const QUrl&)> allowTopNavigation;
    QLabel* status = nullptr;
    QUrl pendingUrl;
    QUrl currentUrl;
    QString profileDirectory;
    bool initializing = false;
    bool ready = false;
    quint64 navigationId = 0;
    quint64 navigationRevision = 0;
    QSet<QString> eventNames;
    QJsonObject permissionDefaults;
    QJsonObject permissionExceptions;
    int permissionTimeoutMs = 30000;
    bool nativeFullscreen = false;
    struct PermissionRequest {
        QUrl origin;
        QString key;
        std::function<void(bool)> complete;
        QPointer<QTimer> deadline;
    };
    QHash<QString, PermissionRequest> permissions;
    struct ClearRequest { QString correlation; QPointer<QTimer> deadline; };
    QHash<QString, ClearRequest> clearRequests;
    int downloadDecisionTimeoutMs = 120000;
    // QPointer is cleared in QObject's destructor, after QWidget has destroyed
    // children and may have pumped native messages. Invalidate callbacks at the
    // beginning of this derived destructor, before either of those can happen.
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
#ifdef Q_OS_WIN
    bool uninitializeCom = false;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    ComPtr<ICoreWebView2_4> downloadView;
    EventRegistrationToken sourceToken{}, titleToken{}, startToken{}, completeToken{}, historyToken{}, newWindowToken{}, processToken{}, permissionToken{}, fullscreenToken{};
    EventRegistrationToken resourceToken{},webMessageToken{};
    struct DevToolsSubscription {
        ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        EventRegistrationToken token{};
    };
    QHash<QString, DevToolsSubscription> subscriptions;
    EventRegistrationToken downloadToken{};
    struct Download {
        ComPtr<ICoreWebView2DownloadOperation> operation;
        ComPtr<ICoreWebView2DownloadStartingEventArgs> arguments;
        ComPtr<ICoreWebView2Deferral> deferral;
        EventRegistrationToken bytesToken{}, stateToken{};
        QPointer<QTimer> deadline;
        QString path;
        bool accepted = false;
        bool updateQueued = false;
        QString lastState;
        qint64 lastReceived = -1;
    };
    QHash<QString, Download> downloads;
#endif
};

BrowserWidget::BrowserWidget(QWidget* parent) : QWidget(parent), d(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("browserWidget"));
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    d->status = new QLabel(tr("Starting browser…"), this);
    d->status->setWordWrap(true);
    d->status->setAlignment(Qt::AlignCenter);
    layout->addWidget(d->status);
}

BrowserWidget::~BrowserWidget() {
    d->lifetime.reset();
    const auto requests = std::exchange(d->permissions, {});
    for (const auto& request : requests) {
        if (request.deadline) request.deadline->stop();
        request.complete(false);
    }
#ifdef Q_OS_WIN
    d->ready = false;
    const auto downloads = std::exchange(d->downloads, {});
    for (const auto& download : downloads) {
        if (download.deadline) download.deadline->stop();
        download.operation->remove_BytesReceivedChanged(download.bytesToken);
        download.operation->remove_StateChanged(download.stateToken);
        if (download.arguments) download.arguments->put_Cancel(TRUE);
        if (download.deferral) download.deferral->Complete();
        download.operation->Cancel();
    }
    if (d->downloadView) d->downloadView->remove_DownloadStarting(d->downloadToken);
    d->downloadView.reset();
    for (const auto& subscription : std::as_const(d->subscriptions))
        subscription.receiver->remove_DevToolsProtocolEventReceived(subscription.token);
    d->subscriptions.clear();
    if (d->view) {
        d->view->remove_SourceChanged(d->sourceToken);
        d->view->remove_DocumentTitleChanged(d->titleToken);
        d->view->remove_NavigationStarting(d->startToken);
        d->view->remove_NavigationCompleted(d->completeToken);
        d->view->remove_HistoryChanged(d->historyToken);
        d->view->remove_NewWindowRequested(d->newWindowToken);
        d->view->remove_ProcessFailed(d->processToken);
        d->view->remove_PermissionRequested(d->permissionToken);
        d->view->remove_ContainsFullScreenElementChanged(d->fullscreenToken);
        d->view->remove_WebResourceRequested(d->resourceToken);
        d->view->remove_WebMessageReceived(d->webMessageToken);
    }
    if (d->controller) d->controller->Close();
    d->view.reset();
    d->controller.reset();
    d->environment.reset();
    if (d->uninitializeCom) CoUninitialize();
#endif
}

bool BrowserWidget::isReady() const { return d->ready; }
QUrl BrowserWidget::currentUrl() const { return d->currentUrl; }
bool BrowserWidget::setProfileDirectory(const QString& directory) {
    if (d->initializing || d->ready) return false;
    d->profileDirectory = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    return true;
}
bool BrowserWidget::setLocalContentHandler(std::function<BrowserResourceResponse(const QUrl&,const QByteArray&)> handler,
                                         std::function<bool(const QUrl&)> allowTopNavigation) {
    if(d->initializing||d->ready||!handler||!allowTopNavigation)return false;
    d->localContent=std::move(handler);d->allowTopNavigation=std::move(allowTopNavigation);return true;
}
bool BrowserWidget::postWebMessage(const QJsonValue& value) {
#ifdef Q_OS_WIN
    if(!d->ready||!d->view||!d->localContent)return false;
    auto bytes=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    if(bytes.size()>256*1024)return false;
    const auto message=QString::fromUtf8(bytes.mid(1,bytes.size()-2)).toStdWString();
    return SUCCEEDED(d->view->PostWebMessageAsJson(message.c_str()));
#else
    Q_UNUSED(value)return false;
#endif
}

bool BrowserWidget::setPermissionDefaults(const QJsonObject& defaults) {
    QJsonObject validated;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const QString key = canonicalPermission(it.key()), value = permissionValue(it.value());
        if (key.isEmpty() || value.isEmpty() || (validated.contains(key) && validated.value(key) != value)) return false;
        validated.insert(key, value);
    }
    d->permissionDefaults = validated;
    applyPendingPermissionPolicy();
    return true;
}
bool BrowserWidget::setPermissionExceptions(const QJsonObject& exceptions) {
    QJsonObject validated;
    for (auto it = exceptions.begin(); it != exceptions.end(); ++it) {
        const QString key = canonicalPermission(it.key());
        if (key.isEmpty() || !it.value().isObject()) return false;
        QJsonObject origins = validated.value(key).toObject();
        const auto entries = it.value().toObject();
        for (auto originIt = entries.begin(); originIt != entries.end(); ++originIt) {
            const QJsonValue setting = originIt.value().isObject() ? originIt.value().toObject().value("setting") : originIt.value();
            const QString origin = canonicalOrigin(QUrl(originIt.key())), value = permissionValue(setting);
            if (origin.isEmpty() || value.isEmpty() || (origins.contains(origin) && origins.value(origin) != value)) return false;
            origins.insert(origin, value);
        }
        validated.insert(key, origins);
    }
    d->permissionExceptions = validated;
    applyPendingPermissionPolicy();
    return true;
}
QString BrowserWidget::permissionSetting(const QUrl& origin, const QString& permission) const {
    const QString key = canonicalPermission(permission), site = canonicalOrigin(origin);
    if (key.isEmpty() || site.isEmpty()) return QStringLiteral("block");
    const auto exceptions = d->permissionExceptions.value(key).toObject();
    if (exceptions.contains(site)) return exceptions.value(site).toString();
    if (d->permissionDefaults.contains(key)) return d->permissionDefaults.value(key).toString();
    // Matches the pinned upstream defaults. All other configurable types ask.
    return key == "fullscreen" || key == "midi" ? QStringLiteral("allow") : QStringLiteral("ask");
}
void BrowserWidget::setPermissionTimeoutMs(int milliseconds) { d->permissionTimeoutMs = qBound(50, milliseconds, 120000); }
void BrowserWidget::requestPermissionDecision(const QUrl& origin, const QString& key, bool userInitiated, std::function<void(bool)> complete) {
    const QString setting = permissionSetting(origin, key);
    if (setting != "ask" || d->permissions.size() >= 32) { complete(setting == "allow"); return; }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    Impl::PermissionRequest request{QUrl(canonicalOrigin(origin)), key, std::move(complete), new QTimer(this)};
    request.deadline->setSingleShot(true);
    connect(request.deadline, &QTimer::timeout, this, [this, id] { resolvePermission(id, false); });
    request.deadline->start(d->permissionTimeoutMs);
    d->permissions.insert(id, request);
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    QTimer::singleShot(0, this, [self, lifetime, id, userInitiated] {
        if (lifetime.expired() || !self || !self->d->permissions.contains(id)) return;
        const auto pending = self->d->permissions.value(id);
        emit self->permissionRequested(id, pending.origin, pending.key, userInitiated);
    });
}
void BrowserWidget::resolvePermission(const QString& id, bool allow) {
    if (!d->permissions.contains(id)) return;
    const auto request = d->permissions.take(id);
    if (request.deadline) { request.deadline->stop(); request.deadline->deleteLater(); }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    request.complete(allow);
    if (!lifetime.expired() && self) emit permissionRequestFinished(id, allow);
}
void BrowserWidget::cancelPermissionRequests() {
    const auto ids = d->permissions.keys();
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    for (const auto& id : ids) { resolvePermission(id, false); if (lifetime.expired() || !self) return; }
}
void BrowserWidget::applyPendingPermissionPolicy() {
    const auto ids = d->permissions.keys();
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    for (const auto& id : ids) {
        if (!d->permissions.contains(id)) continue;
        const auto pending = d->permissions.value(id);
        const auto setting = permissionSetting(pending.origin, pending.key);
        if (setting != "ask") resolvePermission(id, setting == "allow");
        if (lifetime.expired() || !self) return;
    }
}

void BrowserWidget::finishDataClear(const QString& id, const QString& correlation, bool success, const QString& error) {
    if (!d->clearRequests.contains(id) || d->clearRequests.value(id).correlation != correlation) return;
    const auto request = d->clearRequests.take(id);
    if (request.deadline) { request.deadline->stop(); request.deadline->deleteLater(); }
    emit browsingDataCleared(id, success, error);
}
void BrowserWidget::clearBrowsingData(const QString& id, const QJsonObject& options) {
    if (d->clearRequests.contains(id)) {
        finishDataClear(id, d->clearRequests.value(id).correlation, false, tr("Duplicate data-clear request ID; the original operation may still finish."));
        return;
    }
    if (id.isEmpty() || id.size() > 512 || !d->ready) { emit browsingDataCleared(id, false, tr("Browser is not ready or request ID is invalid.")); return; }
#ifdef Q_OS_WIN
    const QHash<QString, COREWEBVIEW2_BROWSING_DATA_KINDS> supported{
        {"cookies",COREWEBVIEW2_BROWSING_DATA_KINDS_COOKIES},{"cache",COREWEBVIEW2_BROWSING_DATA_KINDS_DISK_CACHE},
        {"storage",COREWEBVIEW2_BROWSING_DATA_KINDS_LOCAL_STORAGE},{"indexedDB",COREWEBVIEW2_BROWSING_DATA_KINDS_INDEXED_DB},
        {"serviceWorkers",COREWEBVIEW2_BROWSING_DATA_KINDS_SERVICE_WORKERS},{"cacheStorage",COREWEBVIEW2_BROWSING_DATA_KINDS_CACHE_STORAGE},
        {"history",COREWEBVIEW2_BROWSING_DATA_KINDS_BROWSING_HISTORY},{"downloads",COREWEBVIEW2_BROWSING_DATA_KINDS_DOWNLOAD_HISTORY}};
    unsigned int kinds = 0;
    for (auto it=options.begin();it!=options.end();++it) {
        if (!supported.contains(it.key()) || !it.value().isBool()) {
            emit browsingDataCleared(id,false,tr("Unsupported browser data option: %1. This operation currently clears selected runtime data for all time.").arg(it.key()));return;
        }
        if(it.value().toBool())kinds|=static_cast<unsigned int>(supported.value(it.key()));
    }
    if(!kinds){emit browsingDataCleared(id,false,tr("Choose browser data to clear."));return;}
    if(d->clearRequests.size()>=8){emit browsingDataCleared(id,false,tr("Too many data-clear operations are pending."));return;}
    ComPtr<ICoreWebView2_13> extended;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile2> profile2;
    HRESULT result=d->view->QueryInterface(IID_ICoreWebView2_13,reinterpret_cast<void**>(extended.put()));
    if(SUCCEEDED(result))result=extended->get_Profile(profile.put());
    if(SUCCEEDED(result))result=profile->QueryInterface(IID_ICoreWebView2Profile2,reinterpret_cast<void**>(profile2.put()));
    if(FAILED(result)){emit browsingDataCleared(id,false,tr("This WebView2 runtime does not support profile data clearing."));return;}
    const QString correlation=QUuid::createUuid().toString(QUuid::WithoutBraces);
    Impl::ClearRequest request{correlation,new QTimer(this)};request.deadline->setSingleShot(true);request.deadline->start(45000);
    connect(request.deadline,&QTimer::timeout,this,[this,id,correlation]{finishDataClear(id,correlation,false,tr("Browser data clearing timed out; its final state is unknown."));});
    d->clearRequests.insert(id,request);
    const QPointer<BrowserWidget> self(this);const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<ClearDataCallback> callback(new ClearDataCallback([self,lifetime,id,correlation](HRESULT status)->HRESULT{
        if(lifetime.expired()||!self)return S_OK;
        self->finishDataClear(id,correlation,SUCCEEDED(status),SUCCEEDED(status)?QString():tr("Browser data clearing failed (0x%1).").arg(static_cast<quint32>(status),8,16,QLatin1Char('0')));
        return S_OK;
    }));
    result=profile2->ClearBrowsingData(static_cast<COREWEBVIEW2_BROWSING_DATA_KINDS>(kinds),callback.get());
    if(!lifetime.expired()&&self&&FAILED(result))finishDataClear(id,correlation,false,tr("Could not begin browser data clearing (0x%1).").arg(static_cast<quint32>(result),8,16,QLatin1Char('0')));
#else
    Q_UNUSED(options);
    emit browsingDataCleared(id,false,tr("Browser data clearing requires the Windows WebView2 backend."));
#endif
}

void BrowserWidget::setDownloadDecisionTimeoutMs(int milliseconds) { d->downloadDecisionTimeoutMs = qBound(50, milliseconds, 120000); }
bool BrowserWidget::acceptDownload(const QString& id, const QString& absolutePath, bool overwrite) {
#ifdef Q_OS_WIN
    auto it=d->downloads.find(id);
    if(it==d->downloads.end()||it->accepted||!it->arguments||!it->deferral)return false;
    const QString path=QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
    const QFileInfo destination(path);
    QString error;
    if(!QDir::isAbsolutePath(path)||destination.fileName().isEmpty()||path.contains(QChar::Null)||path.mid(2).contains(':'))error=tr("Choose an absolute file path without an alternate data stream.");
    const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)"),QRegularExpression::CaseInsensitiveOption);
    for(const auto& part:path.split('/'))if(part.endsWith('.')||part.endsWith(' ')||reserved.match(part).hasMatch())error=tr("The destination contains an unsupported Windows file name.");
    if(destination.isDir()||destination.isSymLink())error=tr("The destination must be a regular file path.");
    else if(destination.exists()&&!overwrite)error=tr("The destination already exists; choose a new name or explicitly confirm overwrite.");
    for(auto other=d->downloads.cbegin();other!=d->downloads.cend();++other)
        if(other.key()!=id&&other->accepted&&other->path.compare(path,Qt::CaseInsensitive)==0)error=tr("Another active download is using this destination.");
    if(!error.isEmpty()){emit downloadStateChanged(id,"awaiting-path",false,error);return false;}
    const auto wide=QDir::toNativeSeparators(path).toStdWString();
    const HRESULT result=it->arguments->put_ResultFilePath(wide.c_str());
    if(FAILED(result)){emit downloadStateChanged(id,"awaiting-path",false,tr("WebView2 rejected the destination path."));return false;}
    const auto args=it->arguments;const auto deferral=it->deferral;
    it->path=path;it->accepted=true;it->arguments.reset();it->deferral.reset();
    if(it->deadline){it->deadline->stop();it->deadline->deleteLater();it->deadline=nullptr;}
    args->put_Cancel(FALSE);
    const QPointer<BrowserWidget> self(this);const std::weak_ptr<int> lifetime(d->lifetime);
    emit downloadStarted(id,path);
    if(lifetime.expired()||!self)args->put_Cancel(TRUE);
    deferral->Complete();
    if(!lifetime.expired()&&self)queueDownloadUpdate(id);
    return true;
#else
    Q_UNUSED(id);Q_UNUSED(absolutePath);Q_UNUSED(overwrite);return false;
#endif
}
void BrowserWidget::finishDownload(const QString& id, bool success, const QString& error, const QString& state) {
#ifdef Q_OS_WIN
    if(!d->downloads.contains(id))return;
    const auto download=d->downloads.take(id);
    if(download.deadline){download.deadline->stop();download.deadline->deleteLater();}
    download.operation->remove_BytesReceivedChanged(download.bytesToken);
    download.operation->remove_StateChanged(download.stateToken);
    if(download.arguments)download.arguments->put_Cancel(TRUE);
    if(download.deferral)download.deferral->Complete();
    if(!success)download.operation->Cancel();
    const QPointer<BrowserWidget> self(this);const std::weak_ptr<int> lifetime(d->lifetime);
    emit downloadStateChanged(id,state.isEmpty()?(success?QStringLiteral("completed"):QStringLiteral("canceled")):state,false,error);
    if(!lifetime.expired()&&self)emit downloadFinished(id,download.path,success,error);
#else
    Q_UNUSED(id);Q_UNUSED(success);Q_UNUSED(error);Q_UNUSED(state);
#endif
}
void BrowserWidget::cancelDownload(const QString& id) { finishDownload(id,false,tr("Download canceled.")); }
bool BrowserWidget::pauseDownload(const QString& id) {
#ifdef Q_OS_WIN
    const auto it=d->downloads.constFind(id);
    if(it==d->downloads.cend()||!it->accepted)return false;
    const HRESULT result=it->operation->Pause();
    if(SUCCEEDED(result))queueDownloadUpdate(id);
    return SUCCEEDED(result);
#else
    Q_UNUSED(id);return false;
#endif
}
bool BrowserWidget::resumeDownload(const QString& id) {
#ifdef Q_OS_WIN
    const auto it=d->downloads.constFind(id);
    if(it==d->downloads.cend()||!it->accepted)return false;
    BOOL canResume=FALSE;it->operation->get_CanResume(&canResume);
    if(!canResume)return false;
    const HRESULT result=it->operation->Resume();
    if(SUCCEEDED(result))queueDownloadUpdate(id);
    return SUCCEEDED(result);
#else
    Q_UNUSED(id);return false;
#endif
}
void BrowserWidget::queueDownloadUpdate(const QString& id) {
#ifdef Q_OS_WIN
    auto it=d->downloads.find(id);if(it==d->downloads.end()||it->updateQueued)return;it->updateQueued=true;
    QTimer::singleShot(0,this,[this,id]{updateDownloadState(id);});
#else
    Q_UNUSED(id);
#endif
}
void BrowserWidget::updateDownloadState(const QString& id) {
#ifdef Q_OS_WIN
    auto it=d->downloads.find(id);if(it==d->downloads.end())return;
    it->updateQueued=false;
    if(!it->accepted)return;
    const auto operation=it->operation;
    INT64 received=0,total=-1;operation->get_BytesReceived(&received);operation->get_TotalBytesToReceive(&total);
    COREWEBVIEW2_DOWNLOAD_STATE state{};COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason{};BOOL canResume=FALSE;
    if(FAILED(operation->get_State(&state)))return;
    operation->get_InterruptReason(&reason);operation->get_CanResume(&canResume);
    const QPointer<BrowserWidget> self(this);const std::weak_ptr<int> lifetime(d->lifetime);
    if(it->lastReceived!=received){it->lastReceived=received;emit downloadProgress(id,received,total);}
    if(lifetime.expired()||!self||!d->downloads.contains(id))return;
    if(state==COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED){finishDownload(id,true,QString());return;}
    const bool paused=state==COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED&&reason==COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED;
    const QString label=paused?QStringLiteral("paused"):state==COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS?QStringLiteral("in-progress"):QStringLiteral("interrupted");
    const QString error=state==COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED&&!paused?tr("Download interrupted (WebView2 reason %1).").arg(int(reason)):QString();
    if(state==COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED&&!canResume){finishDownload(id,false,error,"interrupted");return;}
    if(d->downloads.value(id).lastState!=label){d->downloads[id].lastState=label;emit downloadStateChanged(id,label,canResume,error);}
#else
    Q_UNUSED(id);
#endif
}

void BrowserWidget::reportError(const QString& operation, long result) {
    const QString message = operation + tr(" (0x%1)").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    if (!d->ready) {
        d->initializing = false;
        d->status->setText(message);
        d->status->show();
    }
    emit errorOccurred(message);
}

void BrowserWidget::initialize() {
    if (d->ready || d->initializing) return;
    d->initializing = true;
#ifdef Q_OS_WIN
    if (!d->uninitializeCom) {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult)) { reportError(tr("The browser requires a Windows STA UI thread"), comResult); return; }
        d->uninitializeCom = true;
    }
    const auto createEnvironment = environmentFactory();
    if (!createEnvironment) {
        reportError(tr("WebView2Loader.dll is missing beside the application"), HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    const QString dataDir = d->profileDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/browser-profile")
        : d->profileDirectory;
    if (!QDir().mkpath(dataDir)) {
        reportError(tr("Cannot create the browser profile directory"), E_ACCESSDENIED);
        return;
    }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<EnvironmentCallback> callback(new EnvironmentCallback([self, lifetime](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
        if (lifetime.expired() || !self) return S_OK;
        if (FAILED(result) || !environment) {
            self->reportError(tr("Cannot start WebView2. Install the Microsoft Edge WebView2 Runtime"), FAILED(result) ? result : E_POINTER);
            return S_OK;
        }
        environment->AddRef();
        self->d->environment = ComPtr<ICoreWebView2Environment>(environment);
        ComPtr<ControllerCallback> controllerCallback(new ControllerCallback([self, lifetime](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
            if (lifetime.expired() || !self) { if (controller) controller->Close(); return S_OK; }
            if (FAILED(controllerResult) || !controller) {
                self->reportError(tr("Cannot create the embedded browser"), FAILED(controllerResult) ? controllerResult : E_POINTER);
                return S_OK;
            }
            controller->AddRef();
            self->d->controller = ComPtr<ICoreWebView2Controller>(controller);
            const HRESULT viewResult = controller->get_CoreWebView2(self->d->view.put());
            if (FAILED(viewResult) || !self->d->view) {
                self->reportError(tr("Cannot access the embedded browser"), FAILED(viewResult) ? viewResult : E_POINTER);
                return S_OK;
            }
            auto* view = self->d->view.get();
            if(self->d->localContent){
                ComPtr<ICoreWebView2Settings> settings;
                if(SUCCEEDED(view->get_Settings(settings.put()))){settings->put_AreHostObjectsAllowed(FALSE);settings->put_IsWebMessageEnabled(TRUE);settings->put_AreDefaultScriptDialogsEnabled(FALSE);}
                ComPtr<ICoreWebView2_22> modern;
                if(FAILED(view->QueryInterface(IID_ICoreWebView2_22,reinterpret_cast<void**>(modern.put())))||
                    FAILED(modern->AddWebResourceRequestedFilterWithRequestSourceKinds(L"*",COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL,COREWEBVIEW2_WEB_RESOURCE_REQUEST_SOURCE_KINDS_DOCUMENT))){
                    self->reportError(tr("The browser runtime does not support isolated app resources"),E_NOINTERFACE);return S_OK;
                }
                ComPtr<ResourceCallback> resource(new ResourceCallback([self,lifetime](ICoreWebView2*,ICoreWebView2WebResourceRequestedEventArgs* args)->HRESULT{
                    if(lifetime.expired()||!self)return S_OK;
                    ComPtr<ICoreWebView2WebResourceRequest> request;if(FAILED(args->get_Request(request.put())))return S_OK;
                    LPWSTR rawUrl=nullptr,rawMethod=nullptr;request->get_Uri(&rawUrl);request->get_Method(&rawMethod);
                    const QUrl url(takeComString(rawUrl));const auto method=takeComString(rawMethod).toLatin1();
                    const auto response=self->d->localContent(url,method);if(lifetime.expired()||!self)return S_OK;
                    if(response.body.size()>16*1024*1024)return E_INVALIDARG;
                    ComPtr<IStream> stream;if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,stream.put())))return E_OUTOFMEMORY;
                    ULONG written=0;HRESULT status=stream->Write(response.body.constData(),static_cast<ULONG>(response.body.size()),&written);
                    if(FAILED(status)||written!=response.body.size())return E_FAIL;
                    LARGE_INTEGER beginning{};stream->Seek(beginning,STREAM_SEEK_SET,nullptr);
                    QByteArray headers="Content-Type: "+response.mimeType+"\r\n";
                    for(auto it=response.headers.cbegin();it!=response.headers.cend();++it){
                        if(it.key().contains('\r')||it.key().contains('\n')||it.value().contains('\r')||it.value().contains('\n'))return E_INVALIDARG;
                        headers+=it.key()+": "+it.value()+"\r\n";
                    }
                    ComPtr<ICoreWebView2WebResourceResponse> result;const auto headerText=QString::fromUtf8(headers).toStdWString();
                    status=self->d->environment->CreateWebResourceResponse(stream.get(),response.status,L"App Resource",headerText.c_str(),result.put());
                    return SUCCEEDED(status)?args->put_Response(result.get()):status;
                }));
                view->add_WebResourceRequested(resource.get(),&self->d->resourceToken);
                ComPtr<WebMessageCallback> message(new WebMessageCallback([self,lifetime](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
                    if(lifetime.expired()||!self)return S_OK;
                    LPWSTR source=nullptr,value=nullptr;if(FAILED(args->get_Source(&source)))return S_OK;
                    const QUrl url(takeComString(source));if(FAILED(args->get_WebMessageAsJson(&value)))return S_OK;
                    const QString text=takeComString(value);if(text.size()>256*1024)return S_OK;
                    QJsonParseError error;const auto document=QJsonDocument::fromJson(QByteArray("[")+text.toUtf8()+']',&error);
                    if(error.error==QJsonParseError::NoError&&document.isArray()&&document.array().size()==1)emit self->webMessageReceived(url,document.array().first());
                    return S_OK;
                }));
                view->add_WebMessageReceived(message.get(),&self->d->webMessageToken);
            }
            ComPtr<SourceCallback> source(new SourceCallback([self, lifetime](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                if (lifetime.expired() || !self) return S_OK;
                LPWSTR value = nullptr;
                if (SUCCEEDED(sender->get_Source(&value))) {
                    self->d->currentUrl = QUrl(takeComString(value));
                    emit self->urlChanged(self->d->currentUrl);
                }
                return S_OK;
            }));
            view->add_SourceChanged(source.get(), &self->d->sourceToken);
            ComPtr<TitleCallback> title(new TitleCallback([self, lifetime](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (lifetime.expired() || !self) return S_OK;
                LPWSTR value = nullptr;
                if (SUCCEEDED(sender->get_DocumentTitle(&value))) emit self->titleChanged(takeComString(value));
                return S_OK;
            }));
            view->add_DocumentTitleChanged(title.get(), &self->d->titleToken);
            ComPtr<StartingCallback> starting(new StartingCallback([self, lifetime](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                if (!lifetime.expired() && self) {
                    if(self->d->allowTopNavigation){LPWSTR uri=nullptr;args->get_Uri(&uri);const bool allowed=self->d->allowTopNavigation(QUrl(takeComString(uri)));if(lifetime.expired()||!self)return S_OK;if(!allowed){args->put_Cancel(TRUE);return S_OK;}}
                    self->cancelPermissionRequests();
                    if (lifetime.expired() || !self) return S_OK;
                    UINT64 navigationId = 0;
                    args->get_NavigationId(&navigationId);
                    self->d->navigationId = navigationId;
                    emit self->loadingChanged(true);
                }
                return S_OK;
            }));
            view->add_NavigationStarting(starting.get(), &self->d->startToken);
            ComPtr<CompletedCallback> completed(new CompletedCallback([self, lifetime](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (lifetime.expired() || !self) return S_OK;
                UINT64 navigationId = 0;
                args->get_NavigationId(&navigationId);
                if (navigationId != self->d->navigationId) return S_OK;
                BOOL succeeded = FALSE;
                args->get_IsSuccess(&succeeded);
                emit self->loadingChanged(false);
                if (!lifetime.expired() && self && !succeeded) {
                    COREWEBVIEW2_WEB_ERROR_STATUS status{};
                    args->get_WebErrorStatus(&status);
                    // Stop/replaced navigation is expected, not a browser failure.
                    if (status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
                        emit self->errorOccurred(tr("Page navigation failed (WebView2 status %1)").arg(int(status)));
                }
                return S_OK;
            }));
            view->add_NavigationCompleted(completed.get(), &self->d->completeToken);
            ComPtr<HistoryCallback> history(new HistoryCallback([self, lifetime](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                if (lifetime.expired() || !self) return S_OK;
                BOOL canBack = FALSE, canForward = FALSE;
                sender->get_CanGoBack(&canBack);
                sender->get_CanGoForward(&canForward);
                emit self->canGoBackChanged(canBack);
                if (!lifetime.expired() && self) emit self->canGoForwardChanged(canForward);
                return S_OK;
            }));
            view->add_HistoryChanged(history.get(), &self->d->historyToken);
            ComPtr<NewWindowCallback> newWindow(new NewWindowCallback([self, lifetime](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                // Stay inside the application's preview surface. Queue navigation
                // because WebView2 does not support nested UI work in callbacks.
                args->put_Handled(TRUE);
                LPWSTR uri = nullptr;
                if (!lifetime.expired() && self && SUCCEEDED(args->get_Uri(&uri))) {
                    const QUrl url(takeComString(uri));
                    QTimer::singleShot(0, self, [self, lifetime, url] { if (!lifetime.expired() && self) self->navigate(url); });
                }
                return S_OK;
            }));
            view->add_NewWindowRequested(newWindow.get(), &self->d->newWindowToken);
            ComPtr<ProcessFailedCallback> process(new ProcessFailedCallback([self, lifetime](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                if (!lifetime.expired() && self) {
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                    args->get_ProcessFailedKind(&kind);
                    emit self->errorOccurred(tr("A browser process stopped unexpectedly (kind %1). Reload the page.").arg(int(kind)));
                }
                return S_OK;
            }));
            view->add_ProcessFailed(process.get(), &self->d->processToken);
            ComPtr<PermissionCallback> permission(new PermissionCallback([self, lifetime](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                if (lifetime.expired() || !self) return S_OK;
                ComPtr<ICoreWebView2PermissionRequestedEventArgs3> extended;
                if (FAILED(args->QueryInterface(IID_ICoreWebView2PermissionRequestedEventArgs3, reinterpret_cast<void**>(extended.put())))) return S_OK;
                // Native preferences are the source of truth; Chromium must not
                // silently remember a prior answer and bypass future requests.
                extended->put_SavesInProfile(FALSE);
                extended->put_Handled(TRUE);
                COREWEBVIEW2_PERMISSION_KIND kind{};
                if (FAILED(args->get_PermissionKind(&kind))) return S_OK;
                const QString key = permissionKey(kind);
                LPWSTR uri = nullptr;
                if (key.isEmpty() || FAILED(args->get_Uri(&uri))) return S_OK;
                const QUrl origin(takeComString(uri));
                BOOL userInitiated = FALSE; args->get_IsUserInitiated(&userInitiated);
                ComPtr<ICoreWebView2Deferral> deferral;
                if (FAILED(args->GetDeferral(deferral.put()))) return S_OK;
                args->AddRef();
                const ComPtr<ICoreWebView2PermissionRequestedEventArgs> pending(args);
                self->requestPermissionDecision(origin, key, userInitiated, [pending, deferral](bool allow) {
                    pending->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
                    deferral->Complete();
                });
                return S_OK;
            }));
            view->add_PermissionRequested(permission.get(), &self->d->permissionToken);
            ComPtr<FullscreenCallback> fullscreen(new FullscreenCallback([self, lifetime](ICoreWebView2*, IUnknown*) -> HRESULT {
                if (lifetime.expired() || !self) return S_OK;
                QTimer::singleShot(0, self, [self, lifetime] {
                    if (lifetime.expired() || !self || !self->d->view) return;
                    BOOL contains = FALSE;
                    self->d->view->get_ContainsFullScreenElement(&contains);
                    if (!contains) {
                        const auto ids = self->d->permissions.keys();
                        for (const auto& id : ids) if (self->d->permissions.value(id).key == "fullscreen") {
                            self->resolvePermission(id, false);
                            if (lifetime.expired() || !self) return;
                        }
                        if (self->d->nativeFullscreen) { self->d->nativeFullscreen = false; emit self->fullscreenChanged(false); }
                        return;
                    }
                    if (self->d->nativeFullscreen) return;
                    for (const auto& pending : std::as_const(self->d->permissions)) if (pending.key == "fullscreen") return;
                    self->requestPermissionDecision(self->currentUrl(), "fullscreen", true, [self, lifetime](bool allow) {
                        if (lifetime.expired() || !self) return;
                        if (allow) { self->d->nativeFullscreen = true; emit self->fullscreenChanged(true); }
                        else self->executeScript(QStringLiteral("if(document.fullscreenElement)document.exitFullscreen().catch(()=>{});"));
                    });
                });
                return S_OK;
            }));
            view->add_ContainsFullScreenElementChanged(fullscreen.get(), &self->d->fullscreenToken);
            if(SUCCEEDED(view->QueryInterface(IID_ICoreWebView2_4,reinterpret_cast<void**>(self->d->downloadView.put())))) {
                ComPtr<DownloadStartingCallback> downloadStarting(new DownloadStartingCallback([self,lifetime](ICoreWebView2*,ICoreWebView2DownloadStartingEventArgs* args)->HRESULT{
                    args->put_Handled(TRUE);args->put_Cancel(TRUE);
                    if(lifetime.expired()||!self||self->d->downloads.size()>=32)return S_OK;
                    Impl::Download download;
                    if(FAILED(args->get_DownloadOperation(download.operation.put()))||FAILED(args->GetDeferral(download.deferral.put())))return S_OK;
                    args->AddRef();download.arguments=ComPtr<ICoreWebView2DownloadStartingEventArgs>(args);
                    LPWSTR value=nullptr;QUrl source;QString name,mime;INT64 total=-1;
                    if(SUCCEEDED(download.operation->get_Uri(&value)))source=QUrl(takeComString(value));
                    if(SUCCEEDED(args->get_ResultFilePath(&value)))name=QFileInfo(takeComString(value)).fileName();
                    if(SUCCEEDED(download.operation->get_MimeType(&value)))mime=takeComString(value);
                    download.operation->get_TotalBytesToReceive(&total);
                    const QString id=QUuid::createUuid().toString(QUuid::WithoutBraces);
                    ComPtr<DownloadBytesCallback> bytes(new DownloadBytesCallback([self,lifetime,id](ICoreWebView2DownloadOperation*,IUnknown*)->HRESULT{
                        if(!lifetime.expired()&&self)self->queueDownloadUpdate(id);
                        return S_OK;
                    }));
                    ComPtr<DownloadStateCallback> state(new DownloadStateCallback([self,lifetime,id](ICoreWebView2DownloadOperation*,IUnknown*)->HRESULT{
                        if(!lifetime.expired()&&self)self->queueDownloadUpdate(id);
                        return S_OK;
                    }));
                    download.operation->add_BytesReceivedChanged(bytes.get(),&download.bytesToken);
                    download.operation->add_StateChanged(state.get(),&download.stateToken);
                    download.deadline=new QTimer(self);download.deadline->setSingleShot(true);download.deadline->start(self->d->downloadDecisionTimeoutMs);
                    QObject::connect(download.deadline,&QTimer::timeout,self,[self,lifetime,id]{if(!lifetime.expired()&&self)self->finishDownload(id,false,tr("Download destination selection timed out."));});
                    self->d->downloads.insert(id,download);
                    QTimer::singleShot(0,self,[self,lifetime,id,source,name,mime,total]{
                        if(!lifetime.expired()&&self&&self->d->downloads.contains(id))emit self->downloadRequested(id,source,name,mime,total);
                    });
                    return S_OK;
                }));
                self->d->downloadView->add_DownloadStarting(downloadStarting.get(),&self->d->downloadToken);
            }
            self->d->initializing = false;
            self->d->ready = true;
            self->d->status->hide();
            self->updateBounds();
            controller->put_IsVisible(self->isVisible());
            const QUrl pending = self->d->pendingUrl;
            const quint64 revision = self->d->navigationRevision;
            const auto eventNames = self->d->eventNames;
            for (const auto& eventName : eventNames) {
                self->subscribeDevToolsEvent(eventName);
                if (lifetime.expired() || !self) return S_OK;
            }
            emit self->readyChanged(true);
            if (!lifetime.expired() && self && self->d->navigationRevision == revision)
                self->navigate(pending.isEmpty() ? QUrl(QStringLiteral("about:blank")) : pending);
            return S_OK;
        }));
        const HRESULT controllerResult = environment->CreateCoreWebView2Controller(
            reinterpret_cast<HWND>(self->winId()), controllerCallback.get());
        if (!lifetime.expired() && self && FAILED(controllerResult)) self->reportError(tr("Cannot initialize the browser window"), controllerResult);
        return S_OK;
    }));
    const auto folder = QDir::toNativeSeparators(dataDir).toStdWString();
    ComPtr<ICoreWebView2EnvironmentOptions> options;
    if(d->localContent)options=ComPtr<ICoreWebView2EnvironmentOptions>(new webview::AppOptions);
    const HRESULT result = createEnvironment(nullptr, folder.c_str(), options.get(), callback.get());
    if (FAILED(result)) reportError(tr("Cannot initialize WebView2"), result);
#else
    reportError(tr("This browser backend currently requires Windows"), -1);
#endif
}

void BrowserWidget::navigate(const QUrl& url) {
    if (!url.isValid() || url.isEmpty()) { emit errorOccurred(tr("Enter a valid page address")); return; }
    if (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")
        && url.scheme() != QStringLiteral("about") && url.scheme() != QStringLiteral("file")
        && url.scheme() != QStringLiteral("data")
        && !(d->localContent&&(url.scheme()==QStringLiteral("app")||url.scheme()==QStringLiteral("ade-app")))) {
        emit errorOccurred(tr("This page address uses an unsupported scheme"));
        return;
    }
    d->pendingUrl = url;
    ++d->navigationRevision;
#ifdef Q_OS_WIN
    if (d->view) {
        const auto address = url.toString(QUrl::FullyEncoded).toStdWString();
        const HRESULT result = d->view->Navigate(address.c_str());
        if (FAILED(result)) reportError(tr("Cannot navigate to the page"), result);
    } else if (isVisible()) initialize();
#endif
}

void BrowserWidget::back() {
#ifdef Q_OS_WIN
    if (d->view) { const HRESULT result = d->view->GoBack(); if (FAILED(result)) reportError(tr("Cannot go back"), result); }
#endif
}
void BrowserWidget::forward() {
#ifdef Q_OS_WIN
    if (d->view) { const HRESULT result = d->view->GoForward(); if (FAILED(result)) reportError(tr("Cannot go forward"), result); }
#endif
}
void BrowserWidget::reload() {
#ifdef Q_OS_WIN
    if (d->view) { const HRESULT result = d->view->Reload(); if (FAILED(result)) reportError(tr("Cannot reload the page"), result); }
    else if (isVisible()) initialize();
#endif
}
void BrowserWidget::stop() {
#ifdef Q_OS_WIN
    if (d->view) { const HRESULT result = d->view->Stop(); if (FAILED(result)) reportError(tr("Cannot stop loading"), result); }
#endif
}

void BrowserWidget::executeScript(const QString& script) {
#ifdef Q_OS_WIN
    if (!d->view) { emit errorOccurred(tr("The browser is not ready")); return; }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<ScriptCallback> callback(new ScriptCallback([self, lifetime](HRESULT result, LPCWSTR json) -> HRESULT {
        if (lifetime.expired() || !self) return S_OK;
        if (FAILED(result)) self->reportError(tr("Page script failed"), result);
        else emit self->scriptResult(QString::fromWCharArray(json ? json : L"null"));
        return S_OK;
    }));
    const auto text = script.toStdWString();
    const HRESULT result = d->view->ExecuteScript(text.c_str(), callback.get());
    if (FAILED(result)) reportError(tr("Cannot execute the page script"), result);
#else
    Q_UNUSED(script)
#endif
}

void BrowserWidget::callDevTools(const QString& method, const QString& json) {
    callDevToolsRequest(QString(), method, json);
}

void BrowserWidget::callDevToolsRequest(const QString& requestId, const QString& method, const QString& json) {
#ifdef Q_OS_WIN
    const auto failRequest = [this, requestId](const QString& message) {
        if (requestId.isEmpty()) emit errorOccurred(message);
        else emit devToolsRequestCompleted(requestId, message, true);
    };
    if (!d->view) { failRequest(tr("The browser is not ready")); return; }
    QJsonParseError parseError;
    const auto parameters = QJsonDocument::fromJson(json.toUtf8(), &parseError);
    if (method.isEmpty() || parseError.error != QJsonParseError::NoError || !parameters.isObject()) {
        failRequest(tr("DevTools requires a method name and a JSON object")); return;
    }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<DevToolsCallback> callback(new DevToolsCallback([self, lifetime, requestId](HRESULT result, LPCWSTR response) -> HRESULT {
        if (lifetime.expired() || !self) return S_OK;
        if (!requestId.isEmpty()) {
            const QString output = FAILED(result)
                ? tr("DevTools command failed (0x%1)").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'))
                : QString::fromWCharArray(response ? response : L"{}");
            emit self->devToolsRequestCompleted(requestId, output, FAILED(result));
        } else if (FAILED(result)) self->reportError(tr("DevTools command failed"), result);
        else emit self->devToolsResult(QString::fromWCharArray(response ? response : L"{}"));
        return S_OK;
    }));
    const auto name = method.toStdWString();
    const auto arguments = json.toStdWString();
    const HRESULT result = d->view->CallDevToolsProtocolMethod(name.c_str(), arguments.c_str(), callback.get());
    if (FAILED(result)) failRequest(tr("Cannot send the DevTools command (0x%1)").arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0')));
#else
    Q_UNUSED(requestId)
    Q_UNUSED(method)
    Q_UNUSED(json)
#endif
}

void BrowserWidget::subscribeDevToolsEvent(const QString& eventName) {
    if (eventName.isEmpty() || eventName.size() > 200) return;
    d->eventNames.insert(eventName);
#ifdef Q_OS_WIN
    if (!d->ready || d->subscriptions.contains(eventName)) return;
    Impl::DevToolsSubscription subscription;
    const auto name = eventName.toStdWString();
    HRESULT result = d->view->GetDevToolsProtocolEventReceiver(name.c_str(), subscription.receiver.put());
    if (FAILED(result)) { reportError(tr("Cannot subscribe to the DevTools event"), result); return; }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<DevToolsEventCallback> callback(new DevToolsEventCallback(
        [self, lifetime, eventName](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args) -> HRESULT {
            if (lifetime.expired() || !self) return S_OK;
            LPWSTR parameters = nullptr;
            if (SUCCEEDED(args->get_ParameterObjectAsJson(&parameters)))
                emit self->devToolsEventReceived(eventName, takeComString(parameters));
            return S_OK;
        }));
    result = subscription.receiver->add_DevToolsProtocolEventReceived(callback.get(), &subscription.token);
    if (FAILED(result)) { reportError(tr("Cannot attach the DevTools event handler"), result); return; }
    d->subscriptions.insert(eventName, std::move(subscription));
#endif
}
void BrowserWidget::unsubscribeDevToolsEvent(const QString& eventName) {
    d->eventNames.remove(eventName);
#ifdef Q_OS_WIN
    auto it=d->subscriptions.find(eventName);if(it==d->subscriptions.end())return;
    auto subscription=it.value();d->subscriptions.erase(it);
    subscription.receiver->remove_DevToolsProtocolEventReceived(subscription.token);
#endif
}

void BrowserWidget::requestScreenshot() {
#ifdef Q_OS_WIN
    if (!d->view) { emit errorOccurred(tr("The browser is not ready")); return; }
    ComPtr<IStream> stream;
    const HRESULT streamResult = CreateStreamOnHGlobal(nullptr, TRUE, stream.put());
    if (FAILED(streamResult)) { reportError(tr("Cannot create the screenshot buffer"), streamResult); return; }
    const QPointer<BrowserWidget> self(this);
    const std::weak_ptr<int> lifetime(d->lifetime);
    ComPtr<CaptureCallback> callback(new CaptureCallback([self, lifetime, stream](HRESULT result) -> HRESULT {
        if (lifetime.expired() || !self) return S_OK;
        if (FAILED(result)) { self->reportError(tr("Cannot capture the page"), result); return S_OK; }
        STATSTG stat{};
        HRESULT readResult = stream->Stat(&stat, STATFLAG_NONAME);
        constexpr quint64 maxBytes = 64ull * 1024 * 1024;
        if (FAILED(readResult) || stat.cbSize.QuadPart > maxBytes) {
            self->reportError(tr("Screenshot buffer is invalid or too large"), FAILED(readResult) ? readResult : E_OUTOFMEMORY);
            return S_OK;
        }
        LARGE_INTEGER start{};
        readResult = stream->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(readResult)) { self->reportError(tr("Cannot read the screenshot"), readResult); return S_OK; }
        QByteArray png(static_cast<qsizetype>(stat.cbSize.QuadPart), Qt::Uninitialized);
        ULONG read = 0;
        readResult = stream->Read(png.data(), static_cast<ULONG>(png.size()), &read);
        if (FAILED(readResult) || read != static_cast<ULONG>(png.size())) {
            self->reportError(tr("Cannot read the complete screenshot"), FAILED(readResult) ? readResult : E_FAIL);
            return S_OK;
        }
        emit self->screenshotReady(png);
        return S_OK;
    }));
    const HRESULT result = d->view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.get(), callback.get());
    if (FAILED(result)) reportError(tr("Cannot start the screenshot capture"), result);
#endif
}

void BrowserWidget::openDevTools() {
#ifdef Q_OS_WIN
    if (d->view) { const HRESULT result = d->view->OpenDevToolsWindow(); if (FAILED(result)) reportError(tr("Cannot open DevTools"), result); }
#endif
}

void BrowserWidget::updateBounds() {
#ifdef Q_OS_WIN
    if (d->controller) {
        // resizeEvent can precede the native HWND's size update. Derive physical
        // pixels from Qt's new logical size instead of reading stale HWND bounds.
        const qreal scale = devicePixelRatioF();
        RECT bounds{0, 0, qRound(width() * scale), qRound(height() * scale)};
        d->controller->put_Bounds(bounds);
        d->controller->NotifyParentWindowPositionChanged();
    }
#endif
}
void BrowserWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const std::weak_ptr<int> lifetime(d->lifetime);
    initialize();
    // An initialization error signal may synchronously close/delete its host.
    if (lifetime.expired()) return;
#ifdef Q_OS_WIN
    if (d->controller) d->controller->put_IsVisible(TRUE);
#endif
    updateBounds();
}
void BrowserWidget::hideEvent(QHideEvent* event) {
#ifdef Q_OS_WIN
    if (d->controller) d->controller->put_IsVisible(FALSE);
#endif
    QWidget::hideEvent(event);
}
void BrowserWidget::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); updateBounds(); }
void BrowserWidget::moveEvent(QMoveEvent* event) { QWidget::moveEvent(event); updateBounds(); }
void BrowserWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
#ifdef Q_OS_WIN
    if (d->controller) d->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
#endif
}

} // namespace ade
