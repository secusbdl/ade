#include "DownloadsPage.h"
#include "browser/browserwidget.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace ade {
namespace {
constexpr int MaxRecords = 500;
constexpr qint64 MaxHistoryBytes = 4 * 1024 * 1024;
bool activeState(const QString& state) { return state == "awaiting-path" || state == "in-progress" || state == "paused"; }
QString stateLabel(const QString& state) {
    if(state=="awaiting-path")return QObject::tr("Choose location");
    if(state=="in-progress")return QObject::tr("Downloading");
    if(state=="paused")return QObject::tr("Paused");
    if(state=="completed")return QObject::tr("Complete");
    if(state=="canceled")return QObject::tr("Canceled");
    return QObject::tr("Interrupted");
}
QString byteLabel(qint64 bytes) { return bytes < 0 ? QObject::tr("Unknown size") : QLocale().formattedDataSize(bytes,1); }
}

DownloadsPage::DownloadsPage(const QString& dataDirectory,QWidget* parent)
    : QWidget(parent),m_directory(QDir(dataDirectory).absolutePath()),m_file(QDir(m_directory).filePath("downloads.json")) {
    setObjectName("downloadsPage");
    m_lastDirectory=QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if(m_lastDirectory.isEmpty())m_lastDirectory=QDir::homePath();
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,16,20,16);layout->setSpacing(12);
    auto* title=new QLabel(tr("Downloads"));title->setObjectName("pageHeading");QFont heading=font();heading.setPointSizeF(font().pointSizeF()+4);heading.setWeight(QFont::DemiBold);title->setFont(heading);layout->addWidget(title);
    auto* top=new QHBoxLayout;
    m_search=new QLineEdit;m_search->setObjectName("downloadsSearch");m_search->setPlaceholderText(tr("Search downloads"));top->addWidget(m_search,1);
    auto* clear=new QPushButton(tr("Clear finished history"));clear->setObjectName("downloadsClearHistory");clear->setToolTip(tr("Remove history entries. Downloaded files stay on disk."));top->addWidget(clear);layout->addLayout(top);
    m_list=new QTreeWidget;m_list->setObjectName("downloadsList");m_list->setColumnCount(5);m_list->setHeaderLabels({tr("File"),tr("Progress"),tr("Size"),tr("Status"),tr("Source")});
    m_list->setRootIsDecorated(false);m_list->setUniformRowHeights(true);m_list->setSelectionMode(QAbstractItemView::SingleSelection);m_list->setAlternatingRowColors(true);
    m_list->header()->setSectionResizeMode(0,QHeaderView::Stretch);m_list->header()->setSectionResizeMode(4,QHeaderView::Stretch);
    for(int column=1;column<=3;++column)m_list->header()->setSectionResizeMode(column,QHeaderView::ResizeToContents);
    layout->addWidget(m_list,1);
    m_path=new QLabel;m_path->setObjectName("downloadSelectedPath");m_path->setTextFormat(Qt::PlainText);m_path->setWordWrap(true);m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(m_path);
    auto* actions=new QHBoxLayout;
    auto button=[actions](const QString& text,const QString& name){auto* result=new QPushButton(text);result->setObjectName(name);actions->addWidget(result);return result;};
    m_choose=button(tr("Choose location"),"downloadChooseLocation");m_pause=button(tr("Pause"),"downloadPause");m_resume=button(tr("Resume"),"downloadResume");m_cancel=button(tr("Cancel"),"downloadCancel");
    actions->addStretch();m_open=button(tr("Open"),"downloadOpen");m_folder=button(tr("Show folder"),"downloadShowFolder");m_remove=button(tr("Remove history"),"downloadRemoveHistory");layout->addLayout(actions);
    m_status=new QLabel;m_status->setObjectName("downloadsStatus");m_status->setTextFormat(Qt::PlainText);m_status->setWordWrap(true);layout->addWidget(m_status);
    m_saveTimer.setSingleShot(true);m_saveTimer.setInterval(500);connect(&m_saveTimer,&QTimer::timeout,this,[this]{save();});
    connect(m_search,&QLineEdit::textChanged,this,[this]{filterRows();});connect(m_list,&QTreeWidget::itemSelectionChanged,this,[this]{updateActions();});
    connect(clear,&QPushButton::clicked,this,[this]{clearHistory();});connect(m_remove,&QPushButton::clicked,this,&DownloadsPage::removeSelectedHistory);
    connect(m_choose,&QPushButton::clicked,this,[this]{requestDestination(selectedId());});
    connect(m_pause,&QPushButton::clicked,this,[this]{const auto id=selectedId();if(auto browser=m_live.value(id).browser)if(!browser->pauseDownload(id))m_status->setText(tr("This download could not be paused."));});
    connect(m_resume,&QPushButton::clicked,this,[this]{const auto id=selectedId();if(auto browser=m_live.value(id).browser)if(!browser->resumeDownload(id))m_status->setText(tr("This download cannot currently resume."));});
    connect(m_cancel,&QPushButton::clicked,this,[this]{const auto id=selectedId();if(auto browser=m_live.value(id).browser)browser->cancelDownload(id);});
    connect(m_open,&QPushButton::clicked,this,[this]{const auto record=m_records.value(selectedId());if(record.state=="completed"&&QFileInfo(record.path).isFile()&&!QDesktopServices::openUrl(QUrl::fromLocalFile(record.path)))m_status->setText(tr("The file could not be opened."));});
    connect(m_folder,&QPushButton::clicked,this,[this]{const QFileInfo file(m_records.value(selectedId()).path);if(!file.filePath().isEmpty()&&QFileInfo(file.absolutePath()).isDir()&&!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath())))m_status->setText(tr("The folder could not be opened."));});
    reload();updateActions();
}

