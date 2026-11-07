#include "attachments/AttachmentStore.h"
#include "browser/BrowserToolBridge.h"
#include "browser/BrowserToolResult.h"
#include "browser/browserwidget.h"
#include "core/AgentEngine.h"
#include "runtime/AgentToolHost.h"
#include "ui/SessionStore.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
class Server : public QObject {
public:
  QTcpServer server;
  QList<QJsonObject> requests;
  QList<QPointer<QTcpSocket>> sockets;
  Server() {
    server.listen(QHostAddress::LocalHost);
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
          auto bytes =
              socket->property("bytes").toByteArray() + socket->readAll();
          socket->setProperty("bytes", bytes);
          if (socket->property("received").toBool())
            return;
          const auto end = bytes.indexOf("\r\n\r\n");
          if (end < 0)
            return;
          int length = 0;
          for (const auto &line : bytes.left(end).split('\n'))
            if (line.toLower().startsWith("content-length:"))
              length = line.mid(15).trimmed().toInt();
          if (bytes.size() < end + 4 + length)
            return;
          socket->setProperty("received", true);
          requests.append(
              QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
          sockets.append(socket);
        });
      }
    });
  }
  void respond(int index, const QByteArray &body) {
    auto socket = sockets.value(index);
    if (!socket)
      return;
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                  "text/event-stream\r\nContent-Length: " +
                  QByteArray::number(body.size()) +
                  "\r\nConnection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
  }
  ProviderConfig provider(const QString &protocol) const {
    return {"fixture",
            "Browser context fixture",
            QString("http://127.0.0.1:%1/v1").arg(server.serverPort()),
            "synthetic-local-only",
            protocol == "anthropic" ? "claude-sonnet-5.5"
            : protocol == "google"  ? "gemini-3.1-pro-preview"
                                    : "gpt-5.5",
            protocol};
  }
};
QString instructions(const QJsonObject &body, const QString &protocol) {
  if (protocol == "responses")
    return body["instructions"].toString();
  if (protocol == "anthropic")
    return body["system"].toString();
  if (protocol == "google")
    return body["systemInstruction"]
        .toObject()["parts"]
        .toArray()
        .first()
        .toObject()["text"]
        .toString();
  return body["messages"].toArray().first().toObject()["content"].toString();
}
QByteArray event(const QJsonObject &object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\n\n";
}
QByteArray reply(const QString &protocol, bool tool,
                 const QString &name = "getBrowserTabs",
                 const QJsonObject &args = {}) {
  const QString id = "browser-discovery";
  const auto arguments =
      QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
  if (protocol == "openai") {
    QJsonObject delta{{"content", "Done"}};
    if (tool)
      delta = {{"tool_calls",
                QJsonArray{QJsonObject{
                    {"index", 0},
                    {"id", id},
                    {"type", "function"},
                    {"function",
                     QJsonObject{{"name", name}, {"arguments", arguments}}}}}}};
    return event({{"choices",
                   QJsonArray{QJsonObject{
                       {"delta", delta},
                       {"finish_reason", tool ? "tool_calls" : "stop"}}}}}) +
           "data: [DONE]\n\n";
  }
  if (protocol == "anthropic") {
    const QJsonObject block =
        tool ? QJsonObject{{"type", "tool_use"},
                           {"id", id},
                           {"name", name},
                           {"input", args}}
             : QJsonObject{{"type", "text"}, {"text", "Done"}};
    return event({{"type", "content_block_start"},
                  {"index", 0},
                  {"content_block", block}}) +
           event({{"type", "content_block_stop"}, {"index", 0}}) +
           event({{"type", "message_delta"},
                  {"delta", QJsonObject{{"stop_reason",
                                         tool ? "tool_use" : "end_turn"}}}}) +
           event({{"type", "message_stop"}});
  }
  if (protocol == "google") {
    const QJsonObject part =
        tool ? QJsonObject{{"functionCall", QJsonObject{{"id", id},
                                                        {"name", name},
                                                        {"args", args}}}}
             : QJsonObject{{"text", "Done"}};
    return event({{"candidates",
                   QJsonArray{QJsonObject{
                       {"content", QJsonObject{{"role", "model"},
                                               {"parts", QJsonArray{part}}}},
                       {"finishReason", "STOP"}}}}});
  }
  const QJsonObject item =
      tool ? QJsonObject{{"id", "item1"},
                         {"type", "function_call"},
                         {"call_id", id},
                         {"name", name},
                         {"arguments", arguments}}
           : QJsonObject{
                 {"id", "item1"},
                 {"type", "message"},
                 {"role", "assistant"},
                 {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                    {"text", "Done"}}}}};
  return event({{"type", "response.output_item.done"},
                {"output_index", 0},
                {"item", item}}) +
         event({{"type", "response.completed"},
                {"response", QJsonObject{{"status", "completed"},
                                         {"output", QJsonArray{item}}}}});
}
void protocols() {
  QTest::addColumn<QString>("protocol");
  for (const auto &p : {"openai", "responses", "anthropic", "google"})
    QTest::newRow(p) << QString(p);
}
} // namespace

