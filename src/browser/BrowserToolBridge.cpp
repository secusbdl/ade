#include "BrowserToolBridge.h"
#include "browserwidget.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <utility>

namespace ade {
namespace {
constexpr qsizetype MaxResponseChars = 2 * 1024 * 1024;
constexpr qsizetype MaxMessageChars = 16384;
constexpr qsizetype MaxStoredLogs = 1000;

QString compact(const QJsonObject& object) { return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)); }
QJsonObject stringProperty(const QString& description) { return {{"type", "string"}, {"description", description}}; }
QJsonObject definition(const QString& name, const QString& description, const QJsonObject& properties, const QJsonArray& required) {
    const QJsonObject parameters{{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
    const QJsonObject function{{"name", name}, {"description", description}, {"parameters", parameters}};
    return {{"type", "function"}, {"function", function}};
}
QString jsonValueText(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}
QString stackText(const QJsonObject& stack) {
    QStringList frames;
    for (const auto& frameValue : stack.value("callFrames").toArray()) {
        const auto frame = frameValue.toObject();
        frames.append(QStringLiteral("%1 (%2:%3:%4)")
            .arg(frame.value("functionName").toString(), frame.value("url").toString())
            .arg(frame.value("lineNumber").toInt() + 1).arg(frame.value("columnNumber").toInt() + 1));
        if (frames.size() == 20) break;
    }
    return frames.join(QLatin1Char('\n')).left(MaxMessageChars);
}
bool validInteger(const QJsonValue& value, int min, int max) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= min
        && value.toDouble() <= max && std::floor(value.toDouble()) == value.toDouble();
}
const QStringList& consoleLevels() {
    static const QStringList levels{"log", "debug", "info", "error", "warning", "dir", "dirxml", "table", "trace",
        "clear", "startGroup", "startGroupCollapsed", "endGroup", "assert", "profile", "profileEnd", "count", "timeEnd"};
    return levels;
}
} // namespace

BrowserToolBridge::BrowserToolBridge(QObject* parent) : QObject(parent) {}
BrowserToolBridge::~BrowserToolBridge() {
    const auto subscriptions=m_subscriptions.keys();for(const auto& id:subscriptions)unsubscribeEvent(id);
    for (const auto& tab : std::as_const(m_tabs))
        for (const auto& connection : tab.connections) disconnect(connection);
    for (const auto& pending : std::as_const(m_pending))
        if (pending.timeout) pending.timeout->stop();
}

QString BrowserToolBridge::registerTab(BrowserWidget* browser, const QString& requestedId) {
    if (!browser || browser->thread() != thread()) return {};
    for (auto it = m_tabs.cbegin(); it != m_tabs.cend(); ++it)
        if (it->browser == browser) return it.key();
    QString id = requestedId;
    if (id.isEmpty()) {
        do { id = QStringLiteral("t_%1").arg(m_nextTabId++); } while (m_usedIds.contains(id));
    }
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (!validId.match(id).hasMatch() || m_usedIds.contains(id)) return {};
    m_usedIds.insert(id);
    Tab tab;
    tab.browser = browser;
    tab.url = browser->currentUrl().toString(QUrl::FullyEncoded);
    tab.connections.append(connect(browser, &QObject::destroyed, this, [this, id] { unregisterTab(id); }));
    tab.connections.append(connect(browser, &BrowserWidget::titleChanged, this, [this, id](const QString& title) {
        if (auto it = m_tabs.find(id); it != m_tabs.end()) { it->title = title; emit tabsChanged(); }
    }));
    tab.connections.append(connect(browser, &BrowserWidget::urlChanged, this, [this, id](const QUrl& url) {
        if (auto it = m_tabs.find(id); it != m_tabs.end()) { it->url = url.toString(QUrl::FullyEncoded); emit tabsChanged(); }
    }));
    tab.connections.append(connect(browser, &BrowserWidget::readyChanged, this, [this, id](bool ready) {
        if (ready) prepareConsole(id);
        emit tabsChanged();
    }));
    tab.connections.append(connect(browser, &BrowserWidget::loadingChanged, this, [this, id](bool loading) {
        if (loading) if (auto it = m_tabs.find(id); it != m_tabs.end()) it->logs.clear();
    }));
    tab.connections.append(connect(browser, &BrowserWidget::devToolsEventReceived, this,
        [this, id](const QString& event, const QString& json) { receiveEvent(id, event, json); }));
    tab.connections.append(connect(browser, &BrowserWidget::devToolsRequestCompleted, this,
        [this, id](const QString& correlation, const QString& output, bool error) {
            const QString requestId = m_correlations.value(correlation);
            if (requestId.isEmpty() || !stillPending(requestId, correlation) || m_pending.value(requestId).tabId != id) return;
            if (output.size() > MaxResponseChars)
                finishJson(requestId, {{"error", "Browser response exceeds 2 MiB; request a narrower DOM/result or a smaller screenshot."}}, true);
            else finish(requestId, output, error);
        }));
    m_tabs.insert(id, std::move(tab));
    m_order.append(id);
    prepareConsole(id);
    emit tabsChanged();
    return id;
}

