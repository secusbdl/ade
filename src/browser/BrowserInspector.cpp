#include "BrowserInspector.h"
#include "browserwidget.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <cmath>

namespace ade {
namespace {
constexpr int MaxReply = 2 * 1024 * 1024;
const char CaptureElement[] = R"JS(function() {
  const el=this;
  if (!el || el.nodeType!==1 || !el.isConnected) throw new Error('The selected element is no longer attached.');
  const doc=el.ownerDocument, view=doc.defaultView;
  const take=(value,n=8192)=>String(value??'').slice(0,n);
  const attributes=node=>Object.fromEntries(Array.from(node.attributes||[]).slice(0,64)
    .filter(a=>!['value','srcdoc'].includes(a.name.toLowerCase())).map(a=>[take(a.name,128),take(a.value,2048)]));
  const properties=['fontFamily','fontSize','fontWeight','lineHeight','letterSpacing','color','textAlign',
    'padding','margin','width','height','maxWidth','minWidth','maxHeight','minHeight','backgroundColor',
    'backgroundImage','border','borderRadius','display','position','top','right','bottom','left','zIndex',
    'flexDirection','alignItems','justifyContent','gap','flexWrap','gridTemplateColumns','gridTemplateRows',
    'gridColumn','gridRow','boxShadow','opacity','overflow','filter','backdropFilter','transform','transition',
    'animation','cursor','visibility','pointerEvents'];
  const styles=pseudo=>{const css=view.getComputedStyle(el,pseudo);return Object.fromEntries(properties
    .concat(pseudo?['content']:[]).map(k=>[k,take(css[k],1024)]));};
  const path=[];let node=el, depth=0;
  while(node&&node.nodeType===1&&depth++<32){let index=1,p=node.previousElementSibling;
    while(p){if(p.tagName===node.tagName)index++;p=p.previousElementSibling;}
    path.unshift(node.localName+'['+index+']');node=node.parentElement;}
  const rect=el.getBoundingClientRect();
  const match=s=>{try{return el.matches(s);}catch{return false;}};
  return {tagName:el.tagName.toLowerCase(),nodeType:el.tagName.toLowerCase(),xpath:'/'+path.join('/'),
    attributes:attributes(el),innerText:take(el.innerText||el.textContent,16384),
    boundingClientRect:{x:rect.x,y:rect.y,width:rect.width,height:rect.height},computedStyles:styles(),
    pseudoElements:{before:styles('::before'),after:styles('::after')},
    interactionState:{hover:match(':hover'),active:match(':active'),focus:match(':focus'),focusWithin:match(':focus-within')},
    parent:el.parentElement?{tagName:el.parentElement.tagName.toLowerCase(),attributes:attributes(el.parentElement)}:undefined,
    siblings:Array.from(el.parentElement?.children||[]).filter(n=>n!==el).slice(0,64).map(n=>({tagName:n.tagName.toLowerCase()})),
    children:Array.from(el.children).slice(0,64).map(n=>({tagName:n.tagName.toLowerCase()})),
    frameLocation:take(doc.location.href),frameTitle:take(doc.title,1024),isMainFrame:view===view.top,
    captureLimits:{textCharacters:16384,attributes:64,children:64,siblings:64}};
})JS";
bool integer(const QJsonValue &v, int min, int max) {
  return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= min &&
         v.toDouble() <= max && std::floor(v.toDouble()) == v.toDouble();
}
} // namespace

