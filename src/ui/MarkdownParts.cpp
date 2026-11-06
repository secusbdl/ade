#include "MarkdownParts.h"
#include "editor/CodeEditor.h"
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace ade {
namespace {
QUrl scopedImageUrl(const QString &name) {
    const QUrl url(name);
    if(url.scheme().compare("path",Qt::CaseInsensitive)==0)return url;
    // Relative mounted paths are handed to the host, never opened here. All
    // filesystem/network authorization remains with its captured resolver.
    if(url.isRelative()&&!name.startsWith('/')&&!name.startsWith('\\')&&!name.isEmpty())return QUrl("path:"+name);
    return {};
}
class TranscriptDocument final : public QTextDocument {
public:
    explicit TranscriptDocument(QObject *parent):QTextDocument(parent){setResourceProvider([](const QUrl&){return QVariant();});}
protected:
    QVariant loadResource(int,const QUrl &) override {
        // QTextDocument's default implementation can try a local file after
        // its parent QTextBrowser returns an invalid resource. Override the
        // document itself so that fallback is never entered.
        setProperty("blockedResourceLoads",property("blockedResourceLoads").toInt()+1);
        return {};
    }
};
void restoreSelection(QTextEdit *edit,const QTextCursor &old,const QString &selected) {
    QTextCursor cursor(edit->document());
    const int limit=qMax(0,edit->document()->characterCount()-1);
    if(!selected.isEmpty()) {
        auto plain=edit->toPlainText();auto match=selected;match.replace(QChar(0x2029),'\n');
        int start=plain.indexOf(match,qMax(0,old.selectionStart()-32));
        if(start<0)start=plain.indexOf(match);
        if(start>=0) {
            cursor.setPosition(start);cursor.setPosition(start+match.size(),QTextCursor::KeepAnchor);
            edit->setTextCursor(cursor);return;
        }
    }
    cursor.setPosition(qMin(old.anchor(),limit));
    cursor.setPosition(qMin(old.position(),limit),QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
}

struct Segment { bool code=false;QString text,language;bool closed=true; };
QList<Segment> splitFences(const QString &text) {
    // Fenced blocks only. QTextDocument handles the remaining Markdown. An
    // unfinished fence stays a code card, rather than flashing into prose.
    static const QRegularExpression opener(QStringLiteral("^ {0,3}(`{3,}|~{3,})([^\\r\\n]*)\\r?$"));
    QList<Segment> result;QString prose,code,language;QChar marker;int markerCount=0;
    qsizetype offset=0;
    while(offset<text.size()) {
        const auto newline=text.indexOf('\n',offset);const auto end=newline<0?text.size():newline;
        const auto line=text.mid(offset,end-offset);const auto full=line+(newline<0?QString():QStringLiteral("\n"));
        if(marker.isNull()) {
            const auto match=opener.match(line);
            if(match.hasMatch()) {
                if(!prose.isEmpty()){result.append({false,prose,{},true});prose.clear();}
                marker=match.captured(1).at(0);markerCount=match.capturedLength(1);
                language=match.captured(2).trimmed().section(' ',0,0).left(80);
            } else prose+=full;
        } else {
            const auto trimmed=line.trimmed();int count=0;
            while(count<trimmed.size()&&trimmed[count]==marker)++count;
            const bool closes=count>=markerCount&&trimmed.mid(count).trimmed().isEmpty()&&line.indexOf(marker)<=3;
            if(closes){result.append({true,code,language,true});code.clear();language.clear();marker={};}
            else code+=full;
        }
        if(newline<0)break;
        offset=newline+1;
    }
    if(!marker.isNull())result.append({true,code,language,false});
    if(!prose.isEmpty())result.append({false,prose,{},true});
    return result;
}

QString extensionFor(QString language) {
    language=language.toLower();
    const QHash<QString,QString> aliases={{"javascript","js"},{"typescript","ts"},{"python","py"},{"c++","cpp"},{"shell","sh"},{"powershell","ps1"},{"markdown","md"},{"yml","yaml"}};
    return aliases.value(language,language);
}

class CodeBlock final : public QFrame {
public:
    explicit CodeBlock(QWidget *parent):QFrame(parent) {
        setObjectName("transcriptCodeBlock");setProperty("segmentKind","code");
        auto *layout=new QVBoxLayout(this);layout->setContentsMargins(8,4,8,5);layout->setSpacing(3);
        auto *header=new QHBoxLayout;label=new QLabel(this);label->setObjectName("codeLanguage");
        copy=new QPushButton("Copy",this);copy->setObjectName("copyCode");copy->setCursor(Qt::PointingHandCursor);copy->setFixedHeight(24);
        header->addWidget(label);header->addStretch();header->addWidget(copy);layout->addLayout(header);
        code=new CodeEditor(this);code->setObjectName("codeContent");code->setReadOnly(true);code->setFrameShape(QFrame::NoFrame);
        code->setLineWrapMode(QPlainTextEdit::NoWrap);code->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);layout->addWidget(code);
        connect(copy,&QPushButton::clicked,this,[this]{
            const QPointer<CodeBlock> self(this);const auto value=source;
            QApplication::clipboard()->setText(value);if(!self)return;
            copy->setText("Copied");QTimer::singleShot(2000,this,[this]{copy->setText("Copy");});
        });
        applyColors();
    }
    void update(const Segment &segment,bool streaming) {
        label->setText(segment.language.isEmpty()?QStringLiteral("text"):segment.language);
        copy->setVisible(!streaming&&segment.closed);copy->setToolTip("Copy code");
        if(source!=segment.text||!initialized) {
            const auto old=code->textCursor();
            if(initialized&&segment.text.startsWith(source)) {
                QTextCursor end(code->document());end.movePosition(QTextCursor::End);end.insertText(segment.text.mid(source.size()));
            } else code->setPlainText(segment.text);
            QTextCursor cursor(code->document());const int limit=qMax(0,code->document()->characterCount()-1);
            cursor.setPosition(qMin(old.anchor(),limit));cursor.setPosition(qMin(old.position(),limit),QTextCursor::KeepAnchor);code->setTextCursor(cursor);
            source=segment.text;initialized=true;
        }
        if(language!=segment.language){language=segment.language;code->setFileName("code."+extensionFor(language));}
        measure();
    }
    void refreshAppearance(){applyColors();measure();}
protected:
    void changeEvent(QEvent *event) override {QFrame::changeEvent(event);if(event->type()==QEvent::PaletteChange||event->type()==QEvent::FontChange){applyColors();measure();}}
    void resizeEvent(QResizeEvent *event) override {QFrame::resizeEvent(event);measure();}
private:
    void measure(){if(!code)return;const int lines=qMax(1,code->blockCount());code->setFixedHeight(qBound(28,lines*code->fontMetrics().lineSpacing()+14,350));}
    void applyColors(){
        if(applyingColors)return;
        applyingColors=true;
        const QWidget *owner=parentWidget();while(owner&&owner->objectName()!="conversationView")owner=owner->parentWidget();
        const bool dark=(owner?owner->palette():qApp->palette()).color(QPalette::Window).lightness()<128;
        setProperty("expectedDark",dark);
        auto p=owner?owner->palette():qApp->palette();p.setColor(QPalette::Window,dark?QColor("#25282f"):QColor("#f3f5f7"));
        const auto background=p.color(QPalette::Window).name();
        setStyleSheet("QFrame#transcriptCodeBlock{background:"+background+";border:1px solid "+QString(dark?"#3b4048":"#dfe3e8")+";border-radius:7px;} QPushButton#copyCode{padding:2px 7px;border:0;background:transparent;} QPlainTextEdit#codeContent{background:"+background+";color:"+p.color(QPalette::Text).name()+";}");
        setPalette(p);setAutoFillBackground(true);
        if(code){auto cp=p;cp.setColor(QPalette::Base,p.color(QPalette::Window));cp.setColor(QPalette::PlaceholderText,dark?QColor("#969eac"):QColor("#69717f"));code->setPalette(cp);code->setDarkTheme(dark);const int pixels=QFontInfo(owner?owner->font():font()).pixelSize();code->setEditorZoom(qRound(100.0*pixels/14));auto small=font();small.setPixelSize(qMax(9,qRound(pixels*12.0/14)));label->setFont(small);copy->setFont(small);}
        applyingColors=false;
    }
    QLabel *label=nullptr;QPushButton *copy=nullptr;CodeEditor *code=nullptr;QString source,language;bool initialized=false,applyingColors=false;
};
} // namespace

TranscriptText::TranscriptText(QWidget *parent):QTextBrowser(parent) {
    setDocument(new TranscriptDocument(this));
    // Qt's built-in image object handler separately retries QImageReader(name)
    // after document resource denial. Replace it before parsing any Markdown.
    m_imageHandler=new TranscriptImagePlaceholder(document());
    document()->documentLayout()->registerHandler(QTextFormat::ImageObject,m_imageHandler);
    setObjectName("markdownSegment");setProperty("segmentKind","markdown");setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);setOpenExternalLinks(false);setReadOnly(true);setUndoRedoEnabled(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);setStyleSheet("background:transparent;border:0;padding:0;");
    document()->setDocumentMargin(0);document()->setDefaultStyleSheet("pre,code{font-family:'Roboto Mono';}p{margin-top:0;margin-bottom:7px;}h1{font-size:18px;}h2{font-size:16px;}h3{font-size:15px;}");
    connect(document(),&QTextDocument::contentsChanged,this,&TranscriptText::measure);
}
QSizeF TranscriptImagePlaceholder::intrinsicSize(QTextDocument *document,int,const QTextFormat &format){
    const auto image=m_images.value(format.property(QTextFormat::ImageName).toString());
    if(!image.isNull()){
        const QSizeF natural=image.size(),limit(qMax(1.0,document->textWidth()),384);
        return natural.width()>limit.width()||natural.height()>limit.height()?natural.scaled(limit,Qt::KeepAspectRatio):natural;
    }
    const QString label="Image: "+format.property(QTextFormat::ImageAltText).toString();const QFontMetricsF metrics(document->defaultFont());return {qBound(50.0,metrics.horizontalAdvance(label)+12,220.0),metrics.height()+8};
}
void TranscriptImagePlaceholder::drawObject(QPainter *painter,const QRectF &rect,QTextDocument *document,int,const QTextFormat &format){
    const auto image=m_images.value(format.property(QTextFormat::ImageName).toString());
    if(!image.isNull()){painter->save();painter->setRenderHint(QPainter::SmoothPixmapTransform);painter->drawImage(rect,image);painter->restore();return;}
    painter->save();painter->setFont(document->defaultFont());painter->setPen(QColor("#808896"));painter->drawRoundedRect(rect.adjusted(0.5,0.5,-0.5,-0.5),3,3);const auto label="Image: "+format.property(QTextFormat::ImageAltText).toString();painter->drawText(rect.adjusted(6,0,-6,0),Qt::AlignVCenter|Qt::AlignLeft,painter->fontMetrics().elidedText(label,Qt::ElideRight,qMax(1,int(rect.width()-12))));painter->restore();
}
void TranscriptText::setSource(const QString &source,bool markdown) {
    if(m_initialized&&m_source==source&&m_markdown==markdown)return;
    const auto old=textCursor();const auto selected=old.selectedText();
    m_source=source;m_markdown=markdown;m_initialized=true;++m_updates;
    if(markdown)document()->setMarkdown(source,QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub)|QTextDocument::MarkdownNoHTML);
    else document()->setPlainText(source);
    refreshColors();restoreSelection(this,old,selected);measure();
    m_imageUrls.clear();
    for(auto block=document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){
        const auto fragment=it.fragment();if(!fragment.isValid()||!fragment.charFormat().isImageFormat())continue;
        const auto name=fragment.charFormat().toImageFormat().name();const auto url=scopedImageUrl(name);
        if(!url.isEmpty()&&m_imageUrls.size()<32)m_imageUrls.insert(name,url);
    }
    const auto revision=++m_imageRevision;
    QTimer::singleShot(0,this,[this,revision]{
        if(revision!=m_imageRevision)return;
        const auto urls=m_imageUrls.values();const QPointer<TranscriptText> self(this);
        for(const auto &url:urls){const auto key=url.toString(QUrl::FullyEncoded);if(m_requestedImages.contains(key))continue;m_requestedImages.insert(key);emit imageRequested(url);if(!self||revision!=m_imageRevision)return;}
    });
}
bool TranscriptText::containsImage(const QUrl &url) const{return m_imageUrls.values().contains(url);}
bool TranscriptText::setTrustedImage(const QUrl &url,const QImage &image){
    if(image.isNull()||!containsImage(url))return false;
    for(auto it=m_imageUrls.cbegin();it!=m_imageUrls.cend();++it)if(it.value()==url)m_imageHandler->setImage(it.key(),image);
    document()->setProperty("trustedImageCount",document()->property("trustedImageCount").toInt()+1);
    const QPointer<TranscriptText> self(this);document()->markContentsDirty(0,document()->characterCount());
    if(self){viewport()->update();measure();}return true;
}
QVariant TranscriptText::loadResource(int,const QUrl &) {setProperty("blockedResourceLoads",property("blockedResourceLoads").toInt()+1);return {};}
void TranscriptText::refreshColors(){
    const QWidget *owner=parentWidget();while(owner&&owner->objectName()!="conversationView")owner=owner->parentWidget();
    const bool dark=(owner?owner->palette():qApp->palette()).color(QPalette::Window).lightness()<128;
    // Qt's fallback link blue is unreadable on the dark surface. Color anchor
    // runs without reparsing or disturbing selected text; resources stay denied.
    const QColor color(dark?"#93b7ff":"#225bb7");
    struct Span{int start,length;};QList<Span> spans;
    for(auto block=document()->begin();block.isValid();block=block.next())for(auto it=block.begin();!it.atEnd();++it){const auto fragment=it.fragment();if(fragment.isValid()&&fragment.charFormat().isAnchor())spans.append({fragment.position(),fragment.length()});}
    QTextCharFormat format;format.setForeground(color);
    for(const auto &span:spans){QTextCursor cursor(document());cursor.setPosition(span.start);cursor.setPosition(span.start+span.length,QTextCursor::KeepAnchor);cursor.mergeCharFormat(format);}
}
void TranscriptText::measure() {
    if(m_measurePending)return;
    m_measurePending=true;
    QTimer::singleShot(0,this,[this]{m_measurePending=false;document()->setTextWidth(qMax(1,viewport()->width()));const auto height=qMax(1,static_cast<int>(std::ceil(document()->size().height()))+2);if(height!=this->height())setFixedHeight(height);});
}
void TranscriptText::resizeEvent(QResizeEvent *event){QTextBrowser::resizeEvent(event);measure();}
void TranscriptText::changeEvent(QEvent *event){QTextBrowser::changeEvent(event);if(event->type()==QEvent::FontChange){document()->setDefaultFont(font());measure();}}
void TranscriptText::wheelEvent(QWheelEvent *event){event->ignore();}