DownloadsPage::~DownloadsPage() {
    m_destroying=true;m_saveTimer.stop();
    for(auto* browser:std::as_const(m_attached))disconnect(browser,nullptr,this,nullptr);
    const auto live=std::exchange(m_live,{});
    for(auto it=live.begin();it!=live.end();++it) {
        if(it->browser)it->browser->cancelDownload(it.key());
        auto record=m_records.find(it.key());if(record==m_records.end())continue;
        record->state="interrupted";record->canResume=false;record->ended=QDateTime::currentMSecsSinceEpoch();record->error=tr("ADE closed before the download finished. Restarting this transfer is not supported yet.");
    }
    save();
}

void DownloadsPage::setDestinationChooser(DestinationChooser chooser) { m_chooser=std::move(chooser); }
void DownloadsPage::attachBrowser(BrowserWidget* browser) {
    if(!browser||m_attached.contains(browser))return;
    m_attached.insert(browser);
    connect(browser,&QObject::destroyed,this,[this,browser]{browserDestroyed(browser);});
    connect(browser,&BrowserWidget::downloadRequested,this,[this,browser](const QString& id,const QUrl& source,const QString& name,const QString& mime,qint64 total){
        const QPointer<DownloadsPage> self(this);
        if(m_records.contains(id)){browser->cancelDownload(id);return;}
        if(m_order.size()>=MaxRecords) {
            browser->cancelDownload(id);
            m_status->setText(tr("Download history has reached 500 entries. Clear finished history before starting another download."));return;
        }
        Record record;record.id=id;record.url=source.toString(QUrl::FullyEncoded).left(16384);record.name=name.left(1024);record.mime=mime.left(256);record.total=total;record.started=QDateTime::currentMSecsSinceEpoch();record.state="awaiting-path";
        m_records.insert(id,record);m_order.prepend(id);m_live.insert(id,{browser,browser});changed(id,true);if(!self)return;
        m_list->setCurrentItem(m_items.value(id));if(!self)return;
        emit downloadAdded(id);if(!self)return;emit activeCountChanged(m_live.size());if(!self)return;
        // DownloadStarting's native callback is already deferred. Queue the
        // chooser again so listeners can switch to this page before it opens.
        QTimer::singleShot(0,this,[this,id]{requestDestination(id);});
    });
    connect(browser,&BrowserWidget::downloadStarted,this,[this,browser](const QString& id,const QString& path){
        if(m_live.value(id).identity!=browser||!m_records.contains(id))return;
        auto& record=m_records[id];record.path=path;record.name=QFileInfo(path).fileName();record.state="in-progress";record.error.clear();m_lastDirectory=QFileInfo(path).absolutePath();changed(id,true);
    });
    connect(browser,&BrowserWidget::downloadProgress,this,[this,browser](const QString& id,qint64 received,qint64 total){
        if(m_live.value(id).identity!=browser||!m_records.contains(id))return;
        auto& record=m_records[id];record.received=qMax<qint64>(0,received);record.total=qMax<qint64>(-1,total);changed(id);
    });
    connect(browser,&BrowserWidget::downloadStateChanged,this,[this,browser](const QString& id,const QString& state,bool canResume,const QString& error){
        if(m_live.value(id).identity!=browser||!m_records.contains(id))return;
        auto& record=m_records[id];record.state=state;record.canResume=canResume;record.error=error;changed(id,true);
    });
    connect(browser,&BrowserWidget::downloadFinished,this,[this,browser](const QString& id,const QString& path,bool success,const QString& error){
        if(m_live.value(id).identity!=browser||!m_records.contains(id))return;
        m_live.remove(id);auto& record=m_records[id];record.path=path;record.state=success?QStringLiteral("completed"):(record.state=="interrupted"?QStringLiteral("interrupted"):QStringLiteral("canceled"));
        record.canResume=false;record.error=error;record.ended=QDateTime::currentMSecsSinceEpoch();const QPointer<DownloadsPage> self(this);changed(id,true);if(self)emit activeCountChanged(m_live.size());
    });
}

