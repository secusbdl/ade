#pragma once
#include <QImage>
#include <QWidget>
class QGraphicsView;
class QLabel;
namespace ade {
class ImagePreview final : public QWidget {
  Q_OBJECT
public:
  ImagePreview(const QString &name, const QImage &image,
               const QSize &originalSize = {}, QWidget *parent = nullptr);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void fit();
  void zoom(double factor);
  QGraphicsView *m_view;
  QLabel *m_scale;
  QSize m_imageSize;
  bool m_fitting = true;
};
} // namespace ade
