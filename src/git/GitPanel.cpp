#include "GitPanel.h"
#include "editor/CodeEditor.h"
#include "editor/TextFile.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

static void initializeGitResources() { static const bool ready=[] { Q_INIT_RESOURCE(git_ui); return true; }(); Q_UNUSED(ready) }
namespace ade {
namespace {
QString suggestedName(const GitBranchList &branches,const QList<GitWorktree> &trees) {
    initializeGitResources(); QFile file(":/ade/git/worktree-words.json");
    if(!file.open(QIODevice::ReadOnly)) return {};
    const auto data=QJsonDocument::fromJson(file.readAll()).object(); const auto adjectives=data["adjectives"].toArray(), nouns=data["nouns"].toArray();
    if(adjectives.isEmpty() || nouns.isEmpty()) return {};
    QSet<QString> reserved;
    for(const auto &branch:branches.branches) reserved.insert(branch.name.toCaseFolded());
    for(const auto &tree:trees) { reserved.insert(QFileInfo(tree.path).fileName().toCaseFolded()); if(!tree.branch.isEmpty()) reserved.insert(tree.branch.toCaseFolded()); }
    auto available=[&](const QString &adjective,const QString &noun) { const auto name=adjective+'-'+noun; return reserved.contains(name.toCaseFolded())?QString():name; };
    for(int i=0;i<20;++i) {
        const auto name=available(adjectives[int(QRandomGenerator::global()->bounded(quint32(adjectives.size())))].toString(),nouns[int(QRandomGenerator::global()->bounded(quint32(nouns.size())))].toString());
        if(!name.isEmpty()) return name;
    }
    for(const auto &adjective:adjectives) for(const auto &noun:nouns) { const auto name=available(adjective.toString(),noun.toString()); if(!name.isEmpty()) return name; }
    return {};
}
QPushButton *button(const QString &label,const QString &name,QWidget *parent=nullptr) {
    auto *result=new QPushButton(label,parent); result->setObjectName(name); result->setCursor(Qt::PointingHandCursor); return result;
}
QString branchLabel(const GitWorktree &tree) { return tree.branch.isEmpty()?(tree.headSha.isEmpty()?QStringLiteral("Detached"):"Detached · "+tree.headSha.left(7)):tree.branch; }
}
struct GitPanel::Private {
    struct Request { quint64 generation=0, preview=0; GitOperation operation=GitOperation::Inspect; QString path; };
    GitService *service=nullptr; GitWorkspaceContext context; QString workspace,contextId;
    QMap<quint64,Request> requests; quint64 generation=0, previewGeneration=0, fetchRequest=0; bool mutating=false, ready=false, selectedExists=false,topologyRefreshPending=false;
    GitSnapshot snapshot; GitBranchList branches; QList<GitWorktree> worktrees; GitDiffSummary diff;
    QString selectedPath, selectedOldPath; bool selectedStaged=false;
    std::function<bool(const QString &,GitOperation)> guard;
    QLabel *heading=nullptr,*summary=nullptr,*message=nullptr,*previewTitle=nullptr;
    QPushButton *refresh=nullptr,*fetch=nullptr,*switchBranch=nullptr,*createBranch=nullptr,*createWorktree=nullptr,*mount=nullptr,*remove=nullptr,*openFile=nullptr;
    QTreeWidget *changes=nullptr,*trees=nullptr; QComboBox *version=nullptr; QTabWidget *tabs=nullptr;
    CodeEditor *original=nullptr,*modified=nullptr; QWidget *preview=nullptr; QTimer *poll=nullptr;
    quint64 track(quint64 id,GitOperation operation,const QString &path={}) {
        requests.insert(id,{generation,previewGeneration,operation,path}); return id;
    }
};
GitPanel::GitPanel(GitServiceOptions options,QWidget *parent):QWidget(parent),d(std::make_unique<Private>()) {
    qRegisterMetaType<WorktreeSetupMetadata>(); qRegisterMetaType<GitOperation>();
    setObjectName("gitPanel"); setMinimumWidth(320);
    d->service=new GitService(std::move(options),this);
    auto *layout=new QVBoxLayout(this); layout->setContentsMargins(16,14,16,14); layout->setSpacing(10);
    auto *top=new QHBoxLayout; d->heading=new QLabel("Source control"); d->heading->setObjectName("gitHeading"); auto titleFont=d->heading->font(); titleFont.setWeight(QFont::DemiBold); if(titleFont.pixelSize()>0) titleFont.setPixelSize(titleFont.pixelSize()+1); else titleFont.setPointSizeF(qMax(8.0,titleFont.pointSizeF())+1); d->heading->setFont(titleFont);
    d->refresh=button("Refresh","gitRefresh"); top->addWidget(d->heading,1); top->addWidget(d->refresh); layout->addLayout(top);
    d->summary=new QLabel("Connect a repository to view changes and worktrees."); d->summary->setObjectName("gitSummary"); d->summary->setWordWrap(true); d->summary->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(d->summary);
    auto *actions=new QHBoxLayout; actions->setSpacing(6);
    d->switchBranch=button("Switch branch","gitSwitchBranch"); d->createBranch=button("New branch","gitCreateBranch"); d->createWorktree=button("New worktree","gitCreateWorktree"); d->fetch=button("Fetch","gitFetch");
    actions->addWidget(d->switchBranch); actions->addWidget(d->createBranch); actions->addWidget(d->createWorktree); actions->addWidget(d->fetch); actions->addStretch(); layout->addLayout(actions);
    d->message=new QLabel; d->message->setObjectName("gitMessage"); d->message->setWordWrap(true); d->message->hide(); layout->addWidget(d->message);
    d->tabs=new QTabWidget; d->tabs->setObjectName("gitTabs"); layout->addWidget(d->tabs,1);
    auto *changesPage=new QWidget; auto *changesLayout=new QVBoxLayout(changesPage); changesLayout->setContentsMargins(0,8,0,0);
    auto *vertical=new QSplitter(Qt::Vertical); changesLayout->addWidget(vertical);
    d->changes=new QTreeWidget; d->changes->setObjectName("gitChanges"); d->changes->setHeaderLabels({"File","State","+","−"}); d->changes->setRootIsDecorated(false); d->changes->setAlternatingRowColors(false); d->changes->setUniformRowHeights(true);
    d->changes->header()->setSectionResizeMode(0,QHeaderView::Stretch); for(int i=1;i<4;++i) d->changes->header()->setSectionResizeMode(i,QHeaderView::ResizeToContents); vertical->addWidget(d->changes);
    d->preview=new QWidget; auto *previewLayout=new QVBoxLayout(d->preview); previewLayout->setContentsMargins(0,4,0,0);
    auto *previewTop=new QHBoxLayout; d->previewTitle=new QLabel("Compare versions"); d->previewTitle->setObjectName("gitDiffTitle"); d->version=new QComboBox; d->version->setObjectName("gitDiffVersion"); d->version->addItem("Working file",false); d->version->addItem("Staged snapshot",true);
    d->openFile=button("Open working file","gitOpenWorkingFile"); previewTop->addWidget(d->previewTitle,1); previewTop->addWidget(d->version); previewTop->addWidget(d->openFile); previewLayout->addLayout(previewTop);
    auto *comparison=new QSplitter(Qt::Horizontal); d->original=new CodeEditor; d->modified=new CodeEditor; d->original->setObjectName("gitOriginal"); d->modified->setObjectName("gitModified");
    for(auto *editor:{d->original,d->modified}) { editor->setReadOnly(true); editor->setMinimumWidth(80); editor->setPlaceholderText("Select a changed file"); comparison->addWidget(editor); }
    previewLayout->addWidget(comparison,1); vertical->addWidget(d->preview); vertical->setStretchFactor(0,1); vertical->setStretchFactor(1,2); d->preview->hide(); d->tabs->addTab(changesPage,"Changes");
    auto *worktreesPage=new QWidget; auto *worktreesLayout=new QVBoxLayout(worktreesPage); worktreesLayout->setContentsMargins(0,8,0,0);
    d->trees=new QTreeWidget; d->trees->setObjectName("gitWorktrees"); d->trees->setHeaderLabels({"Worktree","Branch","State"}); d->trees->setRootIsDecorated(false); d->trees->setUniformRowHeights(true); d->trees->header()->setSectionResizeMode(0,QHeaderView::Stretch); d->trees->header()->setSectionResizeMode(1,QHeaderView::Stretch); d->trees->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents); worktreesLayout->addWidget(d->trees,1);
    auto *treeActions=new QHBoxLayout; d->mount=button("Open worktree","gitMountWorktree"); d->remove=button("Delete worktree…","gitDeleteWorktree"); treeActions->addWidget(d->mount); treeActions->addStretch(); treeActions->addWidget(d->remove); worktreesLayout->addLayout(treeActions); d->tabs->addTab(worktreesPage,"Worktrees");
    connect(d->service,&GitService::completed,this,&GitPanel::finish);
    connect(d->service,&GitService::worktreeRemoved,this,&GitPanel::worktreeRemoved);
    connect(d->service,&GitService::repositoryTopologyChanged,this,[this](const QString &common,quint64){
        if(d->workspace.isEmpty()||(!d->snapshot.commonGitDir.isEmpty()&&GitService::pathKey(common)!=GitService::pathKey(d->snapshot.commonGitDir)))return;
        d->topologyRefreshPending=true;refreshAfterTopology();
    });
    connect(d->refresh,&QPushButton::clicked,this,&GitPanel::refresh);
    connect(d->fetch,&QPushButton::clicked,this,[this] { if(d->mutating||!d->ready||d->fetchRequest) return; d->fetchRequest=d->track(d->service->branches(d->workspace,true),GitOperation::Branches); updateActions(); setMessage("Refreshing remote branches…"); });
    connect(d->switchBranch,&QPushButton::clicked,this,&GitPanel::switchDialog);
    connect(d->createBranch,&QPushButton::clicked,this,[this] { createDialog(false); });
    connect(d->createWorktree,&QPushButton::clicked,this,[this] { createDialog(true); });
    connect(d->changes,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem *item) { openSelectedDiff(item->data(0,Qt::UserRole+2).toBool()); });
    connect(d->changes,&QTreeWidget::itemSelectionChanged,this,[this] {
        if(auto *item=d->changes->currentItem()) openSelectedDiff(item->data(0,Qt::UserRole+2).toBool());
    });
    connect(d->version,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int) { if(!d->selectedPath.isEmpty()) openSelectedDiff(d->version->currentData().toBool()); });
    connect(d->openFile,&QPushButton::clicked,this,[this] { if(!d->selectedPath.isEmpty() && !d->selectedStaged) emit openDiffRequested(d->workspace,d->selectedPath,false,d->selectedOldPath); });
    connect(d->trees,&QTreeWidget::itemSelectionChanged,this,&GitPanel::updateActions);
    auto mount=[this] { const auto *item=d->trees->currentItem(); if(!item || d->mutating) return; const auto path=item->data(0,Qt::UserRole).toString(); if(GitService::pathKey(path)!=GitService::pathKey(d->workspace)) emit mountWorktreeRequested(path,{}); };
    connect(d->mount,&QPushButton::clicked,this,mount); connect(d->trees,&QTreeWidget::itemActivated,this,[mount](QTreeWidgetItem *) { mount(); });
    connect(d->remove,&QPushButton::clicked,this,[this] { const auto *item=d->trees->currentItem(); if(!item||d->mutating||item->data(0,Qt::UserRole+1).toBool()) return; const auto path=item->data(0,Qt::UserRole).toString(); d->track(d->service->deletionInfo(path),GitOperation::DeletionInfo,path); d->remove->setEnabled(false); setMessage("Checking worktree status…"); });
    d->poll=new QTimer(this); d->poll->setInterval(5000); connect(d->poll,&QTimer::timeout,this,[this] { if(isVisible() && !d->mutating && d->requests.isEmpty()) refresh(); }); d->poll->start();
    updateActions();
}
GitPanel::~GitPanel() { d->service->disconnect(this); d->service->cancelAll(); }
void GitPanel::setMutationGuard(std::function<bool(const QString &,GitOperation)> guard) { d->guard=std::move(guard); }
GitService *GitPanel::service() const { return d->service; }
QString GitPanel::workspace() const { return d->workspace; }
bool GitPanel::isMutating() const { return d->mutating; }
void GitPanel::setDarkTheme(bool dark) { d->original->setDarkTheme(dark); d->modified->setDarkTheme(dark); }
void GitPanel::setWorkspaceContext(const GitWorkspaceContext &context) { d->context=context; d->service->setWorkspaceContext(context); }
void GitPanel::setWorkspace(const QString &workspace,const QString &contextId) {
    const auto resolved=GitService::canonicalPath(workspace);
    if(GitService::pathKey(resolved)==GitService::pathKey(d->workspace) && contextId==d->contextId && d->ready) return;
    ++d->generation; ++d->previewGeneration;
    for(auto it=d->requests.begin();it!=d->requests.end();++it) {
        const auto operation=it->operation;
        if(operation==GitOperation::Inspect||operation==GitOperation::Diff||operation==GitOperation::Branches||operation==GitOperation::Worktrees||operation==GitOperation::FileDiff||operation==GitOperation::DeletionInfo) d->service->cancel(it.key());
    }
    d->workspace=resolved; d->contextId=contextId; d->ready=false; d->snapshot={}; d->branches={}; d->worktrees.clear(); d->diff={}; d->selectedPath.clear(); d->selectedExists=false;d->topologyRefreshPending=false;
    { QSignalBlocker block(d->changes); d->changes->clear(); } d->trees->clear(); d->preview->hide();
    d->summary->setText(resolved.isEmpty()?"Connect a repository to view changes and worktrees.":resolved); d->heading->setText("Source control"); setMessage({}); updateActions();
    if(!resolved.isEmpty()) refresh();
}
void GitPanel::refresh() {
    if(d->workspace.isEmpty() || d->mutating) return;
    for(const auto &request:d->requests) if(request.generation==d->generation && request.operation==GitOperation::Inspect) return;
    d->topologyRefreshPending=false;d->track(d->service->inspect(d->workspace),GitOperation::Inspect); d->refresh->setEnabled(false);
}
void GitPanel::refreshAfterTopology(){if(d->topologyRefreshPending&&!d->mutating&&d->requests.isEmpty())refresh();}
void GitPanel::setMessage(const QString &message,bool error) {
    d->message->setText(message); d->message->setVisible(!message.isEmpty()); d->message->setProperty("error",error);
    d->message->setStyleSheet(error?"color: #d65353;":"");
}
void GitPanel::updateActions() {
    const bool ready=d->ready&&!d->mutating;
    bool inspecting=false,checkingDeletion=false;
    for(const auto &request:d->requests) if(request.generation==d->generation) { inspecting|=request.operation==GitOperation::Inspect; checkingDeletion|=request.operation==GitOperation::DeletionInfo; }
    d->refresh->setEnabled(!d->workspace.isEmpty()&&!d->mutating&&!inspecting);
    d->switchBranch->setEnabled(ready&&!d->branches.branches.isEmpty()); d->createBranch->setEnabled(ready&&!d->branches.branches.isEmpty()); d->createWorktree->setEnabled(ready&&!d->branches.branches.isEmpty()); d->fetch->setEnabled(ready&&!d->fetchRequest);
    const auto *item=d->trees->currentItem(); const auto path=item?item->data(0,Qt::UserRole).toString():QString();
    d->mount->setEnabled(ready && item && GitService::pathKey(path)!=GitService::pathKey(d->workspace));
    d->remove->setEnabled(ready && item && !item->data(0,Qt::UserRole+1).toBool()&&!checkingDeletion);
    d->openFile->setEnabled(ready && !d->selectedPath.isEmpty() && !d->selectedStaged && d->selectedExists);
}
void GitPanel::loadBranches() { d->track(d->service->branches(d->workspace),GitOperation::Branches); }
void GitPanel::populateChanges() {
    const auto selected=d->selectedPath; bool found=false;
    QSignalBlocker block(d->changes); d->changes->clear();
    for(const auto &entry:d->diff.entries) {
        auto *item=new QTreeWidgetItem(d->changes,{entry.path,QString(entry.status)+(entry.staged?" · Staged":" · Working"),entry.binary?QStringLiteral("—"):QString::number(entry.added),entry.binary?QStringLiteral("—"):QString::number(entry.deleted)});
        item->setData(0,Qt::UserRole,entry.path); item->setData(0,Qt::UserRole+1,entry.oldPath); item->setData(0,Qt::UserRole+2,entry.staged);
        item->setToolTip(0,entry.oldPath.isEmpty()?entry.path:entry.oldPath+" → "+entry.path);
        if(entry.binary) item->setToolTip(1,"Binary content");
        item->setForeground(2,QColor("#32865a")); item->setForeground(3,QColor("#c95d56"));
        if(entry.path==selected) { d->changes->setCurrentItem(item); found=true; }
    }
    d->tabs->setTabText(0,QString("Changes (%1)").arg(d->diff.entries.size()));
    if(found) openSelectedDiff(d->selectedStaged);
    else { d->selectedPath.clear(); d->selectedExists=false; ++d->previewGeneration; d->preview->hide(); }
}
void GitPanel::populateWorktrees() {
    const auto selected=d->trees->currentItem()?d->trees->currentItem()->data(0,Qt::UserRole).toString():QString(); QSignalBlocker block(d->trees); d->trees->clear();
    auto sorted=d->worktrees; std::stable_sort(sorted.begin(),sorted.end(),[](const auto &a,const auto &b){ if(a.main!=b.main) return a.main; return a.createdAt>b.createdAt; });
    for(const auto &tree:sorted) {
        QStringList states; if(tree.main) states<<"Main"; if(tree.current) states<<"Current"; if(tree.locked) states<<"Locked";
        auto *item=new QTreeWidgetItem(d->trees,{QFileInfo(tree.path).fileName(),branchLabel(tree),states.join(" · ")}); item->setData(0,Qt::UserRole,tree.path); item->setData(0,Qt::UserRole+1,tree.main); item->setToolTip(0,tree.path);
        if(tree.path==selected) d->trees->setCurrentItem(item);
    }
    d->tabs->setTabText(1,QString("Worktrees (%1)").arg(sorted.size())); updateActions();
}
void GitPanel::openSelectedDiff(bool staged) {
    const auto *item=d->changes->currentItem(); if(!item) return;
    const bool changed=d->selectedPath!=item->data(0,Qt::UserRole).toString() || d->selectedStaged!=staged;
    d->selectedPath=item->data(0,Qt::UserRole).toString(); d->selectedOldPath=item->data(0,Qt::UserRole+1).toString(); d->selectedStaged=staged; ++d->previewGeneration;
    { QSignalBlocker block(d->version); d->version->setCurrentIndex(staged?1:0); }
    d->preview->show(); d->previewTitle->setText("HEAD → "+QString(staged?"Index":"Working file")+" · "+d->selectedPath);
    if(changed) { d->selectedExists=false; d->original->setPlainText({}); d->modified->setPlainText({}); d->original->setPlaceholderText("Loading committed version…"); d->modified->setPlaceholderText("Loading comparison…"); }
    d->track(d->service->fileDiff(d->workspace,d->selectedPath,staged,d->selectedOldPath),GitOperation::FileDiff); updateActions();
}
void GitPanel::finish(quint64 id,const GitResult &result) {
    const auto found=d->requests.find(id); if(found==d->requests.end()) return; const auto request=found.value(); d->requests.erase(found);
    const bool fetched=d->fetchRequest==id; if(fetched) d->fetchRequest=0;
    const QPointer<GitPanel> self(this);
    const bool mutation=request.operation==GitOperation::SwitchBranch||request.operation==GitOperation::CreateBranch||request.operation==GitOperation::CreateWorktree||request.operation==GitOperation::RemoveWorktree;
    if(mutation) {
        d->mutating=false;
        emit mutationFinished(request.path,request.operation,result.ok);
        if(!self) return;
        if(request.operation==GitOperation::RemoveWorktree) emit worktreeRemovalFinished(request.path,result.ok);
        if(!self) return;
        if(result.ok || result.mayHaveMutated) emit repositoryChanged(request.path);
        if(!self) return;
    }
    if(request.generation!=d->generation) { updateActions(); if(mutation) refresh();else refreshAfterTopology(); return; }
    if(!result.ok) {
        if(result.error.code!=GitErrorCode::Cancelled) setMessage(result.error.message,true);
        if(request.operation==GitOperation::Inspect) d->ready=false;
        if(request.operation==GitOperation::FileDiff && request.preview==d->previewGeneration) { d->original->setPlaceholderText("Comparison unavailable"); d->modified->setPlaceholderText("Comparison unavailable"); }
        updateActions(); if(mutation && result.mayHaveMutated) refresh();else refreshAfterTopology(); return;
    }
    if(mutation) {
        setMessage(result.warnings.isEmpty()?"Git operation completed.":result.warnings.join('\n'));
        if(request.operation==GitOperation::CreateWorktree) emit mountWorktreeRequested(result.path,result.setup);
        if(!self) return;
        if(request.operation==GitOperation::RemoveWorktree && GitService::pathKey(result.path)==GitService::pathKey(d->workspace)) { setWorkspace({}); return; }
        refresh(); updateActions(); return;
    }
    switch(request.operation) {
    case GitOperation::Inspect:
        if(!d->ready) setMessage({});
        d->ready=true; d->snapshot=result.snapshot; d->heading->setText(QFileInfo(d->workspace).fileName()+" · "+(result.snapshot.branch.isEmpty()?result.snapshot.headSha.left(7):result.snapshot.branch));
        d->summary->setText(QString("%1 staged · %2 working · %3 untracked\n%4").arg(result.snapshot.status.stagedCount).arg(result.snapshot.status.unstagedCount).arg(result.snapshot.status.untrackedCount).arg(d->workspace));
        d->track(d->service->diff(d->workspace),GitOperation::Diff); d->track(d->service->worktrees(d->workspace),GitOperation::Worktrees); loadBranches(); break;
    case GitOperation::Branches:
        d->branches=result.branches;
        if(result.branches.refreshSucceeded) setMessage("Remote branches refreshed.");
        else if(fetched) setMessage("Remote refresh failed. Showing the available local refs.",true);
        break;
    case GitOperation::Diff: d->diff=result.diff; populateChanges(); break;
    case GitOperation::Worktrees: d->worktrees=result.worktrees; populateWorktrees(); break;
    case GitOperation::FileDiff: {
        if(request.preview!=d->previewGeneration) break;
        TextFile original,modified; QString error;
        const bool decoded=TextFileIO::decode(result.fileDiff.original,&original,&error)&&TextFileIO::decode(result.fileDiff.modified,&modified,&error);
        d->selectedExists=result.fileDiff.modifiedExists;
        if(!decoded) { d->selectedExists=false; d->original->setPlainText({}); d->modified->setPlainText({}); d->original->setPlaceholderText("Binary or unsupported text encoding"); d->modified->setPlaceholderText("Binary or unsupported text encoding"); setMessage("This content cannot be displayed as a text comparison."); }
        else { d->original->setFileName(d->selectedOldPath.isEmpty()?d->selectedPath:d->selectedOldPath); d->modified->setFileName(d->selectedPath); if(d->original->toPlainText()!=original.text) d->original->setPlainText(original.text); if(d->modified->toPlainText()!=modified.text) d->modified->setPlainText(modified.text); d->original->setPlaceholderText(result.fileDiff.originalExists?"Empty committed file":"No committed file"); d->modified->setPlaceholderText(result.fileDiff.modifiedExists?"Empty file":"File is absent in this version"); }
        break;
    }
    case GitOperation::DeletionInfo: deletionDialog(result.deletion); break;
    default: break;
    }
    updateActions();
    refreshAfterTopology();
}
void GitPanel::startMutation(GitOperation operation,const QString &path,const QString &name,const QString &source,bool force) {
    if(d->mutating) return;
    const auto generation=d->generation; const QPointer<GitPanel> self(this);
    const auto guard=d->guard; const bool accepted=!guard||guard(path,operation);
    if(!self) return;
    if(!accepted) { setMessage("Git operation was cancelled."); return; }
    if(generation!=d->generation) { setMessage("Git operation was cancelled because the active chat changed."); return; }
    d->mutating=true; updateActions(); setMessage("Applying Git operation…"); quint64 id=0;
    emit mutationStarted(path,operation); if(!self) return;
    if(operation==GitOperation::SwitchBranch) id=d->service->switchBranch(path,name);
    else if(operation==GitOperation::CreateBranch) id=d->service->createBranch(path,name,source);
    else if(operation==GitOperation::CreateWorktree) id=d->service->createWorktree(path,name,source);
    else if(operation==GitOperation::RemoveWorktree) { emit worktreeRemovalStarted(path); if(!self) return; id=d->service->removeWorktree(path,GitRemovePolicy::Explicit,force); }
    else { d->mutating=false; updateActions(); return; }
    d->requests.insert(id,{generation,d->previewGeneration,operation,path});
}
void GitPanel::createDialog(bool worktree) {
    if(d->mutating||!d->ready) return;
    QDialog dialog(this); dialog.setObjectName("gitCreateDialog"); dialog.setWindowTitle(worktree?"Create worktree":"Create branch"); auto *layout=new QVBoxLayout(&dialog);
    auto *description=new QLabel(worktree?"Create a new branch in a separate working directory.":"Create and switch to a new branch in this workspace."); description->setWordWrap(true); layout->addWidget(description);
    auto *form=new QFormLayout; auto *name=new QLineEdit(suggestedName(d->branches,d->worktrees)); name->setObjectName("gitNewName"); auto *source=new QComboBox; source->setObjectName("gitSourceBranch");
    for(const auto &branch:d->branches.branches) source->addItem(branch.name,branch.name);
    const auto preferred=d->branches.defaultRemoteBranch.isEmpty()?d->branches.defaultBranch:d->branches.defaultRemoteBranch; const int index=source->findData(preferred); if(index>=0) source->setCurrentIndex(index);
    form->addRow(worktree?"Worktree name":"Branch name",name); form->addRow("Create from",source); layout->addLayout(form);
    auto *validation=new QLabel; validation->setObjectName("gitNameValidation"); validation->setWordWrap(true); layout->addWidget(validation);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(worktree?"Create worktree":"Create branch"); layout->addWidget(buttons);
    auto validate=[&] { const auto value=name->text().trimmed(); QString error; if(value.isEmpty()) error="Enter a name."; for(const auto &branch:d->branches.branches) if(!branch.remote && branch.name.compare(value,Qt::CaseInsensitive)==0) error="A branch with this name already exists."; if(source->currentIndex()<0) error="Select an available source branch."; validation->setText(error); buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty()); };
    connect(name,&QLineEdit::textChanged,&dialog,validate); connect(source,qOverload<int>(&QComboBox::currentIndexChanged),&dialog,validate); connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject); validate(); name->selectAll(); dialog.resize(460,220);
    const auto workspace=d->workspace; const auto generation=d->generation;
    if(dialog.exec()==QDialog::Accepted && generation==d->generation) startMutation(worktree?GitOperation::CreateWorktree:GitOperation::CreateBranch,workspace,name->text().trimmed(),source->currentData().toString());
}
void GitPanel::switchDialog() {
    if(d->mutating||!d->ready) return;
    QDialog dialog(this); dialog.setObjectName("gitSwitchDialog"); dialog.setWindowTitle("Switch branch"); auto *layout=new QVBoxLayout(&dialog); auto *choices=new QComboBox; choices->setObjectName("gitCheckoutBranch");
    for(const auto &branch:d->branches.branches) {
        if(branch.remote) continue;
        const bool disabled=branch.checkedOut&&!branch.current; choices->addItem(branch.name+(disabled?" · open in another worktree":""),branch.name);
        if(auto *model=qobject_cast<QStandardItemModel *>(choices->model())) if(auto *item=model->item(choices->count()-1)) { item->setEnabled(!disabled); if(disabled) item->setToolTip(branch.checkedOutPath); }
    }
    const int current=choices->findData(d->branches.current); if(current>=0) choices->setCurrentIndex(current);
    layout->addWidget(new QLabel("Git will refuse changes that would overwrite local files.")); layout->addWidget(choices);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText("Switch branch"); layout->addWidget(buttons); connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    const auto workspace=d->workspace; const auto generation=d->generation; dialog.resize(440,140);
    if(dialog.exec()==QDialog::Accepted && generation==d->generation && choices->currentIndex()>=0) startMutation(GitOperation::SwitchBranch,workspace,choices->currentData().toString());
}
void GitPanel::deletionDialog(const GitDeletionInfo &info) {
    if(info.main) { setMessage("The main worktree cannot be deleted.",true); return; }
    const bool dirty=!info.status.known||info.status.dirty;
    QMessageBox dialog(dirty?QMessageBox::Warning:QMessageBox::Question,"Delete worktree?",
        QString(dirty?"This worktree has uncommitted changes. Deleting it will permanently discard those changes.\n\n":"This permanently deletes the worktree directory, including ignored files.\n\n")+info.path+"\n\nBranch (will be kept): "+(info.branch.isEmpty()?"Detached":info.branch),QMessageBox::Cancel,this);
    dialog.setObjectName("gitDeleteConfirmation"); auto *remove=dialog.addButton(dirty?"Delete anyway":"Delete",QMessageBox::DestructiveRole); remove->setObjectName("gitConfirmDelete"); dialog.setDefaultButton(QMessageBox::Cancel);
    const auto generation=d->generation; dialog.exec();
    if(dialog.clickedButton()==remove && generation==d->generation) startMutation(GitOperation::RemoveWorktree,info.path,{},{},dirty);
}
}