void DownloadsPage::requestDestination(const QString& id) {
    if(!m_records.contains(id)||m_records.value(id).state!="awaiting-path")return;
    const QPointer<BrowserWidget> browser=m_live.value(id).browser;if(!browser)return;
    const QPointer<DownloadsPage> self(this);const auto record=m_records.value(id);
    const QString suggested=QDir(m_lastDirectory).filePath(QFileInfo(record.name).fileName().isEmpty()?QStringLiteral("download.bin"):QFileInfo(record.name).fileName());
    DestinationChoice choice;
    const auto chooser=m_chooser;
    if(chooser)choice=chooser(this,QUrl(record.url),suggested);
    else {
        choice.path=QFileDialog::getSaveFileName(this,tr("Save download"),suggested);
        // The default dialog asks before replacing an existing file.
        choice.overwriteConfirmed=!choice.path.isEmpty()&&QFileInfo::exists(choice.path);
    }
    if(!self||!browser||m_live.value(id).browser!=browser)return;
    if(choice.path.isEmpty()){browser->cancelDownload(id);return;}
    if(!browser->acceptDownload(id,choice.path,choice.overwriteConfirmed)) {
        if(!self)return;
        m_status->setText(tr("The destination was not accepted. Choose another location or cancel the download."));
    }
}
void DownloadsPage::browserDestroyed(BrowserWidget* browser) {
    const QPointer<DownloadsPage> self(this);
    m_attached.remove(browser);
    const auto ids=m_live.keys();
    for(const auto& id:ids)if(m_live.value(id).identity==browser) {
        m_live.remove(id);auto& record=m_records[id];record.state="interrupted";record.canResume=false;record.ended=QDateTime::currentMSecsSinceEpoch();record.error=tr("The browser tab closed before the download finished. Restarting this transfer is not supported yet.");updateRow(id);
    }
    save();if(!self)return;updateActions();emit recordsChanged();if(self)emit activeCountChanged(m_live.size());
}

