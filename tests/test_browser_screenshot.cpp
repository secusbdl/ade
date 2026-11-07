#include "browser/BrowserInspector.h"
#include "browser/BrowserScreenshot.h"
#include "browser/BrowserToolBridge.h"
#include "browser/BrowserToolResult.h"
#include "browser/browserwidget.h"
#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
using namespace ade;

// B6: an oversized PNG capture falls back to browser-encoded JPEG (Stagewise
// compressImageToTargetSize: JPEG 85/70/50/30, then half size) instead of
// failing. Ladder tests are pure; the rest drive the installed WebView2 runtime
// against a local fixture with temporary profiles only.
namespace {
QByteArray encode(const QImage &image, const char *format, int quality = -1) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, format, quality);
  return bytes;
}
QImage pattern(int width, int height) {
  QImage image(width, height, QImage::Format_RGB32);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      image.setPixel(x, y, qRgb((x * 7) & 255, (y * 5) & 255, (x + y) & 255));
  return image;
}
QString captureReply(const QByteArray &bytes) {
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{"data", QString::fromLatin1(bytes.toBase64())}})
          .toJson(QJsonDocument::Compact));
}
QString oversized(qsizetype limit = BrowserScreenshotTransportChars) {
  return QString(limit + 1, QLatin1Char('x'));
}
QByteArray formatOf(const QByteArray &bytes) {
  QBuffer buffer;
  buffer.setData(bytes);
  buffer.open(QIODevice::ReadOnly);
  return QImageReader(&buffer).format();
}
// Deterministic per-device-pixel noise: PNG cannot compress it, JPEG can.
const char NoisePage[] = R"HTML(<!doctype html><style>html,body{margin:0;overflow:hidden;background:#fff}canvas{position:fixed;left:0;top:0;width:100vw;height:100vh}</style><canvas id=c></canvas><script>
const d=devicePixelRatio||1;c.width=Math.round(innerWidth*d);c.height=Math.round(innerHeight*d);
const x=c.getContext('2d'),im=x.createImageData(c.width,c.height);let s=2463534242;
for(let i=0;i<im.data.length;i+=4){s^=s<<13;s^=s>>>17;s^=s<<5;im.data[i]=s&255;im.data[i+1]=(s>>>8)&255;im.data[i+2]=(s>>>16)&255;im.data[i+3]=255}
x.putImageData(im,0,0);requestAnimationFrame(()=>requestAnimationFrame(()=>{window.fixtureReady=[c.width,c.height]}));
</script>)HTML";
} // namespace