void BrowserToolBridge::unregisterTab(const QString& tabId) {
    if (!m_tabs.contains(tabId)) return;
    const auto tab = m_tabs.take(tabId);
    for (const auto& connection : tab.connections) disconnect(connection);
    m_order.removeAll(tabId);
    if (m_activeTabId == tabId) m_activeTabId.clear();
    const QPointer<BrowserToolBridge> self(this);
    const auto subscriptions=m_subscriptions.keys();for(const auto& id:subscriptions)if(m_subscriptions.value(id).tabId==tabId){m_subscriptions.remove(id);emit subscriptionEnded(id,QStringLiteral("Browser tab was closed."));if(!self)return;}
    const auto pendingIds = m_pending.keys();
    for (const auto& id : pendingIds) {
        if (m_pending.value(id).tabId == tabId) finishJson(id, {{"error", "Browser tab was closed."}}, true);
        if (!self) return;
    }
    emit tabsChanged();
}

bool BrowserToolBridge::setActiveTab(const QString& tabId) {
    if (tabId.isEmpty()) {
        if (!m_activeTabId.isEmpty()) { m_activeTabId.clear(); emit tabsChanged(); }
        return true;
    }
    if (!m_tabs.contains(tabId) || !m_tabs.value(tabId).browser) return false;
    quint64 order = 0;
    for (const auto& tab : std::as_const(m_tabs)) order = qMax(order, tab.focusOrder);
    auto& tab = m_tabs[tabId];
    tab.focusOrder = order + 1;
    tab.lastFocusedAt = QDateTime::currentMSecsSinceEpoch();
    m_activeTabId = tabId;
    emit tabsChanged();
    return true;
}
void BrowserToolBridge::setRequestTimeoutMs(int milliseconds) { m_timeoutMs = qBound(50, milliseconds, 120000); }
void BrowserToolBridge::subscribeEvent(const QString& id,const QString& tabId,const QString& event){
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z][A-Za-z0-9]*\\.[A-Za-z][A-Za-z0-9]*$"));
    if(id.isEmpty()||id.size()>256||m_subscriptions.contains(id)||m_subscriptions.size()>=256||!valid.match(event).hasMatch()||event.size()>200||!m_tabs.contains(tabId)||!m_tabs.value(tabId).browser){emit subscriptionEnded(id,QStringLiteral("Invalid browser event subscription or missing tab."));return;}
    m_subscriptions.insert(id,{tabId,event});m_tabs.value(tabId).browser->subscribeDevToolsEvent(event);
}
void BrowserToolBridge::unsubscribeEvent(const QString& id){
    if(!m_subscriptions.contains(id))return;
    const auto subscription=m_subscriptions.take(id);
    for(const auto& other:std::as_const(m_subscriptions))if(other.tabId==subscription.tabId&&other.eventName==subscription.eventName)return;
    if(QStringList{"Runtime.consoleAPICalled","Runtime.exceptionThrown","Log.entryAdded"}.contains(subscription.eventName))return;
    if(auto* browser=m_tabs.value(subscription.tabId).browser.data())browser->unsubscribeDevToolsEvent(subscription.eventName);
}
void BrowserToolBridge::receiveEvent(const QString& tabId,const QString& event,const QString& json){
    const QPointer<BrowserToolBridge> self(this);receiveConsole(tabId,event,json);if(!self)return;
    const auto ids=m_subscriptions.keys();QJsonParseError error;const auto document=QJsonDocument::fromJson(json.toUtf8(),&error);
    for(const auto& id:ids){const auto it=m_subscriptions.constFind(id);if(it==m_subscriptions.cend()||it->tabId!=tabId||it->eventName!=event)continue;
        if(json.size()>256*1024||error.error!=QJsonParseError::NoError||!document.isObject()){unsubscribeEvent(id);emit subscriptionEnded(id,QStringLiteral("Browser event is invalid or exceeds 256 KiB."));}
        else emit eventReceived(id,document.object());
        if(!self)return;
    }
}
bool BrowserToolBridge::handles(const QString& name) const {
    return name == QStringLiteral("getBrowserTabs") || name == QStringLiteral("sendCDP") || name == QStringLiteral("readConsoleLogs");
}