QJsonObject DownloadsPage::encode(const Record& r) {
    return {{"id",r.id},{"url",r.url},{"filename",r.name},{"targetPath",r.path},{"mimeType",r.mime},{"state",r.state},
        {"receivedBytes",r.received},{"totalBytes",r.total},{"startTime",r.started},{"endTime",r.ended},{"error",r.error}};
}
QJsonArray DownloadsPage::records() const { QJsonArray result;for(const auto& id:m_order)result.append(encode(m_records.value(id)));return result; }
void DownloadsPage::setStorageError(const QString& error) {
    m_storageError=error;if(m_status)m_status->setText(error);
    if(!m_destroying)emit storageErrorOccurred(error);
}
bool DownloadsPage::save() {
    m_saveTimer.stop();
    if(!m_storageValid)return false;
    QFile existing(m_file);const bool exists=existing.exists();QByteArray current;
    if(exists&&(!existing.open(QIODevice::ReadOnly)||existing.size()>MaxHistoryBytes)){setStorageError(tr("Download history could not be read safely; existing history was preserved."));return false;}
    if(exists)current=existing.readAll();
    existing.close();
    if(exists!=m_fileExisted||current!=m_diskSnapshot){setStorageError(tr("Download history changed outside this window. Reload it before saving; existing history was preserved."));return false;}
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"records",records()}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>MaxHistoryBytes||!QDir().mkpath(m_directory)){setStorageError(tr("Download history could not be saved."));return false;}
    QSaveFile output(m_file);
    if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit()){setStorageError(tr("Download history could not be saved: ")+output.errorString());return false;}
    m_diskSnapshot=bytes;m_fileExisted=true;m_storageError.clear();return true;
}
bool DownloadsPage::reload() {
    if(!m_live.isEmpty()){setStorageError(tr("Finish or cancel active downloads before reloading history."));return false;}
    QFile file(m_file);m_fileExisted=file.exists();m_diskSnapshot.clear();m_storageValid=true;
    QHash<QString,Record> loaded;QStringList order;
    if(m_fileExisted) {
        if(!file.open(QIODevice::ReadOnly)||file.size()>MaxHistoryBytes){m_storageValid=false;setStorageError(tr("Download history could not be read safely."));return false;}
        m_diskSnapshot=file.readAll();QJsonParseError parse;const auto document=QJsonDocument::fromJson(m_diskSnapshot,&parse);
        if(parse.error!=QJsonParseError::NoError||!document.isObject()||document.object().value("version").toInt()!=1||!document.object().value("records").isArray()||document.object().value("records").toArray().size()>MaxRecords){m_storageValid=false;setStorageError(tr("Download history is invalid. The original file has been preserved."));return false;}
        for(const auto& value:document.object().value("records").toArray()) {
            const auto object=value.toObject();Record record;
            record.id=object.value("id").toString();record.url=object.value("url").toString();record.name=object.value("filename").toString();record.path=object.value("targetPath").toString();record.mime=object.value("mimeType").toString();record.state=object.value("state").toString();record.error=object.value("error").toString();
            record.received=object.value("receivedBytes").toInteger(-1);record.total=object.value("totalBytes").toInteger(-2);record.started=object.value("startTime").toInteger(-1);record.ended=object.value("endTime").toInteger(-1);
            static const QSet<QString> states{"awaiting-path","in-progress","paused","interrupted","completed","canceled"};
            if(!value.isObject()||record.id.isEmpty()||record.id.size()>128||loaded.contains(record.id)||record.url.size()>16384||record.name.size()>1024||record.path.size()>32768||record.error.size()>8192||!states.contains(record.state)||record.received<0||record.total<-1||record.started<0||record.ended<0||(!record.path.isEmpty()&&!QDir::isAbsolutePath(record.path))) {
                m_storageValid=false;setStorageError(tr("Download history contains an invalid record. The original file has been preserved."));return false;
            }
            if(activeState(record.state)){record.state="interrupted";record.error=tr("ADE closed before the download finished. Restarting this transfer is not supported yet.");record.ended=QDateTime::currentMSecsSinceEpoch();}
            loaded.insert(record.id,record);order.append(record.id);
        }
    }
    m_records=loaded;m_order=order;m_storageError.clear();m_status->clear();rebuildRows();emit recordsChanged();return true;
}

