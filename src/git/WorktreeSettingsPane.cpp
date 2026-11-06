#include "WorktreeSettingsPane.h"
#include "WorktreeSettingsController.h"
#include "editor/CodeEditor.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace ade {
namespace {
QLabel *label(const QString &text){auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;}
QString age(qint64 time){if(time<0)return "Usage unknown";const auto minutes=qMax<qint64>(1,(QDateTime::currentMSecsSinceEpoch()-time)/60000);if(minutes<60)return QString("%1m ago").arg(minutes);if(minutes<1440)return QString("%1h ago").arg(minutes/60);return QString("%1d ago").arg(minutes/1440);}
}
WorktreeSettingsPane::WorktreeSettingsPane(WorktreeSettingsController *controller,QWidget *parent):QWidget(parent),m_controller(controller){
    setObjectName("worktreeSettingsPane");auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(12);
    auto *heading=label("Worktree setup");auto font=heading->font();font.setBold(true);font.setPointSizeF(font.pointSizeF()>0?font.pointSizeF()+3:13);heading->setFont(font);layout->addWidget(heading);
    layout->addWidget(label("Set commands to run after a new Git worktree is created and connected. Scripts are stored in each repository's main worktree."));
    auto *row=new QHBoxLayout;m_repositories=new QComboBox;m_repositories->setObjectName("worktreeRepository");m_repositories->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_repositories->setMinimumContentsLength(16);row->addWidget(m_repositories,1);
    m_refresh=new QPushButton("Refresh");m_refresh->setObjectName("refreshWorktreeSettings");row->addWidget(m_refresh);layout->addLayout(row);
    m_variants=new QTabBar;m_variants->setObjectName("worktreeScriptVariant");m_variants->addTab("Shell (.sh)");m_variants->addTab("PowerShell (.ps1)");m_variants->setExpanding(false);layout->addWidget(m_variants);
    m_path=label({});m_path->setObjectName("worktreeScriptPath");m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_path);
    m_note=label({});m_note->setObjectName("worktreeCodexNotice");layout->addWidget(m_note);
    m_editor=new CodeEditor;m_editor->setObjectName("worktreeSetupEditor");m_editor->setMinimumHeight(240);layout->addWidget(m_editor);
    auto *actions=new QHBoxLayout;actions->addStretch();m_reset=new QPushButton("Reset");m_reset->setObjectName("resetWorktreeScript");actions->addWidget(m_reset);m_save=new QPushButton("Save");m_save->setObjectName("saveWorktreeScript");actions->addWidget(m_save);layout->addLayout(actions);
    m_message=label({});m_message->setObjectName("worktreeSettingsMessage");layout->addWidget(m_message);
    layout->addWidget(label("Available environment variables"));
    for(const auto &entry:QList<QPair<QString,QString>>{{"STAGEWISE_SOURCE_WORKTREE_PATH","The workspace from which the new worktree was created."},{"STAGEWISE_TARGET_WORKTREE_PATH","The newly created worktree; also the script's working directory."},{"STAGEWISE_MAIN_WORKTREE_PATH","The repository's main worktree."}}){
        auto *variableRow=new QHBoxLayout;auto *name=label(entry.first+"\n"+entry.second);name->setTextInteractionFlags(Qt::TextSelectableByMouse);variableRow->addWidget(name,1);
        auto *copy=new QToolButton;copy->setText("Copy");copy->setObjectName("copy"+entry.first);copy->setAccessibleName("Copy "+entry.first);variableRow->addWidget(copy);layout->addLayout(variableRow);
        connect(copy,&QToolButton::clicked,this,[name=entry.first]{QApplication::clipboard()->setText(name);});
    }
    layout->addWidget(label("Managed worktrees"));m_search=new QLineEdit;m_search->setObjectName("worktreeSettingsSearch");m_search->setPlaceholderText("Search worktrees");layout->addWidget(m_search);
    m_worktrees=new QTreeWidget;m_worktrees->setObjectName("settingsManagedWorktrees");m_worktrees->setHeaderLabels({"Worktree","Branch","Last used","Status"});m_worktrees->setRootIsDecorated(false);m_worktrees->setMinimumHeight(160);m_worktrees->header()->setMinimumSectionSize(24);m_worktrees->header()->setSectionResizeMode(3,QHeaderView::Stretch);m_worktrees->viewport()->installEventFilter(this);layout->addWidget(m_worktrees);
    m_remove=new QPushButton("Delete worktree…");m_remove->setObjectName("settingsDeleteWorktree");layout->addWidget(m_remove,0,Qt::AlignRight);
    connect(m_repositories,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int i){if(!m_syncing&&m_controller)m_controller->selectRepository(m_repositories->itemData(i).toString());});
    connect(m_variants,&QTabBar::currentChanged,this,[this](int i){if(!m_syncing&&m_controller)m_controller->selectVariant(i==1?GitSetupVariant::PowerShell:GitSetupVariant::Posix);});
    connect(m_editor,&QPlainTextEdit::textChanged,this,[this]{if(!m_syncing&&m_controller)m_controller->setDraft(m_controller->selectedRepository(),m_controller->selectedVariant(),m_editor->sourceText());});
    auto save=[this]{if(m_controller)m_controller->save(m_controller->selectedRepository(),m_controller->selectedVariant());};connect(m_save,&QPushButton::clicked,this,save);connect(m_editor,&CodeEditor::saveRequested,this,save);
    auto *shortcut=new QShortcut(QKeySequence::Save,this);shortcut->setContext(Qt::WidgetWithChildrenShortcut);connect(shortcut,&QShortcut::activated,this,save);
    connect(m_reset,&QPushButton::clicked,this,[this]{if(m_controller&&QMessageBox::question(this,"Reset setup script","Discard this draft and restore the latest loaded script?",QMessageBox::Yes|QMessageBox::No,QMessageBox::No)==QMessageBox::Yes)m_controller->resetDraft(m_controller->selectedRepository(),m_controller->selectedVariant());});
    connect(m_refresh,&QPushButton::clicked,this,[this]{if(m_controller)m_controller->refresh();});connect(m_search,&QLineEdit::textChanged,this,&WorktreeSettingsPane::filterWorktrees);
    connect(m_worktrees,&QTreeWidget::itemSelectionChanged,this,&WorktreeSettingsPane::updateRemoval);connect(m_remove,&QPushButton::clicked,this,&WorktreeSettingsPane::removeSelected);
    if(controller){connect(controller,&WorktreeSettingsController::changed,this,&WorktreeSettingsPane::synchronize);connect(controller,&QObject::destroyed,this,[this]{m_controller=nullptr;synchronize();});controller->refresh();}synchronize();
}
void WorktreeSettingsPane::setDarkTheme(bool dark){m_editor->setDarkTheme(dark);}
bool WorktreeSettingsPane::hasUnsavedDrafts() const{return m_controller&&m_controller->hasUnsavedDrafts();}
bool WorktreeSettingsPane::requestClose(CloseChoice choice){
    if(!m_controller)return true;
    if(m_controller->isMutating()){m_message->setText("Wait for the workspace operation to finish.");return false;}
    if(!hasUnsavedDrafts())return true;
    if(choice==CloseChoice::Cancel)return false;
    if(choice==CloseChoice::Ask){const auto answer=QMessageBox::warning(this,"Unsaved setup scripts","Discard unsaved setup-script drafts?",QMessageBox::Discard|QMessageBox::Cancel,QMessageBox::Cancel);if(answer!=QMessageBox::Discard)return false;}
    m_controller->discardDrafts();return true;
}
void WorktreeSettingsPane::synchronize(){
    m_syncing=true;const QSignalBlocker repos(m_repositories),variants(m_variants),editor(m_editor),trees(m_worktrees);
    const auto selectedPath=m_worktrees->currentItem()?m_worktrees->currentItem()->data(0,Qt::UserRole).toString():QString();
    if(!m_controller){m_repositories->setEnabled(false);m_variants->setEnabled(false);m_editor->setReadOnly(true);m_save->setEnabled(false);m_reset->setEnabled(false);m_refresh->setEnabled(false);m_remove->setEnabled(false);m_message->setText("Git settings service is unavailable.");m_syncing=false;return;}
    const auto id=m_controller->selectedRepository();const auto v=m_controller->selectedVariant();const auto r=m_controller->repository(id);const auto d=m_controller->draft(id,v);
    const bool metadataChanged=m_renderedRevision!=m_controller->repositoryRevision();
    const bool repositoryChanged=m_renderedRepository!=id;
    if(metadataChanged){
        m_repositories->clear();
        for(const auto &repositoryId:m_controller->repositoryIds()){const auto repository=m_controller->repository(repositoryId);const bool available=m_controller->repositoryAvailable(repositoryId);m_repositories->addItem(QString("%1 (%2)%3").arg(repository.name).arg(repository.worktrees.size()).arg(available?"":" — unavailable"),repositoryId);}
    }
    m_repositories->setCurrentIndex(m_repositories->findData(id));m_variants->setCurrentIndex(v==GitSetupVariant::PowerShell?1:0);m_repositories->setEnabled(!m_controller->repositoryIds().isEmpty());m_variants->setEnabled(!id.isEmpty());
    m_path->setText(d.path);m_note->setText(r.hasCodexSetupScript?"If no platform-specific .stagewise script exists, setup falls back to .codex/environments/environment.toml.":"The script runs only after a newly created worktree is connected successfully.");m_note->setVisible(!id.isEmpty());
    if(m_editor->sourceText()!=d.text)m_editor->setPlainText(d.text);
    if(m_renderedVariant!=int(v))m_editor->setFileName(v==GitSetupVariant::PowerShell?"setup.ps1":"setup.sh");
    m_editor->setReadOnly(id.isEmpty()||!m_controller->repositoryAvailable(id)||!d.error.isEmpty());m_save->setEnabled(m_controller->canSave(id,v));m_reset->setEnabled((d.dirty()||d.conflict)&&!m_controller->isMutating());m_refresh->setEnabled(!m_controller->isLoading());
    QString message=m_controller->message();if(!d.error.isEmpty())message=d.error;else if(d.conflict)message="This script changed outside ADE. Your draft is preserved. Refresh, then Reset to load the external version.";else if(id.isEmpty())message=m_controller->isLoading()?"Loading repositories…":"Connect a Git repository to configure its worktree setup.";
    m_message->setText(message);m_message->setVisible(!message.isEmpty());
    if(metadataChanged||repositoryChanged){
        m_worktrees->clear();
        for(const auto &item:r.worktrees){auto *row=new QTreeWidgetItem(m_worktrees,{QFileInfo(item.worktree.path).fileName(),item.worktree.branch,age(item.lastUsedAt),item.removable?"Clean":item.disabledReason});row->setData(0,Qt::UserRole,item.worktree.path);row->setToolTip(0,item.worktree.path);for(int column=1;column<4;++column)row->setToolTip(column,row->text(column));if(!repositoryChanged&&GitService::pathKey(selectedPath)==GitService::pathKey(item.worktree.path))m_worktrees->setCurrentItem(row);}
        sizeColumns();
    }
    m_renderedRevision=m_controller->repositoryRevision();m_renderedRepository=id;m_renderedVariant=int(v);
    m_syncing=false;filterWorktrees();updateRemoval();
}
void WorktreeSettingsPane::filterWorktrees(){const auto query=m_search->text().trimmed();for(int i=0;i<m_worktrees->topLevelItemCount();++i){auto *item=m_worktrees->topLevelItem(i);item->setHidden(!query.isEmpty()&&!(item->text(0)+' '+item->text(1)+' '+item->data(0,Qt::UserRole).toString()).contains(query,Qt::CaseInsensitive));}updateRemoval();}
void WorktreeSettingsPane::updateRemoval(){const auto *item=m_worktrees->currentItem();m_remove->setEnabled(m_controller&&item&&!item->isHidden()&&m_controller->canRemove(m_controller->selectedRepository(),item->data(0,Qt::UserRole).toString()));}
bool WorktreeSettingsPane::eventFilter(QObject *watched,QEvent *event){
    if(watched==m_worktrees->viewport()&&event->type()==QEvent::Resize)sizeColumns();
    return QWidget::eventFilter(watched,event);
}
void WorktreeSettingsPane::sizeColumns(){
    // Fit short values while reserving at least 40% for the status explanation.
    // Longer names remain available via tooltips instead of forcing wide minima.
    const int cap=qMax(24,m_worktrees->viewport()->width()/5);
    for(int column=0;column<3;++column){
        int wanted=m_worktrees->fontMetrics().horizontalAdvance(m_worktrees->headerItem()->text(column))+20;
        for(int row=0;row<m_worktrees->topLevelItemCount();++row)wanted=qMax(wanted,m_worktrees->fontMetrics().horizontalAdvance(m_worktrees->topLevelItem(row)->text(column))+20);
        m_worktrees->setColumnWidth(column,qMin(cap,wanted));
    }
}
void WorktreeSettingsPane::removeSelected(){
    auto *item=m_worktrees->currentItem();if(!m_controller||!item)return;const auto path=item->data(0,Qt::UserRole).toString(),id=m_controller->selectedRepository();if(!m_controller->canRemove(id,path))return;
    QMessageBox box(QMessageBox::Warning,"Delete managed worktree","Delete this clean, unmounted worktree? Its branch will be retained.\n\n"+path,QMessageBox::Yes|QMessageBox::Cancel,this);box.setObjectName("settingsWorktreeDeleteConfirmation");box.setDefaultButton(QMessageBox::Cancel);box.setTextFormat(Qt::PlainText);
    if(box.exec()==QMessageBox::Yes&&m_controller)m_controller->remove(id,path);
}
}