QJsonArray BrowserToolBridge::definitions() const {
    QJsonArray levels;
    for (const auto& level : consoleLevels()) levels.append(level);
    return {
        definition("getBrowserTabs", "List ADE's registered browser tabs and active tab ID. Use these stable tab IDs for sendCDP and readConsoleLogs. This is native tab discovery.", {}, {}),
        definition("sendCDP", "Send one Chrome DevTools Protocol command to an ADE browser tab. Native exposure of Stagewise API.sendCDP; this does not execute Node.js sandbox code. Use Runtime.evaluate for page JavaScript/DOM inspection (awaitPromise supported), Page.navigate for navigation, Page.getNavigationHistory and Page.navigateToHistoryEntry for history, and Page.captureScreenshot for base64 image data. Returns raw protocol JSON, limited to 2 MiB. Actions run in the selected page; cancellation stops waiting but cannot undo an issued browser action.",
            {{"tabId", stringProperty("Stable tab ID from getBrowserTabs.")},
             {"method", stringProperty("CDP method, for example Runtime.evaluate or Page.navigate.")},
             {"params", QJsonObject{{"type", "object"}, {"description", "CDP method parameters; default {}."}, {"additionalProperties", true}}}}, {"tabId", "method"}),
        definition("readConsoleLogs", "Read captured browser console logs, newest first. Logs are cleared on navigation. Matches Stagewise readConsoleLogs arguments; capture begins when ADE registers the tab. Supports a delay before reading for asynchronous logging.",
            {{"id", stringProperty("Stable browser tab ID.")},
             {"filter", stringProperty("Optional case-insensitive message substring.")},
             {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"description", "Maximum returned logs, default 50."}}},
             {"levels", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}, {"enum", levels}}}}},
             {"delayMs", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 5000}, {"description", "Wait before reading, default 0."}}}}, {"id"})
    };
}

