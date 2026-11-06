#pragma once
#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QTextDocument>
#include <QMimeData>
#include <QImage>
#include <QUrl>
namespace ade {
class Composer : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit Composer(QWidget* parent=nullptr) : QPlainTextEdit(parent) {
        setObjectName("composerInput");
        setPlaceholderText("Use / to plan and run commands. Use @ for context.");
        setFrameShape(QFrame::NoFrame); setMinimumHeight(74); setMaximumHeight(220);
        connect(document(),&QTextDocument::contentsChanged,this,[this]{updateHeight();});
        setFixedHeight(74);
    }
signals:
    void submitted();
    void filesAttached(QStringList paths);
    void imageAttached(QImage image);
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
    }
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key()==Qt::Key_Return || event->key()==Qt::Key_Enter) && !(event->modifiers() & Qt::ShiftModifier)) {
            emit submitted(); event->accept(); return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
private:
    void updateHeight(){const int desired=qBound(74,qMax(1,document()->lineCount())*fontMetrics().lineSpacing()+14,220);if(height()!=desired)setFixedHeight(desired);}
};
}
