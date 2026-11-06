#include "EditorWidget.h"
#include "CodeEditor.h"

#include <QCheckBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTimer>
#include <QVBoxLayout>

namespace ade {
namespace {
QPushButton *button(const QString &text,const char *name,QHBoxLayout *layout){
    auto *result=new QPushButton(text);result->setObjectName(name);result->setCursor(Qt::PointingHandCursor);layout->addWidget(result);return result;
}
QByteArray textDigest(const QString &text){
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char *>(text.constData()),text.size()*sizeof(QChar)),QCryptographicHash::Sha256);
}
QByteArray packEndings(const QVector<LineEnding> &endings){
    return qCompress(reinterpret_cast<const uchar *>(endings.constData()),endings.size(),1);
}
QVector<LineEnding> unpackEndings(const QByteArray &packed){
    const auto bytes=qUncompress(packed);QVector<LineEnding> endings;endings.reserve(bytes.size());
    for(const auto value:bytes)endings.append(static_cast<LineEnding>(value));
    return endings;
}
}

EditorWidget::EditorWidget(QWidget *parent):QWidget(parent){
    setObjectName("nativeEditorPane");
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto *toolbar=new QHBoxLayout;toolbar->setContentsMargins(12,6,10,6);toolbar->setSpacing(5);
    m_title=new QLabel; m_title->setTextFormat(Qt::PlainText);toolbar->addWidget(m_title,1);
    auto *undo=button("Undo","editorUndo",toolbar);auto *redo=button("Redo","editorRedo",toolbar);
    auto *findButton=button("Find","editorFind",toolbar);m_save=button("Save","editorSave",toolbar);layout->addLayout(toolbar);
    m_conflictBar=new QWidget;auto *conflictRow=new QHBoxLayout(m_conflictBar);conflictRow->setContentsMargins(12,8,12,8);
    m_conflictText=new QLabel;m_conflictText->setTextFormat(Qt::PlainText);m_conflictText->setWordWrap(true);conflictRow->addWidget(m_conflictText,1);
    auto *reload=button("Reload","editorReload",conflictRow);m_overwrite=button("Overwrite","editorOverwrite",conflictRow);
    layout->addWidget(m_conflictBar);m_conflictBar->hide();
    m_findBar=new QWidget;auto *findLayout=new QVBoxLayout(m_findBar);findLayout->setContentsMargins(10,6,10,6);findLayout->setSpacing(4);
    auto *findRow=new QHBoxLayout;m_find=new QLineEdit;m_find->setObjectName("editorFindText");m_find->setPlaceholderText("Find");findRow->addWidget(m_find,1);
    m_case=new QCheckBox("Aa");m_case->setToolTip("Match case");m_words=new QCheckBox("Word");m_regex=new QCheckBox(".*");m_regex->setToolTip("Regular expression; replacement captures use \\1");
    findRow->addWidget(m_case);findRow->addWidget(m_words);findRow->addWidget(m_regex);
    auto *previous=button("Previous","editorFindPrevious",findRow);auto *next=button("Next","editorFindNext",findRow);auto *closeFind=button("×","editorCloseFind",findRow);closeFind->setAccessibleName("Close find and replace");findLayout->addLayout(findRow);
    auto *replaceRow=new QHBoxLayout;m_replace=new QLineEdit;m_replace->setObjectName("editorReplaceText");m_replace->setPlaceholderText("Replace");replaceRow->addWidget(m_replace,1);
    auto *replace=button("Replace","editorReplace",replaceRow);auto *replaceAll=button("Replace all","editorReplaceAll",replaceRow);findLayout->addLayout(replaceRow);layout->addWidget(m_findBar);m_findBar->hide();
    m_editor=new CodeEditor;layout->addWidget(m_editor,1);
    m_message=new QLabel;m_message->setTextFormat(Qt::PlainText);m_message->setWordWrap(true);m_message->setContentsMargins(10,4,10,4);layout->addWidget(m_message);m_message->hide();
    m_status=new QLabel;m_status->setObjectName("editorStatus");m_status->setContentsMargins(12,5,12,5);layout->addWidget(m_status);
    m_watcher=new QFileSystemWatcher(this);m_watchTimer=new QTimer(this);m_watchTimer->setSingleShot(true);m_watchTimer->setInterval(80);
    connect(m_watcher,&QFileSystemWatcher::fileChanged,m_watchTimer,qOverload<>(&QTimer::start));
    connect(m_watcher,&QFileSystemWatcher::directoryChanged,m_watchTimer,qOverload<>(&QTimer::start));
    connect(m_watchTimer,&QTimer::timeout,this,&EditorWidget::checkExternalChanges);
    connect(m_editor,&QPlainTextEdit::textChanged,this,&EditorWidget::contentChanged);
    connect(m_editor,&QPlainTextEdit::cursorPositionChanged,this,&EditorWidget::updateStatus);
    connect(m_editor,&CodeEditor::zoomChanged,this,&EditorWidget::updateStatus);
    connect(m_editor,&CodeEditor::saveRequested,this,[this]{save();});
    connect(m_editor,&CodeEditor::findRequested,this,&EditorWidget::showFind);
    connect(undo,&QPushButton::clicked,m_editor,&QPlainTextEdit::undo);connect(redo,&QPushButton::clicked,m_editor,&QPlainTextEdit::redo);
    connect(m_editor,&QPlainTextEdit::undoAvailable,undo,&QWidget::setEnabled);connect(m_editor,&QPlainTextEdit::redoAvailable,redo,&QWidget::setEnabled);undo->setEnabled(false);redo->setEnabled(false);
    connect(findButton,&QPushButton::clicked,this,[this]{showFind(false);});connect(m_save,&QPushButton::clicked,this,[this]{save();});
    connect(next,&QPushButton::clicked,this,[this]{find(false);});connect(previous,&QPushButton::clicked,this,[this]{find(true);});connect(m_find,&QLineEdit::returnPressed,this,[this]{find(false);});
    connect(closeFind,&QPushButton::clicked,this,[this]{m_findBar->hide();m_editor->setFocus();});
    connect(replace,&QPushButton::clicked,this,[this]{QString error;m_editor->replaceCurrent(m_find->text(),m_replace->text(),m_case->isChecked(),m_words->isChecked(),m_regex->isChecked(),&error);if(!error.isEmpty())showError(error);});
    connect(replaceAll,&QPushButton::clicked,this,[this]{QString error;const auto count=m_editor->replaceAll(m_find->text(),m_replace->text(),m_case->isChecked(),m_words->isChecked(),m_regex->isChecked(),&error);if(!error.isEmpty())showError(error);else{m_message->setText(QString("Replaced %1 occurrence(s).").arg(count));m_message->show();}});
    connect(reload,&QPushButton::clicked,this,[this]{
        if(m_modified&&QMessageBox::question(this,"Reload file","Discard your unsaved edits and load the current file from disk?")!=QMessageBox::Yes)return;
        reloadFromDisk(nullptr,true);
    });
    connect(m_overwrite,&QPushButton::clicked,this,[this]{
        if(QMessageBox::question(this,"Overwrite file","Save your editor content and replace the current file on disk?")!=QMessageBox::Yes)return;
        save(nullptr,true);
    });
    updateStatus();
}