bool DownloadsPage::clearHistory() {
    const QPointer<DownloadsPage> self(this);
    const auto previous=m_records;const auto previousOrder=m_order;
    for(const auto& id:previousOrder)if(!m_live.contains(id)){m_records.remove(id);m_order.removeAll(id);}
    const bool saved=save();if(!self)return false;
    if(!saved){m_records=previous;m_order=previousOrder;return false;}
    rebuildRows();m_status->setText(tr("Finished download history cleared. Downloaded files were kept."));emit recordsChanged();return true;
}
void DownloadsPage::removeSelectedHistory() {
    const auto id=selectedId();if(id.isEmpty()||m_live.contains(id))return;
    const auto record=m_records.take(id);const int index=m_order.indexOf(id);m_order.removeAll(id);
    const QPointer<DownloadsPage> self(this);const bool saved=save();if(!self)return;
    if(!saved){m_records.insert(id,record);m_order.insert(index,id);return;}
    rebuildRows();emit recordsChanged();
}
QString DownloadsPage::selectedId() const { const auto* item=m_list->currentItem();return item?item->data(0,Qt::UserRole).toString():QString(); }
void DownloadsPage::changed(const QString& id,bool immediately) {
    updateRow(id);updateActions();filterRows();const QPointer<DownloadsPage> self(this);if(immediately)save();else if(!m_saveTimer.isActive())m_saveTimer.start();if(self)emit recordsChanged();
}
void DownloadsPage::rebuildRows() {
    const auto selected=selectedId();m_items.clear();m_list->clear();for(const auto& id:m_order)updateRow(id);
    if(m_items.contains(selected))m_list->setCurrentItem(m_items.value(selected));
    filterRows();updateActions();
}
void DownloadsPage::updateRow(const QString& id) {
    if(!m_records.contains(id))return;
    const auto record=m_records.value(id);auto* item=m_items.value(id);
    if(!item){item=new QTreeWidgetItem;item->setData(0,Qt::UserRole,id);m_list->insertTopLevelItem(qMax(0,m_order.indexOf(id)),item);m_items.insert(id,item);}
    item->setText(0,record.name.isEmpty()?tr("Download"):record.name);
    QString progress=record.state=="completed"?QStringLiteral("100%"):record.total>0?QStringLiteral("%1%").arg(qBound(0,static_cast<int>(qMin(1.0,static_cast<double>(record.received)/static_cast<double>(record.total))*100.0),100)):byteLabel(record.received);
    item->setText(1,progress);item->setText(2,byteLabel(record.total));item->setText(3,stateLabel(record.state));item->setText(4,QUrl(record.url).host());
    item->setToolTip(0,record.path);item->setToolTip(3,record.error);item->setToolTip(4,record.url);
}
void DownloadsPage::filterRows() {
    const QString query=m_search->text().trimmed();
    for(auto it=m_items.begin();it!=m_items.end();++it){const auto record=m_records.value(it.key());it.value()->setHidden(!query.isEmpty()&&!record.name.contains(query,Qt::CaseInsensitive)&&!record.url.contains(query,Qt::CaseInsensitive)&&!record.path.contains(query,Qt::CaseInsensitive));}
}
void DownloadsPage::updateActions() {
    const auto id=selectedId();const auto record=m_records.value(id);const bool live=m_live.value(id).browser;
    m_choose->setEnabled(live&&record.state=="awaiting-path");m_pause->setEnabled(live&&record.state=="in-progress");m_resume->setEnabled(live&&record.canResume);m_cancel->setEnabled(live);
    m_open->setEnabled(!id.isEmpty()&&record.state=="completed"&&QFileInfo(record.path).isFile());m_folder->setEnabled(!record.path.isEmpty()&&QFileInfo(QFileInfo(record.path).absolutePath()).isDir());m_remove->setEnabled(!id.isEmpty()&&!live);
    m_resume->setToolTip(live?QString():tr("Resuming after the browser closes is not supported yet."));m_path->setText(record.error.isEmpty()?record.path:record.path+(record.path.isEmpty()?QString():QStringLiteral("\n"))+record.error);
}
}