bool BrowserToolBridge::canAccessTab(const QString& tabId,const QString& agentId) const{
    const auto* browser=m_tabs.value(tabId).browser.data();if(!browser)return false;
    const QString owner=browser->property("adeOwnerAgentId").toString();return owner.isEmpty()||(!agentId.isEmpty()&&owner==agentId);
}
BrowserContextSnapshot BrowserToolBridge::snapshotForAgent(const QString& agentId) const {
    BrowserContextSnapshot snapshot;
    if (QThread::currentThread() != thread() || agentId.trimmed().isEmpty() ||
        agentId.size() > 256 || agentId.contains(QChar::Null)) return snapshot;
    snapshot.available = true;
    if (canAccessTab(m_activeTabId, agentId)) snapshot.activeTabId = m_activeTabId;
    for (const auto& id : m_order) {
        if (!canAccessTab(id, agentId)) continue;
        const auto& tab = m_tabs[id];
        snapshot.tabs.append({id, tab.title, tab.url, tab.lastFocusedAt, tab.focusOrder});
    }
    // Stable ties preserve registration order, including never-focused tabs.
    std::stable_sort(snapshot.tabs.begin(), snapshot.tabs.end(), [](const auto& a, const auto& b) {
        return a.focusOrder > b.focusOrder;
    });
    if (snapshot.tabs.size() > BrowserContextMaxTabs) {
        snapshot.omittedTabs = snapshot.tabs.size() - BrowserContextMaxTabs;
        snapshot.tabs.resize(BrowserContextMaxTabs);
    }
    return snapshot;
}
QJsonObject BrowserToolBridge::tabsSnapshot(const QString& agentId) const {
    QJsonArray tabs;
    for (const auto& id : m_order) {
        const auto tab = m_tabs.value(id);
        if (!tab.browser||(!agentId.isEmpty()&&!canAccessTab(id,agentId))) continue;
        tabs.append(QJsonObject{{"id", id}, {"url", tab.browser->currentUrl().toString()}, {"title", tab.title},
            {"ready", tab.browser->isReady()}, {"active", id == m_activeTabId}});
    }
    return {{"activeTabId",agentId.isEmpty()||canAccessTab(m_activeTabId,agentId)?m_activeTabId:QString()}, {"tabs", tabs}};
}

bool BrowserToolBridge::stillPending(const QString& id, const QString& correlation) const {
    const auto it = m_pending.constFind(id);
    return it != m_pending.cend() && it->correlation == correlation;
}

void BrowserToolBridge::execute(const QString& id, const QString& name, const QJsonObject& arguments) {
    executeScoped(id,{},name,arguments);
}
void BrowserToolBridge::executeForAgent(const QString& id,const QString& agentId,const QString& name,const QJsonObject& arguments){
    if(agentId.isEmpty()||agentId.size()>256){emit completed(id,compact({{"error","Invalid browser agent identity."}}),true);return;}
    executeScoped(id,agentId,name,arguments);
}
void BrowserToolBridge::executeScoped(const QString& id,const QString& agentId,const QString& name,const QJsonObject& arguments){
    if (m_pending.contains(id)) { finishJson(id, {{"error", "Duplicate browser request ID; the original request was canceled."}}, true); return; }
    if (id.isEmpty() || id.size() > 512 || m_pending.size() >= 64) {
        emit completed(id, compact({{"error", "Invalid request ID or too many pending browser requests."}}), true);
        return;
    }
    Pending pending;
    pending.correlation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.timeout = new QTimer(this);
    pending.timeout->setSingleShot(true);
    const QString correlation = pending.correlation;
    connect(pending.timeout, &QTimer::timeout, this, [this, id, correlation] {
        if (stillPending(id, correlation)) finishJson(id, {{"error", "Browser request timed out. An already issued page action may continue."}}, true);
    });
    pending.timeout->start(m_timeoutMs);
    m_pending.insert(id, pending);
    m_correlations.insert(correlation, id);
    QTimer::singleShot(0, this, [this, id, agentId,name, arguments, correlation] {
        if (!stillPending(id, correlation)) return;
        if (!handles(name)) { finishJson(id, {{"error", "Unknown native browser tool."}}, true); return; }
        if (name == QStringLiteral("getBrowserTabs")) { finishJson(id, tabsSnapshot(agentId)); return; }
        const QString tabId = arguments.value(name == QStringLiteral("readConsoleLogs") ? "id" : "tabId").toString();
        auto tab = m_tabs.find(tabId);
        if (tab == m_tabs.end() || !tab->browser) { finishJson(id, {{"error", "Unknown or closed browser tab; call getBrowserTabs."}}, true); return; }
        if(!agentId.isEmpty()&&!canAccessTab(tabId,agentId)){finishJson(id,{{"error","Browser app tab belongs to another agent."}},true);return;}
        m_pending[id].tabId = tabId;
        if (!tab->browser->isReady()) { finishJson(id, {{"error", "Browser tab is still starting."}}, true); return; }
        if (name == QStringLiteral("sendCDP")) {
            const QString method = arguments.value("method").toString();
            static const QRegularExpression methodPattern(QStringLiteral("^[A-Za-z][A-Za-z0-9]*\\.[A-Za-z][A-Za-z0-9]*$"));
            if (!methodPattern.match(method).hasMatch() || method.size() > 200
                || (arguments.contains("params") && !arguments.value("params").isObject())) {
                finishJson(id, {{"error", "sendCDP requires a valid Domain.method and an object for params."}}, true); return;
            }
            const auto json = compact(arguments.value("params").toObject());
            if (json.size() > MaxResponseChars) { finishJson(id, {{"error", "CDP parameters exceed 2 MiB."}}, true); return; }
            tab->browser->callDevToolsRequest(correlation, method, json);
            return;
        }
        if ((arguments.contains("limit") && !validInteger(arguments.value("limit"), 1, 500))
            || (arguments.contains("delayMs") && !validInteger(arguments.value("delayMs"), 0, 5000))
            || (arguments.contains("filter") && !arguments.value("filter").isString())
            || (arguments.contains("levels") && !arguments.value("levels").isArray())) {
            finishJson(id, {{"error", "Invalid console filter, limit (1-500), levels, or delayMs (0-5000)."}}, true); return;
        }
        for (const auto& level : arguments.value("levels").toArray()) {
            if (!level.isString() || !consoleLevels().contains(level.toString())) {
                finishJson(id, {{"error", "Unknown console log level."}}, true); return;
            }
        }
        QTimer::singleShot(arguments.value("delayMs").toInt(), this, [this, id, tabId, arguments, correlation] {
            if (stillPending(id, correlation)) readConsole(id, tabId, arguments);
        });
    });
}