MarkdownParts::MarkdownParts(QWidget *parent):QWidget(parent) {m_layout=new QVBoxLayout(this);m_layout->setContentsMargins(0,0,0,0);m_layout->setSpacing(8);}
void MarkdownParts::refreshAppearance(){for(auto *segment:m_segments){if(segment->property("segmentKind")=="code")static_cast<CodeBlock*>(segment)->refreshAppearance();else static_cast<TranscriptText*>(segment)->refreshColors();}}
void MarkdownParts::changeEvent(QEvent *event){QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange||event->type()==QEvent::FontChange)refreshAppearance();}
void MarkdownParts::updateText(const QString &text,bool streaming) {
    if(m_initialized&&m_text==text&&m_streaming==streaming)return;
    m_initialized=true;m_text=text;m_streaming=streaming;const auto parts=splitFences(text);
    for(qsizetype index=0;index<parts.size();++index){
        const auto &part=parts[index];const QString kind=part.code?"code":"markdown";
        bool create=index>=m_segments.size();
        if(!create&&m_segments[index]->property("segmentKind").toString()!=kind){delete m_segments.takeAt(index);create=true;}
        if(create){
            QWidget *widget;
            if(part.code)widget=new CodeBlock(this);
            else {
                auto *browser=new TranscriptText(this);widget=browser;
                connect(browser,&QTextBrowser::anchorClicked,this,[this](const QUrl &url){
                    const auto scheme=url.scheme().toLower();
                    if(QStringList{"http","https","path","tab","color"}.contains(scheme))emit linkActivated(url);
                });
                connect(browser,&TranscriptText::imageRequested,this,&MarkdownParts::imageRequested);
            }
            m_segments.insert(index,widget);m_layout->insertWidget(index,widget);
        }
        if(part.code)static_cast<CodeBlock*>(m_segments[index])->update(part,streaming);
        else static_cast<TranscriptText*>(m_segments[index])->setSource(part.text);
    }
    while(m_segments.size()>parts.size())delete m_segments.takeLast();
}
} // namespace ade