void EditorWidget::setDarkTheme(bool dark){m_editor->setDarkTheme(dark);}

bool EditorWidget::openFile(const QString &path,QString *error){
    if(m_modified){const auto message=QString("Save or discard the open document before opening another file.");if(error)*error=message;showError(message);return false;}
    TextFile file;QString failure;
    if(!TextFileIO::read(path,&file,&failure)){if(error)*error=failure;showError(failure);return false;}
    if(error)error->clear();
    adopt(file);return true;
}
void EditorWidget::adopt(const TextFile &file,bool keepPosition){
    const auto position=m_editor->textCursor().position();const auto scroll=m_editor->verticalScrollBar()->value();
    m_loading=true;m_baseline=file;m_previousText=file.text;m_endings=file.lineEndings;
    m_delimiterHistory.clear();m_delimiterState=packEndings(m_endings);m_delimiterHistory.insert(textDigest(file.text),m_delimiterState);
    m_editor->setPlainText(file.text);m_editor->setFileName(file.path);m_editor->document()->setModified(false);
    if(keepPosition){auto cursor=m_editor->textCursor();cursor.setPosition(qMin(position,m_editor->document()->characterCount()-1));m_editor->setTextCursor(cursor);m_editor->verticalScrollBar()->setValue(scroll);}
    m_loading=false;
    if(m_modified){m_modified=false;emit modifiedChanged(false);}
    setConflict(false);m_message->hide();watchFile();updateStatus();
}
void EditorWidget::contentChanged(){
    if(m_loading)return;
    const auto text=m_editor->sourceText();
    const auto digest=textDigest(text);
    const auto history=m_delimiterHistory.constFind(digest);
    if(history!=m_delimiterHistory.cend()){
        m_delimiterState=history.value();m_endings=unpackEndings(m_delimiterState);
    }else{
        const auto endings=TextFileIO::reconcileEndings(m_previousText,text,m_endings,m_baseline.defaultLineEnding);
        if(endings!=m_endings)m_delimiterState=packEndings(endings);
        m_endings=endings;m_delimiterHistory.insert(digest,m_delimiterState);
    }
    m_previousText=text;
    if(text==m_baseline.text)m_endings=m_baseline.lineEndings;
    const bool modified=text!=m_baseline.text;
    if(modified!=m_modified){m_modified=modified;emit modifiedChanged(modified);}
    updateStatus();
}
bool EditorWidget::save(QString *error,bool force){
    if(error)error->clear();
    QString failure;
    if(m_editor->isReadOnly())failure="This editor is read-only.";
    else if(m_conflict&&!force){failure="Resolve the external file change using Reload or Overwrite before saving.";}
    QByteArray bytes;
    if(failure.isEmpty()&&!TextFileIO::encode(m_baseline,m_editor->sourceText(),m_endings,&bytes,&failure)){}
    if(failure.isEmpty()&&!TextFileIO::write(m_baseline,bytes,force,&failure)){}
    if(!failure.isEmpty()){
        if(error)*error=failure;
        showError(failure);checkExternalChanges();return false;
    }
    // Preserve undo history and the exact editor text while recording the
    // committed bytes as the new optimistic-concurrency baseline.
    m_baseline.originalBytes=bytes;m_baseline.text=m_editor->sourceText();m_baseline.lineEndings=m_endings;m_previousText=m_baseline.text;
    m_editor->document()->setModified(false);
    if(m_modified){m_modified=false;emit modifiedChanged(false);}
    setConflict(false);m_message->hide();watchFile();updateStatus();emit fileSaved(filePath());return true;
}
bool EditorWidget::reloadFromDisk(QString *error,bool discard){
    if(m_modified&&!discard){if(error)*error="Reload would discard unsaved changes.";return false;}
    TextFile fresh;QString failure;
    if(!TextFileIO::read(filePath(),&fresh,&failure)){if(error)*error=failure;showError(failure);return false;}
    if(error)error->clear();
    adopt(fresh,true);return true;
}
void EditorWidget::checkExternalChanges(){
    if(filePath().isEmpty()||m_loading)return;
    watchFile();TextFile fresh;QString error;
    if(!TextFileIO::read(filePath(),&fresh,&error)){setConflict(true,error);return;}
    if(fresh.path!=m_baseline.path){setConflict(true,"The file path now resolves to a different target. Reopen it before saving.");return;}
    if(fresh.originalBytes==m_baseline.originalBytes){setConflict(false);return;}
    if(m_modified){setConflict(true,"This file changed on disk while you were editing it. Reload or explicitly overwrite the external changes.");return;}
    adopt(fresh,true);
}
void EditorWidget::watchFile(){
    if(!m_watcher->files().isEmpty())m_watcher->removePaths(m_watcher->files());
    if(!m_watcher->directories().isEmpty())m_watcher->removePaths(m_watcher->directories());
    if(filePath().isEmpty())return;
    const QFileInfo info(filePath());if(info.exists())m_watcher->addPath(filePath());
    if(info.absoluteDir().exists())m_watcher->addPath(info.absolutePath());
}
void EditorWidget::setConflict(bool value,const QString &message){
    if(m_conflict!=value){m_conflict=value;emit externalChangeChanged(value);}
    m_conflictText->setText(message);m_conflictBar->setVisible(value);
    m_overwrite->setText(QFileInfo::exists(filePath())?"Overwrite":"Re-create");updateStatus();
}
void EditorWidget::showError(const QString &message){m_message->setText(message);m_message->show();emit errorOccurred(message);}
void EditorWidget::updateStatus(){
    if(!m_editor)return;
    m_title->setText(QFileInfo(filePath()).fileName()+(m_modified?" •":""));m_title->setToolTip(filePath());
    const auto cursor=m_editor->textCursor();
    m_status->setText(QString("Ln %1, Col %2    %3    %4    %5    %6%")
        .arg(cursor.blockNumber()+1).arg(cursor.positionInBlock()+1).arg(m_baseline.encodingName(),m_baseline.lineEndingName(),m_editor->languageName()).arg(m_editor->editorZoom()));
    m_save->setEnabled(!filePath().isEmpty()&&m_modified&&!m_conflict);
}
void EditorWidget::showFind(bool replace){
    m_findBar->show();if(m_editor->textCursor().hasSelection()){auto selection=m_editor->textCursor().selectedText();if(!selection.contains(QChar(0x2029)))m_find->setText(selection);}
    if(replace)m_replace->setFocus();else{m_find->setFocus();m_find->selectAll();}
}
void EditorWidget::find(bool backwards){QString error;const bool found=m_editor->findText(m_find->text(),backwards,m_case->isChecked(),m_words->isChecked(),m_regex->isChecked(),&error);
    if(!error.isEmpty())showError(error);else if(!found){m_message->setText("No matches.");m_message->show();}else m_message->hide();}
bool EditorWidget::requestClose(CloseChoice choice){
    if(!m_modified)return true;
    if(choice==CloseChoice::Ask){
        const auto answer=QMessageBox::warning(this,"Unsaved changes","Save changes to "+QFileInfo(filePath()).fileName()+"?",QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Save);
        choice=answer==QMessageBox::Save?CloseChoice::Save:answer==QMessageBox::Discard?CloseChoice::Discard:CloseChoice::Cancel;
    }
    if(choice==CloseChoice::Save)return save();
    return choice==CloseChoice::Discard;
}

} // namespace ade