BrowserInspector::BrowserInspector(BrowserWidget *browser, QObject *parent)
    : QObject(parent), m_browser(browser) {
  if (!browser || browser->isClosing()) {
    m_browser = nullptr;
    return;
  }
  connect(browser, &BrowserWidget::aboutToBeDestroyed, this, [this] {
    m_browser = nullptr;
    resetForNavigation();
  });
  connect(browser, &BrowserWidget::loadingChanged, this, [this](bool loading) {
    if (loading)
      resetForNavigation();
  });
  connect(browser, &QObject::destroyed, this, [this] {
    m_browser = nullptr;
    resetForNavigation();
  });
  connect(browser, &BrowserWidget::devToolsRequestCompleted, this,
          [this](const QString &id, const QString &result, bool error) {
            auto it = m_pending.find(id);
            if (it == m_pending.end())
              return;
            auto request = m_pending.take(id);
            if (request.timer) {
              request.timer->stop();
              request.timer->deleteLater();
            }
            if (request.epoch != m_epoch)
              return;
            QJsonParseError parse;
            const auto doc = QJsonDocument::fromJson(result.toUtf8(), &parse);
            const auto obj = doc.object();
            QString failure;
            if (error)
              failure = result.left(1024);
            else if (result.size() > MaxReply)
              failure = "Inspection response is too large.";
            else if (parse.error != QJsonParseError::NoError || !doc.isObject())
              failure = "Invalid browser inspection response.";
            else if (obj.contains("error") || obj.contains("exceptionDetails"))
              failure = "Browser inspection could not complete: " +
                        QString::fromUtf8(
                            QJsonDocument(obj).toJson(QJsonDocument::Compact))
                            .left(1024);
            request.reply(obj, failure);
          });
  browser->subscribeDevToolsEvent("Overlay.inspectNodeRequested", this);
  browser->subscribeDevToolsEvent("Overlay.inspectModeCanceled", this);
  connect(browser, &BrowserWidget::devToolsEventReceived, this,
          [this](const QString &event, const QString &json) {
            if (!m_selecting || json.size() > 8192)
              return;
            if (event == "Overlay.inspectModeCanceled") {
              setSelecting(false);
              return;
            }
            if (event != "Overlay.inspectNodeRequested")
              return;
            const auto node = QJsonDocument::fromJson(json.toUtf8())
                                  .object()
                                  .value("backendNodeId");
            if (integer(node, 1, 2147483647))
              inspectNode(node.toInt());
          });
}
BrowserInspector::~BrowserInspector() {
  for (const auto &request : std::as_const(m_pending))
    if (request.timer)
      request.timer->stop();
  if (m_browser) m_browser->releaseDevToolsEvents(this);
  if (m_browser && m_selecting)
    m_browser->callDevTools("Overlay.setInspectMode", "{\"mode\":\"none\"}");
}
void BrowserInspector::call(const QString &method,
                            const QJsonObject &parameters, Reply reply) {
  if (!m_browser || !m_browser->isReady()) {
    reply({}, "The browser is not ready.");
    return;
  }
  const QString id =
      "inspector-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto *timer = new QTimer(this);
  timer->setSingleShot(true);
  m_pending.insert(id, {std::move(reply), timer, m_epoch});
  connect(timer, &QTimer::timeout, this, [this, id] {
    if (!m_pending.contains(id))
      return;
    auto request = m_pending.take(id);
    if (request.timer)
      request.timer->deleteLater();
    request.reply({}, "Browser inspection timed out.");
  });
  timer->start(10000);
  m_browser->callDevToolsRequest(
      id, method,
      QString::fromUtf8(
          QJsonDocument(parameters).toJson(QJsonDocument::Compact)));
}
void BrowserInspector::resetForNavigation() {
  const auto epoch = ++m_epoch;
  for (const auto &request : std::as_const(m_pending))
    if (request.timer) {
      request.timer->stop();
      request.timer->deleteLater();
    }
  m_pending.clear();
  const bool selected = m_selecting, busy = m_busy;
  m_selecting = false;
  m_busy = false;
  const QPointer<BrowserInspector> self(this);
  if (selected)
    emit selectingChanged(false);
  if (!self || m_epoch != epoch)
    return;
  if (busy)
    emit busyChanged(false);
}
void BrowserInspector::emitFailure(const QString &error) {
  if (!error.isEmpty())
    emit failed(error);
}
void BrowserInspector::finishSelection(const QString &error) {
  const auto epoch = ++m_epoch;
  for (const auto &request : std::as_const(m_pending))
    if (request.timer) {
      request.timer->stop();
      request.timer->deleteLater();
    }
  m_pending.clear();
  const bool selecting = m_selecting, busy = m_busy;
  m_selecting = false;
  m_busy = false;
  const QPointer<BrowserInspector> self(this);
  if (m_browser)
    m_browser->callDevTools("Overlay.setInspectMode", "{\"mode\":\"none\"}");
  if (!self || m_epoch != epoch)
    return;
  if (selecting)
    emit selectingChanged(false);
  if (!self || m_epoch != epoch)
    return;
  if (busy)
    emit busyChanged(false);
  if (self && m_epoch == epoch)
    emitFailure(error);
}
void BrowserInspector::setSelecting(bool enabled) {
  if (!enabled) {
    finishSelection();
    return;
  }
  if (m_selecting || m_busy)
    return;
  const auto epoch = ++m_epoch;
  m_selectionContext = property("captureAgentId").toString();
  m_busy = true;
  const QPointer<BrowserInspector> self(this);
  emit busyChanged(true);
  if (!self || m_epoch != epoch || !m_busy)
    return;
  call("DOM.enable", {}, [this](const QJsonObject &, const QString &error) {
    if (!error.isEmpty()) {
      finishSelection(error);
      return;
    }
    call("Overlay.enable", {},
         [this](const QJsonObject &, const QString &error) {
           if (!error.isEmpty()) {
             finishSelection(error);
             return;
           }
           const QJsonObject color{
               {"r", 65}, {"g", 105}, {"b", 255}, {"a", 0.22}};
           const QJsonObject highlight{{"showInfo", true},
                                       {"showStyles", true},
                                       {"contentColor", color},
                                       {"borderColor", color}};
           call("Overlay.setInspectMode",
                {{"mode", "searchForNode"}, {"highlightConfig", highlight}},
                [this](const QJsonObject &, const QString &error) {
                  if (!error.isEmpty()) {
                    finishSelection(error);
                    return;
                  }
                  m_busy = false;
                  m_selecting = true;
                  const auto epoch = m_epoch;
                  const QPointer<BrowserInspector> self(this);
                  emit busyChanged(false);
                  if (self && m_epoch == epoch && m_selecting)
                    emit selectingChanged(true);
                });
         });
  });
}
void BrowserInspector::inspectNode(int node) {
  if (node <= 0 || m_busy || !m_browser)
    return;
  m_busy = true;
  const auto url = m_browser->currentUrl();
  const auto epoch = ++m_epoch;
  const auto context =
      m_selecting ? m_selectionContext : property("captureAgentId").toString();
  const QPointer<BrowserInspector> self(this);
  emit busyChanged(true);
  if (!self || m_epoch != epoch || !m_busy)
    return;
  call(
      "DOM.resolveNode",
      {{"backendNodeId", node}, {"objectGroup", "ade-element-capture"}},
      [this, url, epoch, context](const QJsonObject &result,
                                  const QString &error) {
        const auto objectId =
            result["object"].toObject()["objectId"].toString();
        if (!error.isEmpty() || objectId.isEmpty()) {
          finishSelection(error.isEmpty()
                              ? "The selected element no longer exists."
                              : error);
          return;
        }
        call(
            "Runtime.callFunctionOn",
            {{"objectId", objectId},
             {"functionDeclaration", QString::fromUtf8(CaptureElement)},
             {"returnByValue", true},
             {"silent", true}},
            [this, url, epoch, context](const QJsonObject &result,
                                        const QString &error) {
              auto element = result["result"].toObject()["value"].toObject();
              const QPointer<BrowserInspector> self(this);
              if (m_browser)
                m_browser->callDevTools(
                    "Runtime.releaseObjectGroup",
                    "{\"objectGroup\":\"ade-element-capture\"}");
              if (!self || m_epoch != epoch)
                return;
              if (!error.isEmpty() || element["tagName"].toString().isEmpty() ||
                  QJsonDocument(element).toJson(QJsonDocument::Compact).size() >
                      256 * 1024) {
                finishSelection(
                    error.isEmpty()
                        ? "The selected element could not be captured."
                        : error);
                return;
              }
              if (epoch != m_epoch || !m_browser ||
                  m_browser->currentUrl() != url) {
                finishSelection("The page changed during element capture.");
                return;
              }
              element.insert("url", url.toString());
              finishSelection();
              if (self && m_epoch == epoch + 1 && !m_busy && !m_selecting)
                emit elementSelected(element, context);
            });
      });
}
QJsonArray BrowserInspector::devicePresets() {
  return {QJsonObject{{"id", "responsive"},
                      {"label", "Responsive"},
                      {"width", 1280},
                      {"height", 720},
                      {"deviceScaleFactor", 1},
                      {"mobile", false}},
          QJsonObject{{"id", "iphone-17"},
                      {"label", "iPhone 17"},
                      {"width", 402},
                      {"height", 874},
                      {"deviceScaleFactor", 3},
                      {"mobile", true}},
          QJsonObject{{"id", "iphone-air"},
                      {"label", "iPhone Air"},
                      {"width", 420},
                      {"height", 912},
                      {"deviceScaleFactor", 3},
                      {"mobile", true}},
          QJsonObject{{"id", "iphone-17-pro-max"},
                      {"label", "iPhone 17 Pro Max"},
                      {"width", 440},
                      {"height", 956},
                      {"deviceScaleFactor", 3},
                      {"mobile", true}},
          QJsonObject{{"id", "iphone-16e"},
                      {"label", "iPhone 16e"},
                      {"width", 390},
                      {"height", 844},
                      {"deviceScaleFactor", 3},
                      {"mobile", true}},
          QJsonObject{{"id", "pixel-10"},
                      {"label", "Pixel 10"},
                      {"width", 412},
                      {"height", 924},
                      {"deviceScaleFactor", 2.625},
                      {"mobile", true}},
          QJsonObject{{"id", "galaxy-s26-ultra"},
                      {"label", "Galaxy S26 Ultra"},
                      {"width", 412},
                      {"height", 892},
                      {"deviceScaleFactor", 3.5},
                      {"mobile", true}},
          QJsonObject{{"id", "ipad-pro-11"},
                      {"label", "iPad Pro 11\""},
                      {"width", 834},
                      {"height", 1210},
                      {"deviceScaleFactor", 2},
                      {"mobile", true}},
          QJsonObject{{"id", "ipad-mini"},
                      {"label", "iPad mini"},
                      {"width", 744},
                      {"height", 1133},
                      {"deviceScaleFactor", 2},
                      {"mobile", true}},
          QJsonObject{{"id", "laptop"},
                      {"label", "Laptop"},
                      {"width", 1440},
                      {"height", 900},
                      {"deviceScaleFactor", 1},
                      {"mobile", false}}};
}
void BrowserInspector::setDevice(const QJsonObject &device) {
  if (m_busy) {
    emitFailure("Finish the current browser operation first.");
    return;
  }
  if (!device.isEmpty() &&
      (!integer(device["width"], 200, 4096) ||
       !integer(device["height"], 200, 4096) || !device["mobile"].isBool() ||
       !device["deviceScaleFactor"].isDouble() ||
       !std::isfinite(device["deviceScaleFactor"].toDouble()) ||
       device["deviceScaleFactor"].toDouble() < 0.5 ||
       device["deviceScaleFactor"].toDouble() > 4)) {
    emitFailure("Invalid device viewport.");
    return;
  }
  const QPointer<BrowserInspector> self(this);
  const auto selectionEpoch = m_epoch;
  if (m_selecting)
    finishSelection();
  if (!self || m_busy || m_selecting || m_epoch > selectionEpoch + 1)
    return;
  const auto epoch = ++m_epoch;
  m_busy = true;
  emit busyChanged(true);
  if (!self || m_epoch != epoch || !m_busy)
    return;
  const auto viewportSize = m_viewportSize;
  const QJsonObject metrics =
      device.isEmpty()
          ? QJsonObject{}
          : QJsonObject{
                {"width", device["width"]},
                {"height", device["height"]},
                {"deviceScaleFactor", device["deviceScaleFactor"]},
                {"mobile", device["mobile"]},
                {"scale",
                 m_viewportSize.isValid()
                     ? qBound(
                           0.1,
                           qMin(double(qMax(1, m_viewportSize.width() - 32)) /
                                    device["width"].toInt(),
                                double(qMax(1, m_viewportSize.height() - 32)) /
                                    device["height"].toInt()),
                           1.0)
                     : 1.0}};
  call(device.isEmpty() ? "Emulation.clearDeviceMetricsOverride"
                        : "Emulation.setDeviceMetricsOverride",
       metrics,
       [this, device, viewportSize, epoch](const QJsonObject &,
                                           const QString &error) {
         if (!error.isEmpty()) {
           m_busy = false;
           const QPointer<BrowserInspector> self(this);
           emit busyChanged(false);
           if (self && m_epoch == epoch)
             emitFailure(error);
           return;
         }
         call("Emulation.setTouchEmulationEnabled",
              {{"enabled", device["mobile"].toBool()}},
              [this, device, viewportSize, epoch](const QJsonObject &,
                                                  const QString &error) {
                m_busy = false;
                m_device = device;
                m_appliedViewportSize = viewportSize;
                const QPointer<BrowserInspector> self(this);
                emit busyChanged(false);
                if (!self || m_epoch != epoch)
                  return;
                emit deviceChanged(device);
                if (self && m_epoch == epoch)
                  emitFailure(error);
              });
       });
}
double BrowserInspector::viewportScale() const {
  if (m_device.isEmpty() || !m_viewportSize.isValid())
    return 1;
  return qBound(0.1,
                qMin(double(qMax(1, m_viewportSize.width() - 32)) /
                         m_device["width"].toInt(),
                     double(qMax(1, m_viewportSize.height() - 32)) /
                         m_device["height"].toInt()),
                1.0);
}
void BrowserInspector::setViewportSize(const QSize &size) {
  if (!size.isValid() ||
      (size == m_viewportSize && size == m_appliedViewportSize))
    return;
  m_viewportSize = size;
  if (!m_device.isEmpty() && !m_busy)
    setDevice(m_device);
}
void BrowserInspector::captureScreenshot() {
  if (m_busy)
    return;
  const auto context = property("captureAgentId").toString();
  const auto epoch = ++m_epoch;
  m_busy = true;
  const QPointer<BrowserInspector> self(this);
  emit busyChanged(true);
  if (!self || m_epoch != epoch || !m_busy)
    return;
  call("Page.captureScreenshot",
       {{"format", "png"}, {"captureBeyondViewport", false}},
       [this, context, epoch](const QJsonObject &result, const QString &error) {
         m_busy = false;
         const QPointer<BrowserInspector> self(this);
         emit busyChanged(false);
         if (!self || m_epoch != epoch)
           return;
         if (!error.isEmpty()) {
           emitFailure(error);
           return;
         }
         const auto data =
             QByteArray::fromBase64(result["data"].toString().toLatin1(),
                                    QByteArray::AbortOnBase64DecodingErrors);
         if (data.isEmpty() || data.size() > 8 * 1024 * 1024) {
           emitFailure("Could not capture the browser viewport.");
           return;
         }
         emit screenshotCaptured(data, context);
       });
}
} // namespace ade