class BrowserScreenshotTest : public QObject {
  Q_OBJECT
  QTcpServer server;
  QTemporaryDir profiles;
  int serial = 0;
  bool wait(const std::function<bool()> &condition, int timeout = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
      if (condition())
        return true;
      QTest::qWait(10);
    }
    return condition();
  }
  QUrl url(const QString &path) {
    return QUrl(
        QString("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
  }
  // Unparsed reply; error is true on a CDP error or a fixture timeout.
  QString rawCdp(BrowserWidget &browser, const QString &method,
                 const QJsonObject &parameters, bool *error) {
    const QString id = "fixture-" + QString::number(++serial);
    QSignalSpy replies(&browser, &BrowserWidget::devToolsRequestCompleted);
    browser.callDevToolsRequest(
        id, method,
        QString::fromUtf8(
            QJsonDocument(parameters).toJson(QJsonDocument::Compact)));
    QString result = "timeout";
    *error = true;
    wait([&] {
      for (const auto &row : replies)
        if (row[0].toString() == id) {
          result = row[1].toString();
          *error = row[2].toBool();
          return true;
        }
      return false;
    });
    return result;
  }
  QJsonObject cdp(BrowserWidget &browser, const QString &method,
                  const QJsonObject &parameters = {}) {
    bool error = true;
    const auto reply = rawCdp(browser, method, parameters, &error);
    auto result = error ? QJsonObject{{"fixtureError", reply}}
                        : QJsonDocument::fromJson(reply.toUtf8()).object();
    result.insert("fixtureChars", reply.size());
    return result;
  }
  // Shown off-screen like the other browser suites; returns device size.
  QSize loadNoise(BrowserWidget &browser) {
    browser.setAttribute(Qt::WA_DontShowOnScreen);
    browser.resize(1000, 800);
    browser.setProfileDirectory(profiles.path() + "/" +
                                QString::number(++serial));
    browser.navigate(url("/noise"));
    browser.show();
    QSize size;
    wait([&] {
      if (!browser.isReady())
        return false;
      const auto value = cdp(browser, "Runtime.evaluate",
                             {{"expression", "window.fixtureReady||null"},
                              {"returnByValue", true}})["result"]
                             .toObject()["value"]
                             .toArray();
      if (value.size() == 2)
        size = QSize(value[0].toInt(), value[1].toInt());
      return size.isValid();
    });
    return size;
  }
  QJsonArray rowsFor(const QSignalSpy &spy, const QString &id) {
    QJsonArray rows;
    for (const auto &row : spy)
      if (row[0].toString() == id)
        rows.append(QJsonArray{row[1].toString(), row[2].toBool()});
    return rows;
  }

private slots:
  void initTestCase() {
    QVERIFY(profiles.isValid());
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const auto request = socket->readAll();
          if (socket->property("sent").toBool())
            return;
          socket->setProperty("sent", true);
          const QByteArray page =
              request.startsWith("GET /noise")
                  ? QByteArray(NoisePage)
                  : QByteArray("<!doctype html><title>Plain</title><p "
                               "style='font:40px sans-serif'>PLAIN</p>");
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
              QByteArray::number(page.size()) +
              "\r\nConnection: close\r\n\r\n" + page);
          socket->disconnectFromHost();
        });
      }
    });
  }

  // ---- Pure ladder: exact upstream step order and bounds ----
  void fittingPngIsReturnedUnchanged() {
    const auto png = encode(pattern(64, 48), "PNG");
    const auto reply = captureReply(png);
    BrowserScreenshotLadder ladder({{"format", "png"}});
    QCOMPARE(ladder.request().method, QString("Page.captureScreenshot"));
    QCOMPARE(ladder.request().params, QJsonObject({{"format", "png"}}));
    QCOMPARE(ladder.accept(reply), BrowserScreenshotLadder::Step::Done);
    QCOMPARE(ladder.output(), reply);
    QCOMPARE(ladder.bytes(), png);
    QCOMPARE(ladder.mimeType(), QString("image/png"));
    QVERIFY(!ladder.fellBack());
    QCOMPARE(ladder.attempts(), 1);
  }
  void oversizedPngWalksUpstreamJpegQualities() {
    const QJsonObject requested{{"format", "png"},
                                {"captureBeyondViewport", false},
                                {"fromSurface", true}};
    BrowserScreenshotLadder ladder(requested);
    QCOMPARE(ladder.accept(oversized()), BrowserScreenshotLadder::Step::Request);
    for (const int quality : {85, 70, 50, 30}) {
      const auto request = ladder.request();
      QCOMPARE(request.method, QString("Page.captureScreenshot"));
      QCOMPARE(request.params["format"].toString(), QString("jpeg"));
      QCOMPARE(request.params["quality"].toInt(), quality);
      QCOMPARE(request.params["fromSurface"].toBool(), true);
      QCOMPARE(request.params["captureBeyondViewport"].toBool(), false);
      QVERIFY(!request.params.contains("clip"));
      if (quality != 30)
        QCOMPARE(ladder.accept(oversized()),
                 BrowserScreenshotLadder::Step::Request);
    }
    const auto jpeg = encode(pattern(320, 200), "JPEG", 30);
    QCOMPARE(ladder.accept(captureReply(jpeg)),
             BrowserScreenshotLadder::Step::Done);
    QCOMPARE(ladder.bytes(), jpeg);
    QCOMPARE(ladder.mimeType(), QString("image/jpeg"));
    QVERIFY(ladder.note().contains("PNG capture exceeded"));
    QVERIFY(ladder.note().contains("quality 30"));
    // The attachment consumer receives the JPEG and the explanation.
    const auto decoded = decodeBrowserScreenshot(ladder.output());
    QVERIFY2(decoded.error.isEmpty(), qPrintable(decoded.error));
    QCOMPARE(decoded.bytes, jpeg);
    QCOMPARE(decoded.mimeType, QString("image/jpeg"));
    QCOMPARE(decoded.fileName, QString("browser-screenshot.jpeg"));
    QCOMPARE(decoded.note, ladder.note());
    QVERIFY(ladder.output().size() <= BrowserScreenshotTransportChars);
  }
  void exhaustedQualitiesHalveTheViewportUntilTheFloor() {
    BrowserScreenshotLadder ladder(
        {{"format", "png"}, {"captureBeyondViewport", false}});
    for (int i = 0; i < 5; ++i)
      QCOMPARE(ladder.accept(oversized()),
               BrowserScreenshotLadder::Step::Request);
    QCOMPARE(ladder.request().method, QString("Page.getLayoutMetrics"));
    const QJsonObject metrics{
        {"cssVisualViewport",
         QJsonObject{{"pageX", 10}, {"pageY", 20}, {"clientWidth", 800},
                     {"clientHeight", 600}}},
        {"cssContentSize",
         QJsonObject{{"x", 0}, {"y", 0}, {"width", 800}, {"height", 5000}}}};
    QCOMPARE(ladder.accept(QString::fromUtf8(
                 QJsonDocument(metrics).toJson(QJsonDocument::Compact))),
             BrowserScreenshotLadder::Step::Request);
    // 600 CSS px: 0.5 (300), 0.25 (150), 0.125 (75 <= 100 stops).
    for (const double scale : {0.5, 0.25, 0.125}) {
      for (const int quality : {85, 70, 50, 30}) {
        const auto params = ladder.request().params;
        const auto clip = params["clip"].toObject();
        QCOMPARE(clip["x"].toDouble(), 10.0);
        QCOMPARE(clip["y"].toDouble(), 20.0);
        QCOMPARE(clip["width"].toDouble(), 800.0);
        QCOMPARE(clip["height"].toDouble(), 600.0);
        QCOMPARE(clip["scale"].toDouble(), scale);
        QCOMPARE(params["quality"].toInt(), quality);
        const auto step = ladder.accept(oversized());
        if (scale == 0.125 && quality == 30)
          QCOMPARE(step, BrowserScreenshotLadder::Step::Failed);
        else
          QCOMPARE(step, BrowserScreenshotLadder::Step::Request);
      }
    }
    QVERIFY(ladder.error().contains("smallest capture scale"));
    QVERIFY(ladder.output().isEmpty());
    QVERIFY(ladder.bytes().isEmpty());
    QCOMPARE(ladder.attempts(), 1 + 4 + 1 + 12);
    QCOMPARE(ladder.accept(captureReply(encode(pattern(8, 8), "PNG"))),
             BrowserScreenshotLadder::Step::Failed);
  }
  void requestedClipFullPageAndJpegQualityAreKept() {
    BrowserScreenshotLadder clipped(
        {{"format", "png"},
         {"clip", QJsonObject{{"x", 0}, {"y", 0}, {"width", 320},
                              {"height", 240}, {"scale", 2}}}});
    for (int i = 0; i < 5; ++i)
      clipped.accept(oversized());
    QCOMPARE(clipped.request().method, QString("Page.captureScreenshot"));
    QCOMPARE(clipped.request().params["clip"].toObject()["scale"].toDouble(),
             1.0);
    const auto jpeg = encode(pattern(320, 240), "JPEG", 85);
    QCOMPARE(clipped.accept(captureReply(jpeg)),
             BrowserScreenshotLadder::Step::Done);
    QVERIFY(clipped.note().contains("capture scale 1"));

    BrowserScreenshotLadder full(
        {{"format", "png"}, {"captureBeyondViewport", true}});
    for (int i = 0; i < 5; ++i)
      full.accept(oversized());
    QCOMPARE(full.request().method, QString("Page.getLayoutMetrics"));
    full.accept(R"({"cssVisualViewport":{"pageX":0,"pageY":40,"clientWidth":800,"clientHeight":600},"cssContentSize":{"x":0,"y":0,"width":800,"height":5000}})");
    // With captureBeyondViewport and no clip WebView2 captures the whole
    // document, so the fallback halves the content size, not the viewport.
    const auto clip = full.request().params["clip"].toObject();
    QCOMPARE(clip["y"].toDouble(), 0.0);
    QCOMPARE(clip["width"].toDouble(), 800.0);
    QCOMPARE(clip["height"].toDouble(), 5000.0);
    QCOMPARE(clip["scale"].toDouble(), 0.5);
    QCOMPARE(full.request().params["captureBeyondViewport"].toBool(), true);

    BrowserScreenshotLadder low({{"format", "jpeg"}, {"quality", 60}});
    QCOMPARE(low.accept(oversized()), BrowserScreenshotLadder::Step::Request);
    QCOMPARE(low.request().params["quality"].toInt(), 50);
    low.accept(oversized());
    QCOMPARE(low.request().params["quality"].toInt(), 30);
  }
  void transportAndDecodedImageBoundsHold() {
    // The envelope reserve keeps a fallback output within the bridge bound.
    BrowserScreenshotLadder edge({{"format", "png"}});
    QCOMPARE(edge.accept(oversized(BrowserScreenshotTransportChars -
                                   BrowserScreenshotEnvelopeChars)),
             BrowserScreenshotLadder::Step::Request);
    // A caller-lowered cutoff is honored and clamped.
    BrowserScreenshotLadder lowered({{"format", "png"}}, 64 * 1024);
    QCOMPARE(lowered.accept(oversized(64 * 1024)),
             BrowserScreenshotLadder::Step::Request);
    // Fitting replies must be complete images; data is never echoed. The
    // ladder and the attachment decoder give the same actionable message.
    const QString invalid = QString::fromLatin1(BrowserScreenshotInvalidMessage);
    QVERIFY(invalid.contains("API.createAttachment"));
    for (const auto &reply :
         {QString("not json"), QString(R"({"data":"PRIVATE_@@@"})"),
          captureReply("PRIVATE_NOT_IMAGE")}) {
      BrowserScreenshotLadder ladder({{"format", "png"}});
      QCOMPARE(ladder.accept(reply), BrowserScreenshotLadder::Step::Failed);
      QCOMPARE(ladder.error(), invalid);
      QVERIFY(ladder.bytes().isEmpty());
      QCOMPARE(decodeBrowserScreenshot(reply).error, invalid);
    }
    QCOMPARE(checkBrowserScreenshotImage(encode(pattern(16385, 1), "PNG")),
             BrowserScreenshotImageCheck::TooLarge);
    QCOMPARE(checkBrowserScreenshotImage("PRIVATE_NOT_IMAGE"),
             BrowserScreenshotImageCheck::Invalid);
    QVERIFY(!validateBrowserScreenshotImage(encode(pattern(16385, 1), "PNG")));
    QVERIFY(validateBrowserScreenshotImage(encode(pattern(16384, 1), "PNG")));
    QVERIFY(!validateBrowserScreenshotImage(encode(pattern(8, 8), "BMP")));
    QCOMPARE(browserScreenshotExtension(encode(pattern(8, 8), "JPEG")),
             QString("jpeg"));
    QCOMPARE(browserScreenshotExtension(encode(pattern(8, 8), "PNG")),
             QString("png"));
  }
  // A fitting reply that decodes but exceeds the side bound steps down the
  // ladder (halving) instead of failing; quality alone cannot shrink it.
  void overBoundImageHalvesInsteadOfFailing() {
    const auto wide = captureReply(encode(pattern(16385, 1), "PNG"));
    QVERIFY(wide.size() < BrowserScreenshotTransportChars / 2);
    BrowserScreenshotLadder viewport({{"format", "png"}});
    QCOMPARE(viewport.accept(wide), BrowserScreenshotLadder::Step::Request);
    QCOMPARE(viewport.request().method, QString("Page.getLayoutMetrics"));

    BrowserScreenshotLadder clipped(
        {{"format", "png"},
         {"clip", QJsonObject{{"x", 0}, {"y", 0}, {"width", 9000},
                              {"height", 400}, {"scale", 2}}}});
    QCOMPARE(clipped.accept(wide), BrowserScreenshotLadder::Step::Request);
    auto params = clipped.request().params;
    QCOMPARE(params["clip"].toObject()["scale"].toDouble(), 1.0);
    QCOMPARE(params["clip"].toObject()["width"].toDouble(), 9000.0);
    QCOMPARE(params["format"].toString(), QString("jpeg"));
    QCOMPARE(params["quality"].toInt(), 85);
    // Still over the bound at half scale: halve again, never fail early.
    QCOMPARE(clipped.accept(captureReply(encode(pattern(16385, 1), "JPEG"))),
             BrowserScreenshotLadder::Step::Request);
    params = clipped.request().params;
    QCOMPARE(params["clip"].toObject()["scale"].toDouble(), 0.5);
    QCOMPARE(params["quality"].toInt(), 85);
    const auto jpeg = encode(pattern(4500, 200), "JPEG", 85);
    QCOMPARE(clipped.accept(captureReply(jpeg)),
             BrowserScreenshotLadder::Step::Done);
    QCOMPARE(clipped.bytes(), jpeg);
    QVERIFY(clipped.note().contains("16384 px image bound"));
    QVERIFY(!clipped.note().contains("transport"));
    QVERIFY(clipped.note().contains("capture scale 0.5"));
    QCOMPARE(decodeBrowserScreenshot(clipped.output()).note, clipped.note());

    // The 100 CSS px floor still ends an over-bound ladder explicitly.
    BrowserScreenshotLadder thin(
        {{"format", "png"},
         {"clip", QJsonObject{{"x", 0}, {"y", 0}, {"width", 20000},
                              {"height", 150}}}});
    QCOMPARE(thin.accept(wide), BrowserScreenshotLadder::Step::Request);
    QCOMPARE(thin.accept(wide), BrowserScreenshotLadder::Step::Failed);
    QVERIFY(thin.error().contains("smallest capture scale"));
  }

  // ---- Real WebView2: user "Attach a screenshot" (BrowserInspector) ----
  void inspectorOversizedPngAttachesJpeg() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    const auto device = loadNoise(browser);
    QVERIFY(device.isValid());
    // Precondition: the lossless capture really exceeds the 2 MiB transport.
    const auto png = cdp(browser, "Page.captureScreenshot",
                         {{"format", "png"}, {"captureBeyondViewport", false}});
    qInfo() << "device" << device << "PNG reply chars"
            << png["fixtureChars"].toInteger();
    QVERIFY(png["fixtureChars"].toInteger() > BrowserScreenshotTransportChars);
    QSignalSpy screenshots(&inspector, &BrowserInspector::screenshotCaptured),
        errors(&inspector, &BrowserInspector::failed);
    inspector.setProperty("captureAgentId", "noise-chat");
    inspector.captureScreenshot();
    QVERIFY(wait([&] { return !screenshots.isEmpty() || !errors.isEmpty(); }));
    QVERIFY2(errors.isEmpty(),
             qPrintable(errors.isEmpty() ? QString()
                                         : errors.first()[0].toString()));
    const auto bytes = screenshots.first()[0].toByteArray();
    QCOMPARE(screenshots.first()[1].toString(), QString("noise-chat"));
    QCOMPARE(formatOf(bytes), QByteArray("jpeg"));
    QCOMPARE(browserScreenshotExtension(bytes), QString("jpeg"));
    const auto image = QImage::fromData(bytes);
    QVERIFY(!image.isNull());
    qInfo() << "JPEG bytes" << bytes.size() << "image" << image.size();
    QVERIFY(qint64(bytes.toBase64().size()) <= BrowserScreenshotTransportChars);
    QVERIFY(image.width() * 2 >= device.width());
    QVERIFY(!inspector.isBusy());
  }
  void inspectorHalvesScaleUnderLoweredCutoff() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    const auto device = loadNoise(browser);
    QVERIFY(device.isValid());
    inspector.setScreenshotTransportChars(64 * 1024);
    QSignalSpy screenshots(&inspector, &BrowserInspector::screenshotCaptured),
        errors(&inspector, &BrowserInspector::failed);
    inspector.captureScreenshot();
    QVERIFY(wait([&] { return !screenshots.isEmpty() || !errors.isEmpty(); },
                 30000));
    QVERIFY2(errors.isEmpty(),
             qPrintable(errors.isEmpty() ? QString()
                                         : errors.first()[0].toString()));
    const auto bytes = screenshots.first()[0].toByteArray();
    const auto image = QImage::fromData(bytes);
    qInfo() << "halved JPEG bytes" << bytes.size() << "image" << image.size()
            << "device" << device;
    QCOMPARE(formatOf(bytes), QByteArray("jpeg"));
    QVERIFY(bytes.toBase64().size() <= 64 * 1024);
    // Real Page.getLayoutMetrics + clip.scale produced a smaller capture.
    QVERIFY(image.width() > 0 && image.width() * 3 <= device.width() * 2);
  }
  // captureBeyondViewport without a clip captures the whole document in WebView2,
  // so the metrics fallback must keep that area, not shrink to the viewport.
  void fullPageFlagWithoutClipKeepsCapturedArea() {
    BrowserWidget browser;
    QVERIFY(loadNoise(browser).isValid());
    // A document far taller than the viewport, so the two areas differ.
    cdp(browser, "Runtime.evaluate",
        {{"expression", "const e=document.createElement('div');"
                        "e.style.height='6000px';document.body.appendChild(e);"
                        "document.documentElement.scrollHeight"},
         {"returnByValue", true}});
    const auto metrics = cdp(browser, "Page.getLayoutMetrics");
    const auto viewport = metrics["cssVisualViewport"].toObject();
    const auto content = metrics["cssContentSize"].toObject();
    const double viewportAspect = viewport["clientWidth"].toDouble() /
                                  viewport["clientHeight"].toDouble();
    const double contentAspect =
        content["width"].toDouble() / content["height"].toDouble();
    qInfo() << "viewport" << viewport << "content" << content;
    QVERIFY(viewportAspect > 0);
    QVERIFY(contentAspect > 0 && contentAspect * 3 < viewportAspect);
    // Precondition (real WebView2): with captureBeyondViewport and no clip the
    // capture covers the whole document, not just the viewport.
    bool error = true;
    const auto plain =
        rawCdp(browser, "Page.captureScreenshot",
               {{"format", "jpeg"}, {"quality", 30},
                {"captureBeyondViewport", true}},
               &error);
    QVERIFY2(!error, qPrintable(plain.left(300)));
    const auto plainImage = QImage::fromData(QByteArray::fromBase64(
        QJsonDocument::fromJson(plain.toUtf8()).object()["data"].toString().toLatin1()));
    QVERIFY(!plainImage.isNull());
    QVERIFY2(qAbs(double(plainImage.width()) / plainImage.height() -
                  contentAspect) < contentAspect * 0.05,
             qPrintable(QString("plain %1x%2").arg(plainImage.width()).arg(plainImage.height())));
    // Drive the real ladder with a lowered cutoff through the metrics step.
    BrowserScreenshotLadder ladder(
        {{"format", "png"}, {"captureBeyondViewport", true}}, 64 * 1024);
    bool sawMetrics = false;
    while (ladder.step() == BrowserScreenshotLadder::Step::Request) {
      const auto request = ladder.request();
      sawMetrics |= request.method == "Page.getLayoutMetrics";
      const auto reply = rawCdp(browser, request.method, request.params, &error);
      QVERIFY2(!error, qPrintable(reply.left(300)));
      ladder.accept(reply);
    }
    QVERIFY2(ladder.step() == BrowserScreenshotLadder::Step::Done,
             qPrintable(ladder.error()));
    QVERIFY(sawMetrics);
    const auto image = QImage::fromData(ladder.bytes());
    qInfo() << "full-page flag fallback" << image.size() << ladder.note();
    QVERIFY(!image.isNull());
    const double aspect = double(image.width()) / image.height();
    // The fallback keeps the area the requested PNG covered (the document).
    QVERIFY2(qAbs(aspect - contentAspect) < contentAspect * 0.05,
             qPrintable(QString::number(aspect)));
  }
  void navigationRetiresAnInFlightFallbackStep() {
    BrowserWidget browser;
    BrowserInspector inspector(&browser);
    QVERIFY(loadNoise(browser).isValid());
    QSignalSpy screenshots(&inspector, &BrowserInspector::screenshotCaptured),
        errors(&inspector, &BrowserInspector::failed);
    QString first;
    int later = 0;
    // Connected after the inspector: runs once the inspector has handled the
    // oversized PNG reply (and, with B6, dispatched the JPEG step).
    connect(&browser, &BrowserWidget::devToolsRequestCompleted, this,
            [&](const QString &id, const QString &output, bool) {
              if (!id.startsWith("inspector-"))
                return;
              if (first.isEmpty() &&
                  output.size() > BrowserScreenshotTransportChars) {
                first = id;
                emit browser.loadingChanged(true);
              } else if (!first.isEmpty() && id != first)
                ++later;
            });
    inspector.captureScreenshot();
    QVERIFY(wait([&] { return !first.isEmpty(); }));
    // The JPEG step was really in flight; its reply arrives and is dropped.
    QVERIFY(wait([&] { return later > 0; }));
    QTest::qWait(300);
    QVERIFY2(errors.isEmpty(),
             qPrintable(errors.isEmpty() ? QString()
                                         : errors.first()[0].toString()));
    QVERIFY(screenshots.isEmpty());
    QVERIFY(!inspector.isBusy());
  }

  // ---- Real WebView2: direct sendCDP screenshot through the bridge ----
  void directScreenshotFallsBackRawSendCdpDoesNot() {
    BrowserWidget browser;
    BrowserToolBridge bridge;
    const auto tab = bridge.registerTab(&browser, "noise");
    QVERIFY(loadNoise(browser).isValid());
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    const QJsonObject arguments{{"tabId", tab},
                                {"method", "Page.captureScreenshot"},
                                {"params", QJsonObject{{"format", "png"}}}};
    bridge.executeScreenshotForAgent("direct", "agent", arguments);
    bridge.executeForAgent("raw", "agent", "sendCDP", arguments);
    QVERIFY(wait([&] {
      return !rowsFor(completed, "direct").isEmpty() &&
             !rowsFor(completed, "raw").isEmpty();
    }));
    const auto direct = rowsFor(completed, "direct");
    QCOMPARE(direct.size(), 1);
    const auto output = direct[0].toArray()[0].toString();
    QVERIFY2(!direct[0].toArray()[1].toBool(), qPrintable(output.left(300)));
    QVERIFY(output.size() <= BrowserScreenshotTransportChars);
    const auto image = decodeBrowserScreenshot(output);
    QVERIFY2(image.error.isEmpty(), qPrintable(image.error));
    QCOMPARE(image.mimeType, QString("image/jpeg"));
    QVERIFY(image.note.contains("PNG capture exceeded"));
    qInfo() << "direct fallback:" << image.note << image.bytes.size();
    // Sandbox/raw API.sendCDP keeps the requested format and the explicit bound.
    const auto raw = rowsFor(completed, "raw");
    QVERIFY(raw[0].toArray()[1].toBool());
    QVERIFY(raw[0].toArray()[0].toString().contains("exceeds 2 MiB"));
  }
  void canceledLadderIsScopedAndIdCanBeReused() {
    BrowserWidget browser;
    BrowserToolBridge bridge;
    const auto tab = bridge.registerTab(&browser, "noise");
    QVERIFY(loadNoise(browser).isValid());
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    QString correlation;
    int stepReplies = 0;
    // Connected after registerTab: the bridge has already dispatched the next
    // ladder step under the same opaque correlation when this runs.
    connect(&browser, &BrowserWidget::devToolsRequestCompleted, this,
            [&](const QString &id, const QString &output, bool) {
              if (correlation.isEmpty() &&
                  output.size() > BrowserScreenshotTransportChars) {
                correlation = id;
                bridge.cancel("shot");
              } else if (!correlation.isEmpty() && id == correlation)
                ++stepReplies;
            });
    bridge.executeScreenshotForAgent(
        "shot", "agent",
        {{"tabId", tab},
         {"method", "Page.captureScreenshot"},
         {"params", QJsonObject{{"format", "png"}}}});
    QVERIFY(wait([&] { return !correlation.isEmpty(); }));
    QVERIFY(wait([&] { return stepReplies > 0; }));
    QTest::qWait(300);
    auto rows = rowsFor(completed, "shot");
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows[0].toArray()[1].toBool());
    QVERIFY(rows[0].toArray()[0].toString().contains("canceled"));
    // Reuse the ID: only the new request's own (small, lossless) result.
    bridge.executeScreenshotForAgent(
        "shot", "agent",
        {{"tabId", tab},
         {"method", "Page.captureScreenshot"},
         {"params",
          QJsonObject{{"format", "png"},
                      {"clip", QJsonObject{{"x", 0}, {"y", 0}, {"width", 64},
                                           {"height", 48}, {"scale", 1}}}}}});
    QVERIFY(wait([&] { return rowsFor(completed, "shot").size() == 2; }));
    QTest::qWait(200);
    rows = rowsFor(completed, "shot");
    QCOMPARE(rows.size(), 2);
    const auto image = decodeBrowserScreenshot(rows[1].toArray()[0].toString());
    QVERIFY2(image.error.isEmpty(), qPrintable(image.error));
    QCOMPARE(image.mimeType, QString("image/png"));
    QVERIFY(image.note.isEmpty());
  }
  // Each ladder step is a fresh capture: a navigation between steps must fail
  // explicitly instead of returning a fallback image of a different document.
  void navigationBetweenBridgeStepsFailsExplicitly() {
    BrowserWidget browser;
    BrowserToolBridge bridge;
    const auto tab = bridge.registerTab(&browser, "noise");
    QVERIFY(loadNoise(browser).isValid());
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    QString correlation;
    // Connected after registerTab: the bridge has already dispatched the JPEG
    // step under the same correlation when the simulated navigation starts.
    connect(&browser, &BrowserWidget::devToolsRequestCompleted, this,
            [&](const QString &id, const QString &output, bool) {
              if (correlation.isEmpty() &&
                  output.size() > BrowserScreenshotTransportChars) {
                correlation = id;
                emit browser.loadingChanged(true);
              }
            });
    bridge.executeScreenshotForAgent(
        "moved", "agent",
        {{"tabId", tab},
         {"method", "Page.captureScreenshot"},
         {"params", QJsonObject{{"format", "png"}}}});
    QVERIFY(wait([&] { return !rowsFor(completed, "moved").isEmpty(); }));
    QVERIFY(!correlation.isEmpty());
    const auto rows = rowsFor(completed, "moved");
    QCOMPARE(rows.size(), 1);
    const auto output = rows[0].toArray()[0].toString();
    QVERIFY2(rows[0].toArray()[1].toBool(), qPrintable(output.left(200)));
    QVERIFY2(output.contains("navigated during the screenshot fallback"),
             qPrintable(output.left(300)));
    // Without a navigation the same tab still falls back successfully.
    bridge.executeScreenshotForAgent(
        "stayed", "agent",
        {{"tabId", tab},
         {"method", "Page.captureScreenshot"},
         {"params", QJsonObject{{"format", "png"}}}});
    QVERIFY(wait([&] { return !rowsFor(completed, "stayed").isEmpty(); }));
    const auto stayed = rowsFor(completed, "stayed")[0].toArray();
    QVERIFY2(!stayed[1].toBool(), qPrintable(stayed[0].toString().left(300)));
    QCOMPARE(decodeBrowserScreenshot(stayed[0].toString()).mimeType,
             QString("image/jpeg"));
  }
  // A never-shown hidden tab is initialized invisibly for the request. Whether
  // WebView2 paints it is runtime behavior; the contract tested here is that
  // the request settles within the bridge deadline with either a valid image,
  // the explicit hidden-tab error, or the generic timeout while the hidden
  // controller is still starting, and the tab stays hidden.
  void neverShownHiddenTabScreenshotIsBoundedAndExplicit() {
    QTemporaryDir profile;
    BrowserWidget browser;
    browser.resize(600, 400);
    QVERIFY(browser.setProfileDirectory(profile.path()));
    browser.navigate(url("/plain"));
    BrowserToolBridge bridge;
    bridge.setRequestTimeoutMs(15000);
    bridge.registerTab(&browser, "hidden");
    QVERIFY(!browser.isVisible());
    QSignalSpy completed(&bridge, &BrowserToolBridge::completed);
    QElapsedTimer elapsed;
    elapsed.start();
    bridge.executeScreenshotForAgent(
        "hidden-shot", "agent",
        {{"tabId", "hidden"},
         {"method", "Page.captureScreenshot"},
         {"params",
          QJsonObject{{"format", "png"},
                      {"clip", QJsonObject{{"x", 0}, {"y", 0}, {"width", 200},
                                           {"height", 100}, {"scale", 1}}}}}});
    QVERIFY(wait([&] { return !rowsFor(completed, "hidden-shot").isEmpty(); },
                 30000));
    const auto row = rowsFor(completed, "hidden-shot")[0].toArray();
    const auto output = row[0].toString();
    qInfo() << "hidden tab screenshot settled after" << elapsed.elapsed()
            << "ms, error" << row[1].toBool() << output.left(200);
    QVERIFY(elapsed.elapsed() < 25000);
    QVERIFY(!browser.isVisible());
    if (row[1].toBool()) {
      QVERIFY2(output.contains("not visible") ||
                   output.contains("Browser request timed out."),
               qPrintable(output.left(300)));
    } else {
      const auto image = decodeBrowserScreenshot(output);
      QVERIFY2(image.error.isEmpty(), qPrintable(image.error));
      QVERIFY(!QImage::fromData(image.bytes).isNull());
    }
  }
};
QTEST_MAIN(BrowserScreenshotTest)
#include "test_browser_screenshot.moc"