void BrowserToolBridge::finish(const QString& id, const QString& output, bool error) {
    if (!m_pending.contains(id)) return;
    const auto pending = m_pending.take(id);
    m_correlations.remove(pending.correlation);
    if (pending.timeout) { pending.timeout->stop(); pending.timeout->deleteLater(); }
    if (output.size() > MaxResponseChars)
        emit completed(id, compact({{"error", "Browser output exceeds 2 MiB; use a narrower query or fewer console logs."}}), true);
    else emit completed(id, output, error);
}
void BrowserToolBridge::finishJson(const QString& id, const QJsonObject& output, bool error) { finish(id, compact(output), error); }
void BrowserToolBridge::cancel(const QString& id) {
    finishJson(id, {{"error", "Browser request canceled. An already issued page action may continue."}}, true);
}
void BrowserToolBridge::cancelAll() {
    const auto ids = m_pending.keys();
    const QPointer<BrowserToolBridge> self(this);
    for (const auto& id : ids) { cancel(id); if (!self) return; }
}

void BrowserToolBridge::prepareConsole(const QString& tabId) {
    auto tab = m_tabs.find(tabId);
    if (tab == m_tabs.end() || !tab->browser) return;
    tab->browser->subscribeDevToolsEvent(QStringLiteral("Runtime.consoleAPICalled"));
    tab->browser->subscribeDevToolsEvent(QStringLiteral("Runtime.exceptionThrown"));
    tab->browser->subscribeDevToolsEvent(QStringLiteral("Log.entryAdded"));
    if (!tab->browser->isReady()) return;
    tab->browser->callDevToolsRequest(QStringLiteral("console-enable-runtime-") + tabId, QStringLiteral("Runtime.enable"));
    tab->browser->callDevToolsRequest(QStringLiteral("console-enable-log-") + tabId, QStringLiteral("Log.enable"));
}

