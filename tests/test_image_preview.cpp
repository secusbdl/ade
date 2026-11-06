#include "ui/ImagePreview.h"
#include "ui/TrustedImage.h"
#include <QBuffer>
#include <QGraphicsView>
#include <QPushButton>
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
};
QTEST_MAIN(ImagePreviewTest)
#include "test_image_preview.moc"