namespace {
class PageServer : public QTcpServer {
public:
  PageServer() {
    listen(QHostAddress::LocalHost);
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          socket->readAll();
          if (socket->property("sent").toBool())
            return;
          socket->setProperty("sent", true);
          const QByteArray page =
              R"HTML(<!doctype html><style>html,body{margin:0;background:white}</style><canvas id=c width=320 height=240></canvas><div id=large></div><script>
      let seed=123456789;const ctx=c.getContext('2d'),im=ctx.createImageData(320,240);for(let i=0;i<im.data.length;i+=4){seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;im.data[i]=seed&255;im.data[i+1]=(seed>>>8)&255;im.data[i+2]=(seed>>>16)&255;im.data[i+3]=255}ctx.putImageData(im,0,0);large.textContent='L'.repeat(180000);window.fixtureReady=true;
    </script>)HTML";
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
              QByteArray::number(page.size()) +
              "\r\nConnection: close\r\n\r\n" + page);
          socket->disconnectFromHost();
        });
      }
    });
  }
  QUrl url() const {
    return QUrl(QString("http://127.0.0.1:%1/").arg(serverPort()));
  }
};
bool load(BrowserWidget &browser, PageServer &server, const QString &profile) {
  browser.setAttribute(Qt::WA_DontShowOnScreen);
  browser.resize(500, 350);
  browser.setProfileDirectory(profile);
  QSignalSpy loading(&browser, &BrowserWidget::loadingChanged);
  browser.navigate(server.url());
  browser.show();
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 15000) {
    for (const auto &row : loading)
      if (!row[0].toBool() && browser.isReady())
        return true;
    QTest::qWait(10);
  }
  return false;
}
QJsonObject toolRow(const AgentEngine &engine) {
  for (const auto &value : engine.messages()) {
    auto row = value.toObject();
    if (row.value("role") == "tool")
      return row;
  }
  return {};
}
bool hasImage(const QJsonValue &v) {
  if (v.isObject()) {
    const auto o = v.toObject();
    if (o.contains("image_url") || o.value("type") == "input_image" ||
        o.value("type") == "image" || o.contains("inlineData"))
      return true;
    for (auto it = o.begin(); it != o.end(); ++it)
      if (hasImage(it.value()))
        return true;
  } else if (v.isArray())
    for (const auto &value : v.toArray())
      if (hasImage(value))
        return true;
  return false;
}
QString quote(const QString &value) {
  const auto b =
      QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(b.mid(1, b.size() - 2));
}
} // namespace
class BrowserOutputTests : public QObject {
  Q_OBJECT
private slots:
  void screenshotsBecomeImageAttachments_data() { protocols(); }
  void screenshotsBecomeImageAttachments() {
    QFETCH(QString, protocol);
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    AgentEngine engine;
    engine.setProvider(model.provider(protocol));
    engine.setAlwaysAllow(true);
    AgentToolHost host(&engine, &bridge, "image-owner", data.path());
    QSignalSpy raw(&bridge, &BrowserToolBridge::completed);
    QSignalSpy failures(&engine, &AgentEngine::failed);
    engine.submit("Inspect fixture screenshot");
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 1, 5000);
    const QJsonObject clip{
        {"x", 0}, {"y", 0}, {"width", 320}, {"height", 240}, {"scale", 1}};
    const QJsonObject parameters{{"format", "png"}, {"clip", clip}};
    const QJsonObject arguments{{"tabId", tab},
                                {"method", "Page.captureScreenshot"},
                                {"params", parameters}};
    model.respond(0, reply(protocol, true, "sendCDP", arguments));
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 2, 15000);
    model.respond(1, reply(protocol, false));
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(failures.isEmpty());
    const auto row = toolRow(engine);
    const auto content = row.value("content").toString();
    const auto refs =
        row.value("metadata").toObject().value("attachments").toArray();
    qInfo() << "screenshot stored text characters" << content.size()
            << "attachment count" << refs.size();
    QCOMPARE(refs.size(), 1);
    QVERIFY(content.size() < 1024);
    QVERIFY(!content.contains("iVBOR"));
    QVERIFY(hasImage(model.requests[1]));
    QString error;
    const auto ref = AttachmentRef::fromJson(refs[0].toObject(), &error);
    QVERIFY(error.isEmpty());
    const auto bytes =
        AttachmentStore(data.path()).read("image-owner", ref, {}, &error);
    QVERIFY(error.isEmpty());
    QByteArray actualCapture;
    for (const auto &call : raw) {
      const auto object =
          QJsonDocument::fromJson(call[1].toString().toUtf8()).object();
      if (object.value("data").isString())
        actualCapture =
            QByteArray::fromBase64(object.value("data").toString().toLatin1());
    }
    QVERIFY(!actualCapture.isEmpty());
    QCOMPARE(bytes, actualCapture);
    const auto image = QImage::fromData(bytes);
    QVERIFY(!image.isNull());
    QVERIFY(image.width() >= 320 && image.height() >= 240);
    QVERIFY(bytes.size() > 65536);
    Conversation saved;
    saved.id = "image-owner";
    saved.title = "Fixture";
    saved.messages = engine.messages();
    QVERIFY(SessionStore(data.path()).save({saved}, &error));
    QFile file(data.filePath("sessions.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto disk = file.readAll();
    QVERIFY(disk.size() < 65536);
    QVERIFY(!disk.contains(bytes.toBase64().left(100)));
    const auto reopened = SessionStore(data.path()).load(&error);
    QVERIFY(error.isEmpty());
    QCOMPARE(reopened.first().messages, engine.messages());
  }
  // B6: a direct PNG capture above the 2 MiB transport becomes a JPEG
  // attachment (Stagewise compresses captures to JPEG) instead of an error.
  void oversizedDirectScreenshotBecomesJpegAttachment() {
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    browser.resize(1000, 800);
    // Wait until the page itself reports the resized viewport; a noise canvas
    // painted at the old 500 px width would not exceed the transport.
    QSignalSpy widths(&browser, &BrowserWidget::devToolsRequestCompleted);
    int widthPolls = 0;
    double innerWidth = 0;
    QElapsedTimer resized;
    resized.start();
    while (innerWidth < 900 && resized.elapsed() < 10000) {
      const QString id = "fixture-width-" + QString::number(++widthPolls);
      browser.callDevToolsRequest(
          id, "Runtime.evaluate",
          R"({"expression":"innerWidth","returnByValue":true})");
      QElapsedTimer reply;
      reply.start();
      bool answered = false;
      while (!answered && reply.elapsed() < 2000) {
        QTest::qWait(10);
        for (const auto &row : widths)
          if (row[0].toString() == id) {
            answered = true;
            innerWidth = QJsonDocument::fromJson(row[1].toString().toUtf8())
                             .object()["result"]
                             .toObject()["value"]
                             .toDouble();
          }
      }
      if (innerWidth < 900)
        QTest::qWait(50);
    }
    qInfo() << "innerWidth" << innerWidth << "after" << widthPolls << "polls";
    QVERIFY(innerWidth >= 900);
    // Per-device-pixel noise over the whole viewport: PNG cannot compress it.
    const QString noise =
        "new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))"
        ".then(()=>{document.body.innerHTML='';const d=devicePixelRatio||1,"
        "c=document.createElement('canvas');c.width=Math.round(innerWidth*d);"
        "c.height=Math.round(innerHeight*d);c.style.cssText='position:fixed;"
        "left:0;top:0;width:100vw;height:100vh';document.body.style.overflow="
        "'hidden';document.body.appendChild(c);const x=c.getContext('2d'),"
        "im=x.createImageData(c.width,c.height);let s=2463534242;for(let i=0;"
        "i<im.data.length;i+=4){s^=s<<13;s^=s>>>17;s^=s<<5;im.data[i]=s&255;"
        "im.data[i+1]=(s>>>8)&255;im.data[i+2]=(s>>>16)&255;im.data[i+3]=255}"
        "x.putImageData(im,0,0);return new Promise(r=>requestAnimationFrame("
        "()=>requestAnimationFrame(()=>r(c.width*c.height))))})";
    QSignalSpy evaluated(&browser, &BrowserWidget::devToolsRequestCompleted);
    browser.callDevToolsRequest(
        "fixture-noise", "Runtime.evaluate",
        QString::fromUtf8(QJsonDocument(QJsonObject{{"expression", noise},
                                                    {"awaitPromise", true},
                                                    {"returnByValue", true}})
                              .toJson(QJsonDocument::Compact)));
    QJsonObject painted;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
          for (const auto &row : evaluated)
            if (row[0].toString() == "fixture-noise") {
              painted = QJsonDocument::fromJson(row[1].toString().toUtf8())
                            .object();
              return true;
            }
          return false;
        }(),
        10000);
    const auto pixels =
        painted["result"].toObject()["value"].toInteger();
    qInfo() << "noise device pixels" << pixels;
    // 3 bytes per noise pixel, base64 4/3: comfortably above 2 Mi characters.
    QVERIFY(pixels >= 700000);
    AgentEngine engine;
    engine.setProvider(model.provider("openai"));
    engine.setAlwaysAllow(true);
    AgentToolHost host(&engine, &bridge, "jpeg-owner", data.path());
    QSignalSpy failures(&engine, &AgentEngine::failed);
    engine.submit("Capture the oversized fixture");
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 1, 5000);
    model.respond(0, reply("openai", true, "sendCDP",
                           {{"tabId", tab},
                            {"method", "Page.captureScreenshot"},
                            {"params", QJsonObject{{"format", "png"}}}}));
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 2, 20000);
    model.respond(1, reply("openai", false));
    QTRY_VERIFY(!engine.isBusy());
    QVERIFY(failures.isEmpty());
    const auto row = toolRow(engine);
    const auto content = row.value("content").toString();
    const auto refs =
        row.value("metadata").toObject().value("attachments").toArray();
    qInfo() << "fallback tool text" << content;
    QCOMPARE(refs.size(), 1);
    QVERIFY(content.contains("JPEG at quality"));
    QVERIFY(content.size() < 1024);
    QVERIFY(!content.contains("/9j/"));
    QVERIFY(hasImage(model.requests[1]));
    QString error;
    const auto ref = AttachmentRef::fromJson(refs[0].toObject(), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(ref.path.endsWith(".jpeg"));
    const auto bytes =
        AttachmentStore(data.path()).read("jpeg-owner", ref, {}, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(bytes.startsWith("\xFF\xD8\xFF"));
    QVERIFY(!QImage::fromData(bytes, "JPEG").isNull());
  }
  void directLargeDomAndConsoleAreCapped_data() {
    QTest::addColumn<bool>("console");
    QTest::addColumn<QString>("protocol");
    QTest::addColumn<bool>("redaction");
    for (const auto &protocol :
         QStringList{"openai", "responses", "anthropic", "google"})
      for (bool console : {false, true})
        QTest::newRow(qPrintable(protocol + (console ? "-console" : "-DOM")))
            << console << protocol << false;
    QTest::newRow("redaction-expands") << false << QString("openai") << true;
  }
  void directLargeDomAndConsoleAreCapped() {
    QFETCH(bool, console);
    QFETCH(QString, protocol);
    QFETCH(bool, redaction);
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    if (console) {
      QSignalSpy logged(&browser, &BrowserWidget::devToolsEventReceived);
      browser.callDevToolsRequest(
          "setup", "Runtime.evaluate",
          R"({"expression":"for(let i=0;i<80;i++)console.log('LOG-'+i+'-'+ 'x'.repeat(1500));true","returnByValue":true})");
      const auto logCount = [&] {
        int count = 0;
        for (const auto &event : logged)
          if (event[0].toString() == "Runtime.consoleAPICalled")
            ++count;
        return count;
      };
      QTRY_COMPARE_WITH_TIMEOUT(logCount(), 80, 5000);
    }
    AgentEngine engine;
    auto provider = model.provider(protocol);
    if (redaction)
      provider.apiKey = "L";
    engine.setProvider(provider);
    engine.setAlwaysAllow(true);
    AgentToolHost host(&engine, &bridge, "output-owner", data.path());
    const auto name = console ? QString("readConsoleLogs") : QString("sendCDP");
    const QJsonObject args =
        console
            ? QJsonObject{{"id", tab}, {"limit", 100}}
            : QJsonObject{
                  {"tabId", tab},
                  {"method", "Runtime.evaluate"},
                  {"params",
                   QJsonObject{{"expression",
                                "document.getElementById('large').textContent"},
                               {"returnByValue", true}}}};
    engine.submit("Inspect bounded fixture output");
    QTRY_COMPARE(model.requests.size(), 1);
    model.respond(0, reply(protocol, true, name, args));
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 2, 15000);
    model.respond(1, reply(protocol, false));
    QTRY_VERIFY(!engine.isBusy());
    const auto output = toolRow(engine).value("content").toString();
    qInfo() << "direct model output UTF8 bytes" << output.toUtf8().size();
    QVERIFY2(output.toUtf8().size() <= 40 * 1024,
             qPrintable(QString::number(output.toUtf8().size())));
    QVERIFY(output.contains("truncated", Qt::CaseInsensitive));
    QVERIFY(output.contains(redaction ? "[REDACTED]"
                            : console ? "LOG-"
                                      : "LLLL"));
    const auto wire =
        QJsonDocument(model.requests[1]).toJson(QJsonDocument::Compact);
    QVERIFY(!wire.contains(QByteArray(50000, console ? 'x' : 'L')));
  }
  void sandboxRetainsRawScreenshotAndLargeCdp() {
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    AgentEngine engine;
    engine.setProvider(model.provider("openai"));
    engine.setAlwaysAllow(true);
    AgentToolHost host(&engine, &bridge, "sandbox-owner", data.path());
    const auto script =
        QString(
            R"JS(const r=await API.sendCDP(%1,'Runtime.evaluate',{expression:"document.getElementById('large').textContent",returnByValue:true});const img=await API.sendCDP(%1,'Page.captureScreenshot',{format:'png',clip:{x:0,y:0,width:320,height:240,scale:1}});const path=await API.createAttachment('screen.png',Buffer.from(img.data,'base64'));return {length:r.result.value.length,path};)JS")
            .arg(quote(tab));
    engine.submit("Sandbox fixture screenshot");
    QTRY_COMPARE(model.requests.size(), 1);
    model.respond(
        0, reply("openai", true, "executeSandboxJs",
                 {{"explanation", "Capture fixture"}, {"script", script}}));
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 2, 15000);
    model.respond(1, reply("openai", false));
    QTRY_VERIFY(!engine.isBusy());
    const auto row = toolRow(engine);
    QVERIFY2(row.value("content").toString().contains("180000"),
             qPrintable(row.value("content").toString()));
    QCOMPARE(
        row.value("metadata").toObject().value("attachments").toArray().size(),
        1);
    QVERIFY(hasImage(model.requests[1]));
  }
  void textBudgetPreservesUnicodeAndSmallExactPayload() {
    const QString small = R"({"result":{"value":"ordinary"}})";
    QCOMPARE(capBrowserModelOutput(small), small);
    const auto input = QString::fromUtf8("😀\"\\\n").repeated(20000);
    for (bool error : {false, true}) {
      const auto output = capBrowserModelOutput(input, error);
      const auto saved = (error ? QString("Error: ") : QString()) + output;
      const auto encoded =
          QJsonDocument(QJsonArray{saved}).toJson(QJsonDocument::Compact);
      QVERIFY(encoded.size() - 2 <= BrowserModelOutputBytes);
      QVERIFY(output.contains("truncated"));
      QVERIFY(!output.contains(QChar::ReplacementCharacter));
      for (int i = 0; i < output.size(); ++i) {
        if (output[i].isHighSurrogate()) {
          QVERIFY(i + 1 < output.size() && output[i + 1].isLowSurrogate());
          ++i;
        } else
          QVERIFY(!output[i].isLowSurrogate());
      }
    }
  }
  void privateValueIsRedactedBeforeTruncation() {
    const QString key = "SYNTHETIC_PRIVATE_VALUE_123456789";
    const QString raw = QString(40500, 'x') + key + QString(20000, 'y');
    const auto result = capBrowserModelOutput(raw, false, key);
    QVERIFY(!result.contains("SYNTHETIC"));
    QVERIFY(!result.contains("PRIVATE_VALUE"));
    QVERIFY(result.contains("truncated"));
    auto retained = result;
    retained.replace(key, "[REDACTED]");
    QVERIFY(QJsonDocument(QJsonArray{retained})
                    .toJson(QJsonDocument::Compact)
                    .size() -
                2 <=
            BrowserModelOutputBytes);
  }
  void invalidScreenshotsFailWithoutEchoingData_data() {
    QTest::addColumn<QString>("output");
    QTest::newRow("not-json") << QString("not an image");
    QTest::newRow("invalid-base64")
        << QString(R"({"data":"PRIVATE_INVALID_@@@@"})");
    QTest::newRow("not-image") << QString::fromUtf8(
        QJsonDocument(
            QJsonObject{
                {"data", QString::fromLatin1(
                             QByteArray("PRIVATE_NOT_IMAGE").toBase64())}})
            .toJson(QJsonDocument::Compact));
    QTest::newRow("wrong-type") << QString(R"({"data":42})");
    QTest::newRow("transport-limit") << QString(2 * 1024 * 1024 + 1, 'x');
  }
  void invalidScreenshotsFailWithoutEchoingData() {
    QFETCH(QString, output);
    const auto result = decodeBrowserScreenshot(output);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.bytes.isEmpty());
    QVERIFY(!result.error.contains("PRIVATE"));
  }
  void failedSnapshotStorageDoesNotPublishBase64() {
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    AgentEngine engine;
    engine.setProvider(model.provider("openai"));
    engine.setAlwaysAllow(true);
    AgentToolHost host(&engine, &bridge, "failed-owner", data.path());
    QFile blocker(data.filePath("attachment-snapshots"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("owned fixture blocker");
    blocker.close();
    engine.submit("Capture fixture");
    QTRY_COMPARE(model.requests.size(), 1);
    model.respond(0, reply("openai", true, "sendCDP",
                           {{"tabId", tab},
                            {"method", "Page.captureScreenshot"},
                            {"params", QJsonObject{{"format", "png"}}}}));
    QTRY_COMPARE_WITH_TIMEOUT(model.requests.size(), 2, 15000);
    model.respond(1, reply("openai", false));
    QTRY_VERIFY(!engine.isBusy());
    const auto row = toolRow(engine);
    QVERIFY(row.value("content").toString().startsWith("Error:"));
    QVERIFY(row.value("content").toString().size() < 1024);
    QVERIFY(row.value("metadata")
                .toObject()
                .value("attachments")
                .toArray()
                .isEmpty());
    QVERIFY(!hasImage(model.requests[1]));
  }
  void attachmentObserverCannotSettleReplacement_data() {
    QTest::addColumn<QString>("action");
    for (const auto &a : {"cancel-replace", "delete-host", "delete-engine"})
      QTest::newRow(a) << QString(a);
  }
  void attachmentObserverCannotSettleReplacement() {
    QFETCH(QString, action);
    QTemporaryDir data, profile;
    PageServer page;
    Server model;
    BrowserToolBridge bridge;
    BrowserWidget browser;
    const auto tab = bridge.registerTab(&browser, "fixture-tab");
    QVERIFY(load(browser, page, profile.path()));
    QPointer<AgentEngine> engine = new AgentEngine;
    engine->setProvider(model.provider("openai"));
    engine->setAlwaysAllow(true);
    QPointer<AgentToolHost> host =
        new AgentToolHost(engine, &bridge, "observer-owner", data.path());
    bool called = false;
    connect(host, &AgentToolHost::attachmentCreated, this, [&] {
      called = true;
      if (action == "delete-host")
        delete host.data();
      else if (action == "delete-engine")
        delete engine.data();
      else {
        engine->cancel();
        engine->submit("Replacement owns its result");
      }
    });
    engine->submit("Capture fixture");
    QTRY_COMPARE(model.requests.size(), 1);
    model.respond(0, reply("openai", true, "sendCDP",
                           {{"tabId", tab},
                            {"method", "Page.captureScreenshot"},
                            {"params", QJsonObject{{"format", "png"}}}}));
    QTRY_VERIFY_WITH_TIMEOUT(called, 15000);
    if (action == "cancel-replace") {
      QTRY_COMPARE(model.requests.size(), 2);
      QVERIFY(engine && engine->isBusy());
      QVERIFY(!hasImage(model.requests[1]));
      model.respond(1, reply("openai", false));
      QTRY_VERIFY(!engine->isBusy());
      const auto row = toolRow(*engine);
      QVERIFY(row.value("metadata")
                  .toObject()
                  .value("attachments")
                  .toArray()
                  .isEmpty());
      QVERIFY(row.value("content").toString().contains("cancel",
                                                       Qt::CaseInsensitive));
    } else {
      QCOMPARE(model.requests.size(), 1);
      QVERIFY(!engine || !engine->isBusy());
    }
    delete host.data();
    delete engine.data();
  }
};
QTEST_MAIN(BrowserOutputTests)
#include "test_browser_output.moc"