void BrowserToolBridge::receiveConsole(const QString& tabId, const QString& event, const QString& json) {
    auto tab = m_tabs.find(tabId);
    if (tab == m_tabs.end() || !tab->browser) return;
    const auto params = QJsonDocument::fromJson(json.toUtf8()).object();
    QString level, message, stack;
    double timestamp = params.value("timestamp").toDouble(QDateTime::currentMSecsSinceEpoch());
    if (event == QStringLiteral("Runtime.consoleAPICalled")) {
        level = params.value("type").toString(QStringLiteral("log"));
        QStringList pieces;
        for (const auto& value : params.value("args").toArray()) {
            const auto arg = value.toObject();
            pieces.append(arg.contains("value") ? jsonValueText(arg.value("value"))
                : arg.value("unserializableValue").toString(arg.value("description").toString()));
        }
        message = pieces.join(QLatin1Char(' '));
        stack = stackText(params.value("stackTrace").toObject());
    } else if (event == QStringLiteral("Runtime.exceptionThrown")) {
        level = QStringLiteral("error");
        const auto details = params.value("exceptionDetails").toObject();
        message = details.value("exception").toObject().value("description").toString(details.value("text").toString());
        stack = stackText(details.value("stackTrace").toObject());
    } else if (event == QStringLiteral("Log.entryAdded")) {
        const auto entry = params.value("entry").toObject();
        level = entry.value("level").toString(QStringLiteral("log"));
        if (level == QStringLiteral("verbose")) level = QStringLiteral("debug");
        message = entry.value("text").toString();
        timestamp = entry.value("timestamp").toDouble(timestamp);
        stack = stackText(entry.value("stackTrace").toObject());
    } else return;
    if (!std::isfinite(timestamp) || timestamp < 0 || timestamp > 253402300799999.0)
        timestamp = QDateTime::currentMSecsSinceEpoch();
    if (level == QStringLiteral("warn")) level = QStringLiteral("warning");
    QJsonObject log{{"time", QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(timestamp)).toUTC().toString(Qt::ISODateWithMs)},
        {"level", level}, {"message", message.left(MaxMessageChars)}, {"pageUrl", tab->browser->currentUrl().toString()}};
    if (!stack.isEmpty()) log.insert("stackTrace", stack);
    tab->logs.append(log);
    if (tab->logs.size() > MaxStoredLogs) tab->logs.removeFirst();
}

void BrowserToolBridge::readConsole(const QString& id, const QString& tabId, const QJsonObject& arguments) {
    const auto tab = m_tabs.constFind(tabId);
    if (tab == m_tabs.cend() || !tab->browser) { finishJson(id, {{"error", "Browser tab was closed."}}, true); return; }
    const QString filter = arguments.value("filter").toString();
    const auto levels = arguments.value("levels").toArray();
    const int limit = arguments.value("limit").toInt(50);
    QJsonArray logs;
    for (auto it = tab->logs.crbegin(); it != tab->logs.crend() && logs.size() < limit; ++it) {
        if (!filter.isEmpty() && !it->value("message").toString().contains(filter, Qt::CaseInsensitive)) continue;
        if (!levels.isEmpty() && !levels.contains(it->value("level"))) continue;
        logs.append(*it);
    }
    const QJsonObject result{{"logsReturned", logs.size()}, {"totalLogsStored", tab->logs.size()},
        {"filter", filter.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(filter)},
        {"levels", levels.isEmpty() ? QJsonValue(QStringLiteral("all")) : QJsonValue(levels)},
        {"delayMs", arguments.value("delayMs").toInt() > 0 ? QJsonValue(arguments.value("delayMs").toInt()) : QJsonValue(QJsonValue::Null)},
        {"logs", logs}};
    finishJson(id, {{"message", QStringLiteral("Found %1 console log(s) (%2 total stored)").arg(logs.size()).arg(tab->logs.size())},
        {"result", compact(result)}});
}
} // namespace ade
