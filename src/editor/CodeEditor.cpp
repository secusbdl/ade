#include "CodeEditor.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QtMath>

namespace ade {
namespace {
class CodeHighlighter : public QSyntaxHighlighter {
public:
    explicit CodeHighlighter(QTextDocument *document):QSyntaxHighlighter(document){}
    QString language="Plain text";
    bool dark=false;
protected:
    void highlightBlock(const QString &text) override {
        setCurrentBlockState(0);
        if(language=="Plain text")return;
        // Stagewise 17a0123a: code-block-{light,dark}-theme.json, generated from
        // packages/stage-ui/src/styles/code-block-syntax.css (same Monaco tokens).
        const QColor keyword(dark?"#76e2c6":"#e14238"),stringColor(dark?"#ffadce":"#975ee6"),
            number(dark?"#ffb885":"#d74089"),comment(dark?"#8e94a1":"#505868");
        QTextCharFormat commentFormat;commentFormat.setForeground(comment);commentFormat.setFontItalic(true);
        const auto match=[&](const QString &pattern,const QColor &color){
            auto matches=QRegularExpression(pattern).globalMatch(text);
            while(matches.hasNext()){const auto m=matches.next();setFormat(m.capturedStart(),m.capturedLength(),color);}
        };
        if(language=="Markdown"){
            match("^#{1,6}\\s.*$",keyword);match("`[^`]*`",stringColor);match("\\*\\*[^*]+\\*\\*",keyword);return;
        }
        if(language=="Markup")match("</?[A-Za-z][A-Za-z0-9:_-]*|/?>",keyword);
        match("\\b(?:class|struct|enum|namespace|template|typename|using|public|private|protected|virtual|override|const|static|constexpr|auto|void|int|double|float|bool|char|return|if|else|for|while|do|switch|case|break|continue|new|delete|try|catch|throw|function|let|var|async|await|import|export|from|def|self|None|True|False|and|or|not|in|is|with|as|lambda|yield|pass|true|false|null)\\b",keyword);
        match("\\b(?:0[xX][0-9a-fA-F]+|[0-9]+(?:\\.[0-9]+)?)\\b",number);
        match("\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*'|`(?:[^`\\\\]|\\\\.)*`",stringColor);
        // Apply comments after strings only when the marker is outside a quoted
        // literal. Multiline block-comment state survives document edits.
        bool quoted=false;QChar quote;bool escaped=false;int start=-1;
        if(previousBlockState()==1)start=0;
        for(int i=0;i<text.size();++i){
            if(start>=0){
                const auto end=text.indexOf("*/",i);
                if(end<0){setFormat(start,text.size()-start,commentFormat);setCurrentBlockState(1);return;}
                setFormat(start,end+2-start,commentFormat);i=end+1;start=-1;continue;
            }
            const auto ch=text[i];
            if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch==quote)quoted=false;continue;}
            if(ch=='\''||ch=='"'||ch=='`'){quoted=true;quote=ch;continue;}
            const bool hash=(language=="Python"||language=="Shell"||language=="YAML")&&ch=='#';
            const bool slashComments=language=="C++"||language=="JavaScript / TypeScript"||language=="JSON"||language=="CSS";
            if(hash||(slashComments&&ch=='/'&&i+1<text.size()&&text[i+1]=='/')){setFormat(i,text.size()-i,commentFormat);return;}
            if(slashComments&&ch=='/'&&i+1<text.size()&&text[i+1]=='*'){start=i;++i;}
        }
        if(start>=0){setFormat(start,text.size()-start,commentFormat);setCurrentBlockState(1);}
    }
};
QRegularExpression searchPattern(const QString &query,bool sensitive,bool words,bool regex,QString *error){
    if(error)error->clear();
    QString pattern=regex?query:QRegularExpression::escape(query);
    if(words)pattern="\\b(?:"+pattern+")\\b";
    QRegularExpression expression(pattern,sensitive?QRegularExpression::NoPatternOption:QRegularExpression::CaseInsensitiveOption);
    if(!expression.isValid()&&error)*error=expression.errorString();
    return expression;
}
}

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(CodeEditor *editor):QWidget(editor),m_editor(editor){setObjectName("editorLineNumbers");}
    QSize sizeHint() const override {return {m_editor->lineNumberAreaWidth(),0};}
protected:
    void paintEvent(QPaintEvent *event) override {m_editor->paintLineNumbers(event);}
private:
    CodeEditor *m_editor;
};

