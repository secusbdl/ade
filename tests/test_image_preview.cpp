#include "ui/ImagePreview.h"
#include "ui/TrustedImage.h"
#include <QBuffer>
#include <QApplication>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QtTest>
using namespace ade;
namespace {
QByteArray encoded(const QImage &image, const char *format = "PNG") {
  QByteArray result;
  QBuffer buffer(&result);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, format))
    return {};
  return result;
}
} // namespace
class ImagePreviewTest : public QObject {
  Q_OBJECT
private slots:
  void rasterDecodingIsBounded_data() {
    QTest::addColumn<QByteArray>("format");
    QTest::newRow("png") << QByteArray("PNG");
    QTest::newRow("jpeg") << QByteArray("JPEG");
    QTest::newRow("webp") << QByteArray("WEBP");
  }
  void rasterDecodingIsBounded() {
    QFETCH(QByteArray, format);
    QImage source(3000, 1500, QImage::Format_ARGB32);
    source.fill(Qt::red);
    const auto bytes = encoded(source, format.constData());
    QVERIFY(!bytes.isEmpty());
    QString error;
    QSize original;
    const auto image = readTrustedImage(bytes, &error, &original);
    QVERIFY2(!image.isNull(), qPrintable(error));
    QCOMPARE(original, QSize(3000, 1500));
    QVERIFY(image.width() <= 2048);
    QVERIFY(image.height() <= 2048);
    QVERIFY(image.pixelColor(image.width() / 2, image.height() / 2).red() >
            240);
  }
  void rejectsSvgUnknownAndHugeHeaders() {
    QString error;
    QVERIFY(readTrustedImage("<svg xmlns=\"http://www.w3.org/2000/svg\"><image "
                             "href=\"file:///forbidden\"/></svg>",
                             &error)
                .isNull());
    QVERIFY(!error.isEmpty());
    QVERIFY(readTrustedImage(QByteArray(16 * 1024 * 1024 + 1, 'x'), &error)
                .isNull());
    QVERIFY(error.contains("16 MiB"));
    QVERIFY(readTrustedImage(QByteArray::fromHex("89504e470d0a1a0a") + "<svg/>",
                             &error)
                .isNull());
  }
  void fitZoomAndResizeStayBounded() {
    QImage image(1200, 800, QImage::Format_ARGB32);
    image.fill(Qt::green);
    ImagePreview preview("Fixture", image, image.size());
    preview.resize(350, 300);
    preview.show();
    QCoreApplication::processEvents();
    auto *canvas = preview.findChild<QGraphicsView *>("imageCanvas");
    QVERIFY(canvas);
    QVERIFY(canvas->transform().m11() < 0.5);
    preview.findChild<QPushButton *>("imageActualSize")->click();
    QCOMPARE(canvas->transform().m11(), 1.0);
    for (int i = 0; i < 30; ++i)
      preview.findChild<QPushButton *>("imageZoomIn")->click();
    QVERIFY(canvas->transform().m11() <= 8.001);
    preview.resize(500, 400);
    QCoreApplication::processEvents();
    QVERIFY(canvas->transform().m11() > 7.99);
    preview.findChild<QPushButton *>("imageFit")->click();
    QVERIFY(canvas->transform().m11() < 1);
    QVERIFY(preview.minimumSizeHint().width() <= 350);
  }
  // Before center_image existed, Ctrl+Alt+C did nothing, so the manual zoom
  // and scroll position stayed and the scale label differed from the fresh fit.
  void centerShortcutRefitsAfterManualZoom() {
    QImage image(1200, 800, QImage::Format_ARGB32);
    image.fill(Qt::green);
    ImagePreview preview("Fixture", image, image.size());
    preview.resize(350, 300);
    preview.show();
    preview.activateWindow();
    QApplication::setActiveWindow(&preview);
    QTRY_VERIFY(preview.isActiveWindow());
    auto *canvas = preview.findChild<QGraphicsView *>("imageCanvas");
    auto *scale = preview.findChild<QLabel *>("imageScale");
    QVERIFY(canvas && scale);
    QTRY_VERIFY(canvas->transform().m11() < 0.5);
    const auto fittedText = scale->text();
    const auto fitted = canvas->transform().m11();
    for (int click = 0; click < 3; ++click)
      preview.findChild<QPushButton *>("imageZoomIn")->click();
    QVERIFY(canvas->transform().m11() > fitted * 1.5);
    QVERIFY(scale->text() != fittedText);
    // Distance, in viewport pixels, from the viewport center to the point
    // where the scene center is drawn.
    const auto offCenter = [canvas] {
      const auto drawn =
          canvas->viewportTransform().map(canvas->sceneRect().center());
      const auto middle = QRectF(canvas->viewport()->rect()).center();
      return qMax(qAbs(drawn.x() - middle.x()), qAbs(drawn.y() - middle.y()));
    };
    // Scroll to the top-left corner of the zoomed image.
    QCoreApplication::processEvents();
    canvas->horizontalScrollBar()->setValue(
        canvas->horizontalScrollBar()->minimum());
    canvas->verticalScrollBar()->setValue(
        canvas->verticalScrollBar()->minimum());
    QVERIFY2(offCenter() > 10, qPrintable(QString::number(offCenter())));
    canvas->setFocus();
    QTest::keyClick(canvas, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::processEvents();
    QCOMPARE(scale->text(), fittedText);
    QVERIFY(qAbs(canvas->transform().m11() - fitted) < 1e-9);
    QVERIFY(canvas->transform().m11() <= 1);
    QTRY_VERIFY2(offCenter() <= 1.0, qPrintable(QString::number(offCenter())));
  }
  void centerShortcutRequiresFocusWithinPreview() {
    QImage image(1200, 800, QImage::Format_ARGB32);
    image.fill(Qt::green);
    ImagePreview preview("Fixture", image, image.size());
    preview.resize(350, 300);
    preview.show();
    auto *canvas = preview.findChild<QGraphicsView *>("imageCanvas");
    QVERIFY(canvas);
    QTRY_VERIFY(canvas->transform().m11() < 0.5);
    preview.findChild<QPushButton *>("imageZoomIn")->click();
    const auto zoomed = canvas->transform().m11();
    QLineEdit other;
    other.show();
    other.activateWindow();
    QApplication::setActiveWindow(&other);
    QTRY_VERIFY(other.isActiveWindow());
    other.setFocus();
    QTest::keyClick(&other, Qt::Key_C, Qt::ControlModifier | Qt::AltModifier);
    QCoreApplication::processEvents();
    QCOMPARE(canvas->transform().m11(), zoomed);
  }
};
QTEST_MAIN(ImagePreviewTest)
#include "test_image_preview.moc"
