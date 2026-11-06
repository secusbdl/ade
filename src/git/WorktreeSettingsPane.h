#pragma once
#include <QWidget>
#include <QPointer>
#include <limits>
class QComboBox;class QTabBar;class QLabel;class QPushButton;class QLineEdit;class QTreeWidget;
namespace ade {
class CodeEditor;class WorktreeSettingsController;
// The host owns the controller beyond the lifetime of rebuilt settings views.
class WorktreeSettingsPane final : public QWidget {
    Q_OBJECT
public:
    enum class CloseChoice { Ask, Discard, Cancel };
    explicit WorktreeSettingsPane(WorktreeSettingsController *controller,QWidget *parent=nullptr);
    bool hasUnsavedDrafts() const;
    bool requestClose(CloseChoice choice=CloseChoice::Ask);
    void setDarkTheme(bool dark);
    WorktreeSettingsController *controller() const { return m_controller; }
protected:
    bool eventFilter(QObject *watched,QEvent *event) override;
private:
    QPointer<WorktreeSettingsController> m_controller;
    QComboBox *m_repositories;QTabBar *m_variants;QLabel *m_path,*m_note,*m_message;
    CodeEditor *m_editor;QPushButton *m_save,*m_reset,*m_refresh,*m_remove;
    QLineEdit *m_search;QTreeWidget *m_worktrees;
    bool m_syncing=false;
    quint64 m_renderedRevision=std::numeric_limits<quint64>::max();
    QString m_renderedRepository;
    int m_renderedVariant=-1;
    void synchronize();void filterWorktrees();void updateRemoval();void removeSelected();void sizeColumns();
};
}
