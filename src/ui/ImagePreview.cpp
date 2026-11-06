#include "ImagePreview.h"
#include <QEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
namespace ade {
ImagePreview::ImagePreview(const QString &name, const QImage &image,
                           const QSize &originalSize, QWidget *parent)
    : QWidget(parent), m_imageSize(image.size()) {
  setObjectName("imagePreview");
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  auto *title = new QLabel(name, this);
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  layout->addWidget(title);
  auto *controls = new QHBoxLayout;
  auto *fitButton = new QPushButton("Fit", this);
  fitButton->setObjectName("imageFit");
  auto *actual = new QPushButton("100%", this);
  actual->setObjectName("imageActualSize");
  auto *minus = new QPushButton("−", this);
  minus->setObjectName("imageZoomOut");
  auto *plus = new QPushButton("+", this);
  plus->setObjectName("imageZoomIn");
  minus->setFixedWidth(32);
  plus->setFixedWidth(32);
  minus->setAccessibleName("Zoom out");
  plus->setAccessibleName("Zoom in");
  m_scale = new QLabel(this);
  m_scale->setObjectName("imageScale");
  controls->addWidget(fitButton);
  controls->addWidget(actual);
  controls->addStretch();
  controls->addWidget(minus);
  controls->addWidget(m_scale);
  controls->addWidget(plus);
  layout->addLayout(controls);
  m_view = new QGraphicsView(this);
  m_view->setObjectName("imageCanvas");
  m_view->setFrameShape(QFrame::NoFrame);
  m_view->setRenderHint(QPainter::SmoothPixmapTransform);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  m_view->setMinimumSize(0, 0);
  auto *scene = new QGraphicsScene(m_view);
  scene->addPixmap(QPixmap::fromImage(image));
  scene->setSceneRect(QRectF(QPointF(), QSizeF(image.size())));
  m_view->setScene(scene);
  layout->addWidget(m_view, 1);
  m_view->viewport()->installEventFilter(this);
  auto *info = new QLabel(
      QString("%1 × %2 pixels%3")
          .arg(originalSize.isValid() ? originalSize.width() : image.width())
          .arg(originalSize.isValid() ? originalSize.height() : image.height())
          .arg(originalSize.isValid() && originalSize != image.size()
                   ? " · display scaled to 2,048 pixels"
                   : ""),
      this);
  info->setWordWrap(true);
  layout->addWidget(info);
  connect(fitButton, &QPushButton::clicked, this, [this] {
    m_fitting = true;
    fit();
  });
  connect(actual, &QPushButton::clicked, this, [this] {
    m_fitting = false;
    m_view->resetTransform();
    zoom(1);
  });
  connect(minus, &QPushButton::clicked, this, [this] { zoom(1 / 1.25); });
  connect(plus, &QPushButton::clicked, this, [this] { zoom(1.25); });
  QTimer::singleShot(0, this, [this] { fit(); });
}
void ImagePreview::fit() {
  if (!m_fitting || m_imageSize.isEmpty())
    return;
  m_view->resetTransform();
  const double factor =
      qMin(1.0, qMin(double(qMax(1, m_view->viewport()->width() - 8)) /
                         m_imageSize.width(),
                     double(qMax(1, m_view->viewport()->height() - 8)) /
                         m_imageSize.height()));
  m_view->scale(factor, factor);
  m_scale->setText(QString::number(qRound(100 * factor)) + "%");
}
void ImagePreview::zoom(double factor) {
  m_fitting = false;
  const auto current = m_view->transform().m11();
  const auto target = qBound(0.05, current * factor, 8.0);
  m_view->scale(target / current, target / current);
  m_scale->setText(QString::number(qRound(100 * target)) + "%");
}
bool ImagePreview::eventFilter(QObject *object, QEvent *event) {
  if (object == m_view->viewport() && event->type() == QEvent::Resize &&
      m_fitting)
    QTimer::singleShot(0, this, [this] { fit(); });
  return QWidget::eventFilter(object, event);
}
} // namespace ade
