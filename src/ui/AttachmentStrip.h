#pragma once
#include <QWidget>
#include <QJsonArray>
class QHBoxLayout;
namespace ade {
class AttachmentStrip final : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentStrip(QWidget* parent=nullptr);
    void setAttachments(const QJsonArray& references);
signals:
    void removeRequested(int index);
private:
    QHBoxLayout* m_items;
};
}
