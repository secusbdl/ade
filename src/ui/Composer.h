#pragma once
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QTextDocument>
#include <QMimeData>
#include <QImage>
#include <QUrl>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTimer>
namespace ade {
class Composer : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit Composer(QWidget* parent=nullptr) : QPlainTextEdit(parent) {
        setObjectName("composerInput");
        setPlaceholderText("Use / to plan and run commands. Use @ for context.");
        setFrameShape(QFrame::NoFrame);document()->setDocumentMargin(0);setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        connect(document(),&QTextDocument::contentsChanged,this,[this]{updateHeight();});
        setUiScale(1);
    }
    void setUiScale(double scale){m_scale=qBound(0.7,scale,1.3);auto face=font();face.setPixelSize(qRound(14*m_scale));setFont(face);updateHeight();}
    void setAvailableHeightLimit(int height){height=qMax(1,height);if(m_availableHeight==height)return;m_availableHeight=height;updateHeight();}
signals:
    void submitted();
    void filesAttached(QStringList paths);
    void imageAttached(QImage image);
    void stopRequested();
protected:
    bool canInsertFromMimeData(const QMimeData* source) const override {
        return source->hasImage() || source->hasUrls() || QPlainTextEdit::canInsertFromMimeData(source);
    }
    void insertFromMimeData(const QMimeData* source) override {
        if(source->hasUrls()) {
            QStringList paths;
            for(const auto& url:source->urls())if(url.isLocalFile())paths.append(url.toLocalFile());
            if(!paths.isEmpty()){emit filesAttached(paths);return;}
        }
        if(source->hasImage()) {
            const auto image=qvariant_cast<QImage>(source->imageData());
            if(!image.isNull()){emit imageAttached(image);return;}
        }
        QPlainTextEdit::insertFromMimeData(source);
    }
    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);updateHeight();
        if(!m_heightPending){m_heightPending=true;QTimer::singleShot(0,this,[this]{m_heightPending=false;updateHeight();});}
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->key()==Qt::Key_Escape){emit stopRequested();event->accept();return;}
        if ((event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) && !(event->modifiers() & Qt::ShiftModifier)) {
            emit submitted(); event->accept(); return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
private:
    void updateHeight(){
        if(m_adjusting)return;
        m_adjusting=true;
        qreal content=0;const int maximum=qMax(fontMetrics().lineSpacing()+qRound(8*m_scale),qMin(qRound(256*m_scale),m_availableHeight));
        for(auto block=document()->begin();block.isValid()&&content<maximum;block=block.next())content+=document()->documentLayout()->blockBoundingRect(block).height();
        // QPlainTextEdit paints wrapped placeholder lines without adding them to
        // its document. Include those real lines in the empty editor's height.
        if(document()->isEmpty()&&!placeholderText().isEmpty())content=qMax(content,qreal(fontMetrics().boundingRect(QRect(0,0,qMax(1,viewport()->width()),maximum),Qt::TextWordWrap,placeholderText()).height()));
        const int desired=qBound(fontMetrics().lineSpacing()+qRound(8*m_scale),qCeil(content)+qRound(8*m_scale),maximum);
        if(height()!=desired)setFixedHeight(desired);
        m_adjusting=false;
    }
    int m_availableHeight=QWIDGETSIZE_MAX;
    double m_scale=1;bool m_adjusting=false,m_heightPending=false;
};
}
