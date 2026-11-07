#include "editor/CodeEditor.h"
#include "editor/EditorWidget.h"
#include "editor/TextFile.h"

#include <QtTest>
#include <QFile>
#include <QDir>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextLayout>
#include <QScopeGuard>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace ade;
namespace {
bool put(const QString &path,const QByteArray &bytes){QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size();}
QByteArray get(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
QByteArray encoded(const QString &text,int encoding,bool bom){
    QStringEncoder codec(static_cast<QStringConverter::Encoding>(encoding),QStringConverter::Flag::Stateless);
    QByteArray bytes=codec(text);
    if(bom)bytes.prepend(QByteArray::fromHex(encoding==QStringConverter::Utf8?"efbbbf":encoding==QStringConverter::Utf16LE?"fffe":"feff"));
    return bytes;
}
void append(CodeEditor *editor,const QString &text){auto cursor=editor->textCursor();cursor.movePosition(QTextCursor::End);cursor.insertText(text);editor->setTextCursor(cursor);}
QTextCharFormat tokenFormat(CodeEditor *editor,int position){
    const auto block=editor->document()->findBlock(position);
    for(const auto &range:block.layout()->formats())if(position-block.position()>=range.start&&position-block.position()<range.start+range.length)return range.format;
    return {};
}
}

class EditorTest : public QObject {
    Q_OBJECT
private slots:
    void encodingAndEolRoundTrip_data();
    void encodingAndEolRoundTrip();
    void mixedAndUnicodeDelimitersStayLossless();
    void invalidInputIsRejectedWithoutReplacingOpenBuffer();
    void detectsContentChangeEvenWithoutMtimeChange();
    void cleanReloadAndDirtyConflict();
    void deletedFileRequiresExplicitRecreate();
    void closeChoicesNeverSilentlyDiscard();
    void saveFailureKeepsEdits();
    void replacementDeniedByReaderPreservesBytesAndCanRetry_data();
    void replacementDeniedByReaderPreservesBytesAndCanRetry();
    void undoTracksSavedContent();
    void partialUndoRestoresMixedDelimiterMetadata();
    void findReplacePreservesLiteralsAndUndo();
    void gutterSyntaxAndIndentation();
    void readOnlyLineNumbersAreUniform();
    void explicitThemeSurvivesTransparentPalette();
    void watchesAtomicExternalReplacement();
};

void EditorTest::encodingAndEolRoundTrip_data(){
    QTest::addColumn<int>("encoding");QTest::addColumn<bool>("bom");QTest::addColumn<QString>("eol");
    for(const auto encoding:{QStringConverter::Utf8,QStringConverter::Utf16LE,QStringConverter::Utf16BE})
        for(const auto &eol:{QString("\n"),QString("\r\n"),QString("\r")})
            for(const bool bom:{false,true}){
                if(encoding!=QStringConverter::Utf8&&!bom)continue;
                const auto name=QString::number(encoding)+"-"+QString::number(bom)+"-"+eol.toUtf8().toHex();
                QTest::newRow(qPrintable(name))<<int(encoding)<<bom<<eol;
            }
}
void EditorTest::encodingAndEolRoundTrip(){
    QFETCH(int,encoding);QFETCH(bool,bom);QFETCH(QString,eol);
    QTemporaryDir directory;const auto path=directory.filePath("source.txt");
    const QString original=QString::fromUtf8("alpha 日本語 😀\u00a0")+eol+"omega"+eol;
    const auto bytes=encoded(original,encoding,bom);QVERIFY(put(path,bytes));
    EditorWidget widget;QString error;QVERIFY2(widget.openFile(path,&error),qPrintable(error));
    QVERIFY(!widget.isModified());QVERIFY2(widget.save(&error),qPrintable(error));QCOMPARE(get(path),bytes);
    QVERIFY(widget.editor()->sourceText().contains(QChar(0x00a0)));
    auto cursor=widget.editor()->textCursor();cursor.setPosition(0);cursor.movePosition(QTextCursor::Right,QTextCursor::KeepAnchor,5);cursor.insertText("beta");
    QVERIFY(widget.isModified());QVERIFY2(widget.save(&error),qPrintable(error));
    QString expected=original;expected.replace(0,5,"beta");QCOMPARE(get(path),encoded(expected,encoding,bom));QVERIFY(!widget.isModified());
}
void EditorTest::mixedAndUnicodeDelimitersStayLossless(){
    QTemporaryDir directory;const auto path=directory.filePath("mixed.txt");
    const auto original=QString::fromUtf8("one\r\ntwo\nthree\rfour\u2028five\u2029six\u00a0end");
    QVERIFY(put(path,original.toUtf8()));EditorWidget widget;QVERIFY(widget.openFile(path));
    QCOMPARE(widget.editor()->sourceText(),QString::fromUtf8("one\ntwo\nthree\nfour\nfive\nsix\u00a0end"));
    QVERIFY(widget.save());QCOMPARE(get(path),original.toUtf8());
    auto cursor=widget.editor()->textCursor();cursor.setPosition(5);cursor.insertText("X");
    QVERIFY(widget.save());QString changed=original;changed.insert(6,"X");QCOMPARE(get(path),changed.toUtf8());
    auto text=widget.editor()->sourceText();auto endings=QVector<LineEnding>{LineEnding::CRLF,LineEnding::LF,LineEnding::CR,LineEnding::LineSeparator,LineEnding::ParagraphSeparator};
    const auto more=TextFileIO::reconcileEndings(text,"new\n"+text,endings,LineEnding::CRLF);
    QCOMPARE(more.size(),6);QCOMPARE(more[0],LineEnding::CRLF);QCOMPARE(more.mid(1),endings);
}
void EditorTest::invalidInputIsRejectedWithoutReplacingOpenBuffer(){
    TextFile parsed;QString error;
    for(const auto &bytes:{QByteArray::fromHex("c328"),QByteArray::fromHex("e282"),QByteArray::fromHex("fffe00"),QByteArray::fromHex("fffe00d8"),QByteArray::fromHex("fffe0000"),QByteArray("a\0b",3)}){
        QVERIFY2(!TextFileIO::decode(bytes,&parsed,&error),bytes.toHex().constData());QVERIFY(!error.isEmpty());
    }
    // A second BOM is text, not another header to silently consume.
    QVERIFY(TextFileIO::decode(QByteArray::fromHex("efbbbfefbbbf61"),&parsed,&error));QCOMPARE(parsed.text,QString(QChar(0xfeff))+"a");
    QTemporaryDir directory;const auto good=directory.filePath("good.cpp"),bad=directory.filePath("bad.dat");
    QVERIFY(put(good,"int value = 1;\n"));QVERIFY(put(bad,QByteArray::fromHex("fffe00d8")));
    EditorWidget widget;QVERIFY(widget.openFile(good));QVERIFY(!widget.openFile(bad,&error));
    QCOMPARE(widget.filePath(),QFileInfo(good).canonicalFilePath());QCOMPARE(widget.editor()->sourceText(),QString("int value = 1;\n"));
    QCOMPARE(get(bad),QByteArray::fromHex("fffe00d8"));
    QVERIFY(!TextFileIO::decode(QByteArray(TextFileIO::MaxBytes+1,'x'),&parsed,&error));
}
void EditorTest::detectsContentChangeEvenWithoutMtimeChange(){
    QTemporaryDir directory;const auto path=directory.filePath("file.txt");QVERIFY(put(path,"baseline"));
    TextFile baseline;QVERIFY(TextFileIO::read(path,&baseline));const auto timestamp=QFileInfo(path).lastModified();
    QVERIFY(put(path,"external"));QFile file(path);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.setFileTime(timestamp,QFileDevice::FileModificationTime));file.close();
    QString error;QVERIFY(!TextFileIO::write(baseline,"local",false,&error));QCOMPARE(get(path),QByteArray("external"));
    QVERIFY(TextFileIO::write(baseline,"local",true,&error));QCOMPARE(get(path),QByteArray("local"));
}
void EditorTest::cleanReloadAndDirtyConflict(){
    QTemporaryDir directory;const auto path=directory.filePath("file.txt");QVERIFY(put(path,"old\r\n"));
    EditorWidget widget;QVERIFY(widget.openFile(path));
    QVERIFY(put(path,"fresh\r\n"));widget.checkExternalChanges();QCOMPARE(widget.editor()->sourceText(),QString("fresh\n"));QVERIFY(!widget.isModified());
    append(widget.editor(),"local");QVERIFY(put(path,"outside\r\n"));widget.checkExternalChanges();QVERIFY(widget.hasExternalChange());
    QCOMPARE(widget.editor()->sourceText(),QString("fresh\nlocal"));QVERIFY(!widget.save());QCOMPARE(get(path),QByteArray("outside\r\n"));
    QString error;QVERIFY(!widget.reloadFromDisk(&error));QVERIFY(widget.isModified());
    QVERIFY(widget.save(&error,true));QCOMPARE(get(path),QByteArray("fresh\r\nlocal"));QVERIFY(!widget.hasExternalChange());
    append(widget.editor()," again");QVERIFY(put(path,"reloaded\n"));widget.checkExternalChanges();QVERIFY(widget.reloadFromDisk(&error,true));
    QCOMPARE(widget.editor()->sourceText(),QString("reloaded\n"));QVERIFY(!widget.isModified());
}
void EditorTest::deletedFileRequiresExplicitRecreate(){
    QTemporaryDir directory;const auto path=directory.filePath("file.txt");QVERIFY(put(path,"original"));
    EditorWidget widget;QVERIFY(widget.openFile(path));append(widget.editor()," edit");QVERIFY(QFile::remove(path));
    widget.checkExternalChanges();QVERIFY(widget.hasExternalChange());QVERIFY(!widget.save());QVERIFY(!QFile::exists(path));
    QVERIFY(widget.save(nullptr,true));QCOMPARE(get(path),QByteArray("original edit"));
}
void EditorTest::closeChoicesNeverSilentlyDiscard(){
    QTemporaryDir directory;const auto path=directory.filePath("file.txt");QVERIFY(put(path,"original"));
    EditorWidget widget;QVERIFY(widget.openFile(path));append(widget.editor()," edit");
    QVERIFY(!widget.requestClose(EditorWidget::CloseChoice::Cancel));QVERIFY(widget.isModified());
    QVERIFY(widget.requestClose(EditorWidget::CloseChoice::Discard));QCOMPARE(get(path),QByteArray("original"));
    QVERIFY(widget.requestClose(EditorWidget::CloseChoice::Save));QCOMPARE(get(path),QByteArray("original edit"));
    append(widget.editor()," next");QVERIFY(put(path,"outside"));widget.checkExternalChanges();
    QVERIFY(!widget.requestClose(EditorWidget::CloseChoice::Save));QCOMPARE(get(path),QByteArray("outside"));
    QVERIFY(!widget.openFile(directory.filePath("another.txt")));QVERIFY(widget.isModified());
}
void EditorTest::saveFailureKeepsEdits(){
    QTemporaryDir directory;const auto child=directory.filePath("child");QVERIFY(QDir().mkdir(child));const auto path=child+"/file.txt";QVERIFY(put(path,"original"));
    EditorWidget widget;QVERIFY(widget.openFile(path));append(widget.editor()," local");
    widget.editor()->setReadOnly(true);QVERIFY(!widget.save(nullptr,true));QCOMPARE(get(path),QByteArray("original"));widget.editor()->setReadOnly(false);
    QVERIFY(QFile::remove(path));QVERIFY(QDir().rmdir(child));QString error;QVERIFY(!widget.save(&error,true));
    QVERIFY(!error.isEmpty());QVERIFY(widget.isModified());QCOMPARE(widget.editor()->sourceText(),QString("original local"));
}
void EditorTest::replacementDeniedByReaderPreservesBytesAndCanRetry_data(){
    QTest::addColumn<bool>("force");QTest::newRow("normal-save")<<false;QTest::newRow("explicit-overwrite")<<true;
}
void EditorTest::replacementDeniedByReaderPreservesBytesAndCanRetry(){
#ifdef Q_OS_WIN
    QFETCH(bool,force);QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("source.txt");
    // Match the previously reported UTF-16 LE + BOM + LF fixture. This is a
    // controlled OS sharing failure, not evidence identifying that old cause.
    const QString original=QString::fromUtf8("alpha 日本語 😀\n消えない\n");const auto bytes=encoded(original,QStringConverter::Utf16LE,true);QVERIFY(put(path,bytes));
    EditorWidget widget;QString error;QVERIFY2(widget.openFile(path,&error),qPrintable(error));
    auto cursor=widget.editor()->textCursor();cursor.setPosition(0);cursor.movePosition(QTextCursor::Right,QTextCursor::KeepAnchor,5);cursor.insertText("beta");
    const auto edited=widget.editor()->sourceText();QSignalSpy saved(&widget,&EditorWidget::fileSaved),failed(&widget,&EditorWidget::errorOccurred);
    const auto native=QDir::toNativeSeparators(path);HANDLE reader=CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    QVERIFY2(reader!=INVALID_HANDLE_VALUE,qPrintable(QString("CreateFileW fixture failed: %1").arg(GetLastError())));
    const auto release=qScopeGuard([&]{if(reader!=INVALID_HANDLE_VALUE)CloseHandle(reader);});
    // The foreign reader allows reads/writes but denies delete-sharing, which
    // an atomic replacement needs. Explicit overwrite must not bypass that.
    QVERIFY(!widget.save(&error,force));QVERIFY2(error.startsWith("Save failed; the previous file was preserved:"),qPrintable(error));
    qInfo().noquote()<<"Controlled delete-sharing denial:"<<error;
    QCOMPARE(get(path),bytes);QVERIFY(widget.isModified());QCOMPARE(widget.editor()->sourceText(),edited);QCOMPARE(saved.size(),0);QCOMPARE(failed.size(),1);
    QCOMPARE(QDir(directory.path()).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot),QStringList{"source.txt"});
    QVERIFY(CloseHandle(reader));reader=INVALID_HANDLE_VALUE;
    QVERIFY2(widget.save(&error),qPrintable(error));QString expected=original;expected.replace(0,5,"beta");
    QCOMPARE(get(path),encoded(expected,QStringConverter::Utf16LE,true));QVERIFY(!widget.isModified());QCOMPARE(saved.size(),1);
