#pragma once
#include <QJsonObject>
#include <QWidget>
class QVBoxLayout;
namespace ade {
class FileDiffView final : public QWidget {
  Q_OBJECT
public:
  explicit FileDiffView(QWidget *parent = nullptr);
  void setDiff(const QJsonObject &diff);
  static QString unifiedPreview(const QString &before, const QString &after);

private:
  QVBoxLayout *m_layout;
  QJsonObject m_diff;
};
} // namespace ade
