#pragma once

#include <QPlainTextEdit>

class QSyntaxHighlighter;

namespace ade {
class LineNumberArea;

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent=nullptr);
    QString sourceText() const;
    void setFileName(const QString &path);
    QString languageName() const { return m_language; }
    int lineNumberAreaWidth() const;
    void paintLineNumbers(QPaintEvent *event);
    void setEditorZoom(int percent);
    // An explicit host theme takes precedence over transparent stylesheet palettes.
    void setDarkTheme(bool dark);
    int editorZoom() const { return m_zoom; }
    bool findText(const QString &query,bool backwards=false,bool caseSensitive=false,
                  bool wholeWords=false,bool regex=false,QString *error=nullptr);
    bool replaceCurrent(const QString &query,const QString &replacement,bool caseSensitive=false,
                        bool wholeWords=false,bool regex=false,QString *error=nullptr);
    int replaceAll(const QString &query,const QString &replacement,bool caseSensitive=false,
                   bool wholeWords=false,bool regex=false,QString *error=nullptr);
signals:
    void findRequested(bool replace);
    void saveRequested();
    void zoomChanged(int percent);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    void updateGutterWidth();
    void updateCurrentLine();
    void applyColors();
    void indentSelection(bool backwards);
    LineNumberArea *m_gutter=nullptr;
    QSyntaxHighlighter *m_highlighter=nullptr;
    QString m_language="Plain text";
    int m_zoom=100;
    bool m_themeExplicit=false;
    bool m_darkTheme=false;
};

} // namespace ade
