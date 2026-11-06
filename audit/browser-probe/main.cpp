#include "browser/browserwidget.h"
#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("ADE-Browser-Probe");
    ade::BrowserWidget browser;
    browser.setAttribute(Qt::WA_DontShowOnScreen);
    browser.resize(800, 600);
    bool scriptRequested = false;
    bool titleSeen = false;
    QObject::connect(&browser, &ade::BrowserWidget::errorOccurred, &app, [&](const QString& error) {
        qCritical().noquote() << error;
        app.exit(2);
    });
    QObject::connect(&browser, &ade::BrowserWidget::titleChanged, &app, [&](const QString& title) {
        if (title == "ADE native browser probe") titleSeen = true;
    });
    QObject::connect(&browser, &ade::BrowserWidget::loadingChanged, &app, [&](bool loading) {
        if (!loading && !scriptRequested && browser.currentUrl().isLocalFile()) {
            scriptRequested = true;
            browser.executeScript("'native-browser-ok:'+document.getElementById('test').textContent");
        }
    });
    QObject::connect(&browser, &ade::BrowserWidget::scriptResult, &app, [&](const QString& json) {
        if (json != "\"native-browser-ok:Hello from native Qt\"") { qCritical() << "Unexpected script" << json; app.exit(3); return; }
        browser.callDevTools("Runtime.evaluate", "{\"expression\":\"6*7\",\"returnByValue\":true}");
    });
    QObject::connect(&browser, &ade::BrowserWidget::devToolsResult, &app, [&](const QString& json) {
        const auto value = QJsonDocument::fromJson(json.toUtf8()).object().value("result").toObject().value("value").toInt();
        if (value != 42) { qCritical() << "Unexpected CDP" << json; app.exit(4); return; }
        browser.requestScreenshot();
    });
    QObject::connect(&browser, &ade::BrowserWidget::screenshotReady, &app, [&](const QByteArray& png) {
        if (!png.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) || !titleSeen) { qCritical() << "Invalid PNG/title" << png.size() << titleSeen; app.exit(5); return; }
        QFile file(QCoreApplication::applicationDirPath() + "/browser-probe.png");
        if (file.open(QIODevice::WriteOnly)) file.write(png);
        qInfo() << "PASS: Native Qt host, WebView2 navigation/title, page JavaScript, CDP Runtime.evaluate=42, PNG screenshot" << png.size();
        app.exit(0);
    });
    QTimer::singleShot(20000, &app, [&] { qCritical() << "Browser probe timed out"; app.exit(10); });
    browser.navigate(QUrl::fromLocalFile("D:/age/ADE/audit/browser-probe/page.html"));
    browser.show();
    return app.exec();
}