#else
    QSKIP("Windows file-sharing semantics fixture.");
#endif
}
void EditorTest::undoTracksSavedContent(){
    QTemporaryDir directory;const auto path=directory.filePath("file.txt");QVERIFY(put(path,"one\r\ntwo\n"));
    EditorWidget widget;QVERIFY(widget.openFile(path));append(widget.editor(),"edited");QVERIFY(widget.isModified());
    widget.editor()->undo();QVERIFY(!widget.isModified());QVERIFY(widget.save());QCOMPARE(get(path),QByteArray("one\r\ntwo\n"));
    widget.editor()->redo();QVERIFY(widget.isModified());QVERIFY(widget.save());widget.editor()->undo();QVERIFY(widget.isModified());
    QVERIFY(widget.save());QCOMPARE(get(path),QByteArray("one\r\ntwo\n"));
}
void EditorTest::partialUndoRestoresMixedDelimiterMetadata(){
    QTemporaryDir directory;const auto path=directory.filePath("mixed.txt");
    const QByteArray baseline="a\r\nb\nc\r\n";QVERIFY(put(path,baseline));
    EditorWidget widget;QVERIFY(widget.openFile(path));append(widget.editor(),"local");
    // Delete the minority LF boundary, then undo only that operation. The
    // document still has unsaved edits, so a baseline-only shortcut is insufficient.
    auto cursor=widget.editor()->textCursor();cursor.setPosition(3);cursor.movePosition(QTextCursor::Right,QTextCursor::KeepAnchor);cursor.removeSelectedText();
    QCOMPARE(widget.editor()->sourceText(),QString("a\nbc\nlocal"));widget.editor()->undo();
    QVERIFY(widget.isModified());QCOMPARE(widget.editor()->sourceText(),QString("a\nb\nc\nlocal"));
    QVERIFY(widget.save());QCOMPARE(get(path),baseline+"local");
}
void EditorTest::findReplacePreservesLiteralsAndUndo(){
    CodeEditor editor;editor.setPlainText("Alpha alpha alphabet\nitem1 item2");
    QString error;QVERIFY(editor.findText("alpha",false,true,true,false,&error));QCOMPARE(editor.textCursor().selectionStart(),6);
    QVERIFY(editor.replaceCurrent("alpha","C:\\tmp\\1",true,true,false,&error));QVERIFY(editor.sourceText().contains("C:\\tmp\\1"));editor.undo();
    QCOMPARE(editor.replaceAll("alpha","value",false,true,false,&error),2);QCOMPARE(editor.sourceText(),QString("value value alphabet\nitem1 item2"));editor.undo();
    QCOMPARE(editor.replaceAll("item([0-9])","number\\1",true,false,true,&error),2);QVERIFY(editor.sourceText().endsWith("number1 number2"));editor.undo();
    const auto before=editor.sourceText();QCOMPARE(editor.replaceAll("[","x",true,false,true,&error),-1);QVERIFY(!error.isEmpty());QCOMPARE(editor.sourceText(),before);
    editor.setPlainText("ab");QCOMPARE(editor.replaceAll("(?=.)","_",true,false,true,&error),2);QCOMPARE(editor.sourceText(),QString("_a_b"));editor.undo();QCOMPARE(editor.sourceText(),QString("ab"));
}
void EditorTest::gutterSyntaxAndIndentation(){
    CodeEditor editor;editor.resize(600,400);editor.setFileName("file.cpp");editor.setPlainText("int value = 12; // comment\n/* block\ncontinued */\n");
    QCOMPARE(editor.languageName(),QString("C++"));QTRY_VERIFY(!editor.document()->firstBlock().layout()->formats().isEmpty());
    QTRY_COMPARE(editor.document()->findBlockByNumber(1).userState(),1);QCOMPARE(editor.document()->findBlockByNumber(2).userState(),0);
    const int small=editor.lineNumberAreaWidth();editor.setPlainText(QString(100,'\n'));QVERIFY(editor.lineNumberAreaWidth()>small);
    auto *gutter=editor.findChild<QWidget *>("editorLineNumbers");QVERIFY(gutter);QCOMPARE(gutter->width(),editor.lineNumberAreaWidth());
    editor.setPlainText("    value");auto cursor=editor.textCursor();cursor.movePosition(QTextCursor::End);editor.setTextCursor(cursor);QTest::keyClick(&editor,Qt::Key_Return);
    QCOMPARE(editor.sourceText(),QString("    value\n    "));editor.undo();
    cursor=editor.textCursor();cursor.setPosition(0);editor.setTextCursor(cursor);QTest::keyClick(&editor,Qt::Key_Return);QCOMPARE(editor.sourceText(),QString("\n    value"));
    editor.setPlainText("a\nb");editor.selectAll();QTest::keyClick(&editor,Qt::Key_Tab);QCOMPARE(editor.sourceText(),QString("    a\n    b"));editor.undo();QCOMPARE(editor.sourceText(),QString("a\nb"));
}
void EditorTest::readOnlyLineNumbersAreUniform(){
    // Upstream transcript code blocks number every line uniformly. Only an editable editor
    // emphasizes the cursor line, matching its current-line band (absent when read-only).
    const auto darkest=[](const QImage &image,int top,int bottom){
        int best=255;
        for(int y=qMax(0,top);y<qMin(image.height(),bottom);++y)for(int x=0;x<image.width();++x)best=qMin(best,qGray(image.pixel(x,y)));
        return best;
    };
    for(const bool readOnly:{false,true}){
        CodeEditor editor;editor.resize(400,200);editor.setPlainText("one\ntwo\n");editor.setReadOnly(readOnly);
        auto cursor=editor.textCursor();cursor.movePosition(QTextCursor::End);editor.setTextCursor(cursor);
        auto *gutter=editor.findChild<QWidget *>("editorLineNumbers");QVERIFY(gutter);
        const QImage image=gutter->grab().toImage();const qreal ratio=image.devicePixelRatio();
        const auto band=[&](int block){const auto rect=editor.cursorRect(QTextCursor(editor.document()->findBlockByNumber(block)));return darkest(image,qRound(rect.top()*ratio),qRound((rect.bottom()+1)*ratio));};
        const int other=band(0),current=band(2);
        QVERIFY2(other<200&&current<200,qPrintable(QString("numbers not painted: %1 %2").arg(other).arg(current)));
        if(readOnly)QVERIFY2(qAbs(current-other)<=12,qPrintable(QString("read-only %1 vs %2").arg(current).arg(other)));
        else QVERIFY2(current+20<other,qPrintable(QString("editable %1 vs %2").arg(current).arg(other)));
    }
}
void EditorTest::watchesAtomicExternalReplacement(){
    QTemporaryDir directory;const auto path=directory.filePath("watched.txt");QVERIFY(put(path,"before"));
    EditorWidget widget;QVERIFY(widget.openFile(path));
    TextFile baseline;QVERIFY(TextFileIO::read(path,&baseline));QVERIFY(TextFileIO::write(baseline,"after",false));
    QTRY_COMPARE_WITH_TIMEOUT(widget.editor()->sourceText(),QString("after"),3000);
    append(widget.editor()," local");QVERIFY(put(path,"external next"));QTRY_VERIFY_WITH_TIMEOUT(widget.hasExternalChange(),3000);
    QCOMPARE(widget.editor()->sourceText(),QString("after local"));
}
void EditorTest::explicitThemeSurvivesTransparentPalette(){
    QTemporaryDir fixture;const QString text="const char *title = \"Dashboard\"; int size = 12; // comment";
    const auto path=fixture.filePath("theme.cpp");QVERIFY(put(path,text.toUtf8()));
    EditorWidget pane;QVERIFY(pane.openFile(path));auto *editor=pane.editor();
    editor->setStyleSheet("QPlainTextEdit { background: transparent; }");
    auto palette=editor->palette();palette.setColor(QPalette::Base,Qt::transparent);editor->setPalette(palette);
    auto check=[&](const QColor &keyword,const QColor &stringColor,const QColor &number,const QColor &comment){
        QCOMPARE(tokenFormat(editor,0).foreground().color(),keyword);
        QCOMPARE(tokenFormat(editor,text.indexOf("Dashboard")).foreground().color(),stringColor);
        QCOMPARE(tokenFormat(editor,text.indexOf("12")).foreground().color(),number);
        const auto commentStyle=tokenFormat(editor,text.indexOf("comment"));QCOMPARE(commentStyle.foreground().color(),comment);QVERIFY(commentStyle.fontItalic());
    };
    pane.setDarkTheme(false);check(QColor("#e14238"),QColor("#975ee6"),QColor("#d74089"),QColor("#505868"));
    pane.setDarkTheme(true);check(QColor("#76e2c6"),QColor("#ffadce"),QColor("#ffb885"),QColor("#8e94a1"));
    palette.setColor(QPalette::Base,Qt::white);editor->setPalette(palette);
    check(QColor("#76e2c6"),QColor("#ffadce"),QColor("#ffb885"),QColor("#8e94a1"));
    pane.setDarkTheme(false);check(QColor("#e14238"),QColor("#975ee6"),QColor("#d74089"),QColor("#505868"));
    QCOMPARE(editor->sourceText(),text);
}

QTEST_MAIN(EditorTest)
#include "test_editor.moc"