CodeEditor::CodeEditor(QWidget *parent):QPlainTextEdit(parent){
    setObjectName("nativeCodeEditor");setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);m_gutter=new LineNumberArea(this);
    m_highlighter=new CodeHighlighter(document());
    connect(this,&QPlainTextEdit::blockCountChanged,this,[this]{updateGutterWidth();});
    connect(this,&QPlainTextEdit::updateRequest,this,[this](const QRect &rect,int dy){
        if(dy)m_gutter->scroll(0,dy);else m_gutter->update(0,rect.y(),m_gutter->width(),rect.height());
        if(rect.contains(viewport()->rect()))updateGutterWidth();
    });
    connect(this,&QPlainTextEdit::cursorPositionChanged,this,&CodeEditor::updateCurrentLine);
    setEditorZoom(100);applyColors();updateCurrentLine();
}
QString CodeEditor::sourceText() const {
    // toPlainText() normalizes NBSP to ASCII spaces. Source editing must retain it.
    auto text=document()->toRawText();text.replace(QChar(0x2029),'\n');return text;
}
void CodeEditor::setFileName(const QString &path){
    const auto suffix=QFileInfo(path).suffix().toLower();
    if(QStringList{"cpp","cc","c","h","hpp","cxx"}.contains(suffix))m_language="C++";
    else if(QStringList{"js","jsx","ts","tsx","mjs","cjs"}.contains(suffix))m_language="JavaScript / TypeScript";
    else if(suffix=="py")m_language="Python";
    else if(suffix=="json"||suffix=="jsonc")m_language="JSON";
    else if(suffix=="md"||suffix=="mdx")m_language="Markdown";
    else if(suffix=="css"||suffix=="scss")m_language="CSS";
    else if(suffix=="html"||suffix=="htm"||suffix=="xml"||suffix=="svg")m_language="Markup";
    else if(suffix=="yaml"||suffix=="yml")m_language="YAML";
    else if(suffix=="sh"||suffix=="bash"||suffix=="ps1")m_language="Shell";
    else m_language="Plain text";
    static_cast<CodeHighlighter *>(m_highlighter)->language=m_language;m_highlighter->rehighlight();
}
int CodeEditor::lineNumberAreaWidth() const {
    int digits=1;for(int n=qMax(1,blockCount());n>=10;n/=10)++digits;
    return 18+qCeil(fontMetrics().horizontalAdvance('9')*digits);
}
void CodeEditor::updateGutterWidth(){
    setViewportMargins(lineNumberAreaWidth(),0,0,0);
    const auto rect=contentsRect();m_gutter->setGeometry(rect.left(),rect.top(),lineNumberAreaWidth(),rect.height());m_gutter->update();
}
void CodeEditor::resizeEvent(QResizeEvent *event){
    QPlainTextEdit::resizeEvent(event);const auto rect=contentsRect();m_gutter->setGeometry(rect.left(),rect.top(),lineNumberAreaWidth(),rect.height());
}
void CodeEditor::paintLineNumbers(QPaintEvent *event){
    QPainter painter(m_gutter);painter.fillRect(event->rect(),palette().base());
    auto block=firstVisibleBlock();int number=block.blockNumber();
    qreal top=blockBoundingGeometry(block).translated(contentOffset()).top();qreal bottom=top+blockBoundingRect(block).height();
    while(block.isValid()&&top<=event->rect().bottom()){
        if(block.isVisible()&&bottom>=event->rect().top()){
            // Read-only views (transcript code blocks) number lines uniformly, like the
            // upstream code-block gutter; only an editable editor emphasizes the cursor line.
            painter.setPen(!isReadOnly()&&number==textCursor().blockNumber()?palette().text().color():palette().placeholderText().color());
            painter.drawText(0,qRound(top),m_gutter->width()-10,fontMetrics().height(),Qt::AlignRight,QString::number(number+1));
        }
        block=block.next();top=bottom;bottom=top+blockBoundingRect(block).height();++number;
    }
}
void CodeEditor::updateCurrentLine(){
    QTextEdit::ExtraSelection selection;selection.format.setBackground(palette().alternateBase());
    selection.format.setProperty(QTextFormat::FullWidthSelection,true);selection.cursor=textCursor();selection.cursor.clearSelection();
    setExtraSelections(isReadOnly()?QList<QTextEdit::ExtraSelection>{}:QList<QTextEdit::ExtraSelection>{selection});m_gutter->update();
}
void CodeEditor::applyColors(){
    const bool dark=m_themeExplicit?m_darkTheme:palette().base().color().lightness()<128;
    static_cast<CodeHighlighter *>(m_highlighter)->dark=dark;m_highlighter->rehighlight();updateCurrentLine();
}
void CodeEditor::setDarkTheme(bool dark){m_themeExplicit=true;m_darkTheme=dark;applyColors();}
void CodeEditor::changeEvent(QEvent *event){QPlainTextEdit::changeEvent(event);if(event->type()==QEvent::PaletteChange)applyColors();}
void CodeEditor::setEditorZoom(int percent){
    m_zoom=qBound(50,percent,200);QFont code("Roboto Mono");code.setPixelSize(qRound(12*m_zoom/100.0));
    setFont(code);setTabStopDistance(fontMetrics().horizontalAdvance(' ')*4);updateGutterWidth();emit zoomChanged(m_zoom);
}
void CodeEditor::indentSelection(bool backwards){
    auto cursor=textCursor();const auto start=document()->findBlock(cursor.selectionStart());
    auto end=document()->findBlock(cursor.selectionEnd());
    if(cursor.hasSelection()&&cursor.selectionEnd()==end.position())end=end.previous();
    cursor.beginEditBlock();
    for(auto block=start;block.isValid();block=block.next()){
        QTextCursor line(block);if(backwards){
            const auto text=block.text();int count=0;if(text.startsWith('\t'))count=1;else while(count<4&&count<text.size()&&text[count]==' ')++count;
            if(count){line.movePosition(QTextCursor::Right,QTextCursor::KeepAnchor,count);line.removeSelectedText();}
        }else line.insertText("    ");
        if(block==end)break;
    }
    cursor.endEditBlock();
}
void CodeEditor::keyPressEvent(QKeyEvent *event){
    if(event->matches(QKeySequence::Save)){emit saveRequested();return;}
    if(event->matches(QKeySequence::Find)){emit findRequested(false);return;}
    if(event->matches(QKeySequence::Replace)){emit findRequested(true);return;}
    if(event->modifiers().testFlag(Qt::ControlModifier)){
        if(event->key()==Qt::Key_Equal||event->key()==Qt::Key_Plus){setEditorZoom(m_zoom+10);return;}
        if(event->key()==Qt::Key_Minus){setEditorZoom(m_zoom-10);return;}
        if(event->key()==Qt::Key_0){setEditorZoom(100);return;}
    }
    if(!isReadOnly()&&(event->key()==Qt::Key_Backtab||(event->key()==Qt::Key_Tab&&event->modifiers()==Qt::ShiftModifier))){indentSelection(true);return;}
    if(!isReadOnly()&&event->key()==Qt::Key_Tab&&event->modifiers()==Qt::NoModifier){
        if(textCursor().hasSelection())indentSelection(false);else insertPlainText(QString(4-textCursor().positionInBlock()%4,' '));return;
    }
    if(!isReadOnly()&&(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)&&event->modifiers()==Qt::NoModifier){
        const auto line=textCursor().block().text().left(textCursor().positionInBlock());QString indent;
        for(const auto ch:line){if(ch!=' '&&ch!='\t')break;indent+=ch;}
        auto cursor=textCursor();cursor.beginEditBlock();cursor.insertText("\n"+indent);cursor.endEditBlock();setTextCursor(cursor);return;
    }
    QPlainTextEdit::keyPressEvent(event);
}
bool CodeEditor::findText(const QString &query,bool backwards,bool sensitive,bool words,bool regex,QString *error){
    if(query.isEmpty())return false;
    const auto expression=searchPattern(query,sensitive,words,regex,error);if(!expression.isValid())return false;
    QTextDocument::FindFlags flags;
    flags.setFlag(QTextDocument::FindBackward,backwards);
    flags.setFlag(QTextDocument::FindCaseSensitively,sensitive);
    auto found=document()->find(expression,textCursor(),flags);
    if(found.isNull())found=document()->find(expression,backwards?document()->characterCount()-1:0,flags);
    if(found.isNull())return false;
    setTextCursor(found);ensureCursorVisible();return true;
}
bool CodeEditor::replaceCurrent(const QString &query,const QString &replacement,bool sensitive,bool words,bool regex,QString *error){
    if(isReadOnly()||query.isEmpty())return false;
    const auto expression=searchPattern(query,sensitive,words,regex,error);if(!expression.isValid())return false;
    auto cursor=textCursor();QTextDocument::FindFlags flags;flags.setFlag(QTextDocument::FindCaseSensitively,sensitive);
    const auto found=document()->find(expression,cursor.selectionStart(),flags);
    if(!cursor.hasSelection()||found.isNull()||found.selectionStart()!=cursor.selectionStart()||found.selectionEnd()!=cursor.selectionEnd())
        return findText(query,false,sensitive,words,regex,error);
    QString value=cursor.selectedText();value.replace(QChar(0x2029),'\n');
    cursor.insertText(regex?value.replace(expression,replacement):replacement);setTextCursor(cursor);return true;
}
int CodeEditor::replaceAll(const QString &query,const QString &replacement,bool sensitive,bool words,bool regex,QString *error){
    if(isReadOnly()||query.isEmpty())return 0;
    const auto expression=searchPattern(query,sensitive,words,regex,error);if(!expression.isValid())return -1;
    const auto original=sourceText();int count=0;auto matches=expression.globalMatch(original);
    while(matches.hasNext()){matches.next();++count;}
    if(!count)return 0;
    QString changed=original;
    if(regex)changed.replace(expression,replacement);
    else{
        // Literal replacement strings must retain backslashes such as C:\tmp.
        auto iterator=expression.globalMatch(original);QString result;qsizetype offset=0;
        while(iterator.hasNext()){const auto match=iterator.next();result+=QStringView(original).mid(offset,match.capturedStart()-offset);result+=replacement;offset=match.capturedEnd();}
        result+=QStringView(original).mid(offset);changed=std::move(result);
    }
    auto cursor=textCursor();cursor.beginEditBlock();cursor.select(QTextCursor::Document);cursor.insertText(changed);cursor.endEditBlock();setTextCursor(cursor);return count;
}

} // namespace ade
