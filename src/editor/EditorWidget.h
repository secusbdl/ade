#pragma once

#include "TextFile.h"
#include <QHash>
#include <QWidget>

class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace ade {
class CodeEditor;

class EditorWidget : public QWidget {
    Q_OBJECT
public:
    enum class CloseChoice { Ask, Save, Discard, Cancel };
    explicit EditorWidget(QWidget *parent=nullptr);
    bool openFile(const QString &path,QString *error=nullptr);
    bool save(QString *error=nullptr,bool force=false);
    QString filePath() const { return m_baseline.path; }
    bool isModified() const { return m_modified; }
    bool hasExternalChange() const { return m_conflict; }
    CodeEditor *editor() const { return m_editor; }
    bool requestClose(CloseChoice choice=CloseChoice::Ask);
    bool reloadFromDisk(QString *error=nullptr,bool discardEdits=false);
    void checkExternalChanges();
    void setDarkTheme(bool dark);
signals:
    void modifiedChanged(bool modified);
    void fileSaved(const QString &path);
    void externalChangeChanged(bool conflict);
    void errorOccurred(const QString &message);
private:
    void adopt(const TextFile &file,bool keepPosition=false);
    void contentChanged();
    void updateStatus();
    void setConflict(bool conflict,const QString &message={});
    void showError(const QString &message);
    void watchFile();
    void showFind(bool replace);
    void find(bool backwards);
    TextFile m_baseline;
    QString m_previousText;
    QVector<LineEnding> m_endings;
    // Text states reached by Qt undo/redo retain their delimiter metadata too.
    // Compressed payloads are implicitly shared for ordinary character edits.
    QHash<QByteArray,QByteArray> m_delimiterHistory;
    QByteArray m_delimiterState;
    bool m_loading=false;
    bool m_modified=false;
    bool m_conflict=false;
    CodeEditor *m_editor=nullptr;
    QLabel *m_title=nullptr;
    QLabel *m_status=nullptr;
    QLabel *m_message=nullptr;
    QPushButton *m_save=nullptr;
    QWidget *m_conflictBar=nullptr;
    QLabel *m_conflictText=nullptr;
    QPushButton *m_overwrite=nullptr;
    QWidget *m_findBar=nullptr;
    QLineEdit *m_find=nullptr;
    QLineEdit *m_replace=nullptr;
    QCheckBox *m_case=nullptr;
    QCheckBox *m_words=nullptr;
    QCheckBox *m_regex=nullptr;
    QFileSystemWatcher *m_watcher=nullptr;
    QTimer *m_watchTimer=nullptr;
};

} // namespace ade
