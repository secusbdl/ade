#pragma once
#include <QTextBrowser>
#include <QWidget>
#include <QAbstractTextDocumentLayout>
#include <QHash>
#include <QImage>
#include <QSet>

class QVBoxLayout;
namespace ade {
class TranscriptImagePlaceholder final : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)
public:
    explicit TranscriptImagePlaceholder(QObject *parent):QObject(parent){}
    void setImage(const QString &name,const QImage &image){m_images.insert(name,image);}
    QSizeF intrinsicSize(QTextDocument *document,int position,const QTextFormat &format) override;
    void drawObject(QPainter *painter,const QRectF &rect,QTextDocument *document,int position,const QTextFormat &format) override;
private:
    QHash<QString,QImage> m_images;
};
// Deliberately denies all document resource loading. Attachments require a host
// resolver; Markdown cannot fetch remote images or read local files implicitly.
class TranscriptText final : public QTextBrowser {
    Q_OBJECT
public:
    explicit TranscriptText(QWidget *parent=nullptr);
    void setSource(const QString &source, bool markdown=true, bool allowIncompleteLinks=true);
    QString source() const { return m_source; }
    int updateCount() const { return m_updates; }
    void refreshColors();
    bool setTrustedImage(const QUrl &url,const QImage &image);
    bool containsImage(const QUrl &url) const;
signals:
    void imageRequested(QUrl url);
protected:
    QVariant loadResource(int type,const QUrl &url) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
private:
    void measure();
    QString m_source;
    bool m_initialized=false;
    bool m_markdown=true;
    bool m_allowIncompleteLinks=true;
    bool m_measurePending=false;
    int m_updates=0;
    TranscriptImagePlaceholder *m_imageHandler=nullptr;
    QHash<QString,QUrl> m_imageUrls;
    QSet<QString> m_requestedImages;
    quint64 m_imageRevision=0;
};

class MarkdownParts final : public QWidget {
    Q_OBJECT
public:
    explicit MarkdownParts(QWidget *parent=nullptr);
    void updateText(const QString &text,bool streaming);
    void refreshAppearance();
signals:
    void linkActivated(QUrl url);
    void imageRequested(QUrl url);
protected:
    void changeEvent(QEvent *event) override;
private:
    QVBoxLayout *m_layout;
    QList<QWidget*> m_segments;
    QString m_text;
    bool m_initialized=false;
    bool m_streaming=false;
};
} // namespace ade
