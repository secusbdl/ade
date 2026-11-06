#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <QUrl>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ade {
class BrowserWidget;

// One manager per application data directory. Keep it alive while its browser
// tabs are alive; hiding this page does not affect active transfers.
class DownloadsPage final : public QWidget {
    Q_OBJECT
public:
    struct DestinationChoice { QString path; bool overwriteConfirmed = false; };
    using DestinationChooser = std::function<DestinationChoice(QWidget*, const QUrl&, const QString&)>;
    explicit DownloadsPage(const QString& dataDirectory, QWidget* parent = nullptr);
    ~DownloadsPage() override;
    void attachBrowser(BrowserWidget* browser);
    QJsonArray records() const;
    int activeCount() const { return m_live.size(); }
    QString storageError() const { return m_storageError; }
    bool reload();
    bool clearHistory();
    // A platform integration seam; the default uses a native save-file dialog.
    void setDestinationChooser(DestinationChooser chooser);

signals:
    void recordsChanged();
    void storageErrorOccurred(const QString& error);
    void downloadAdded(const QString& downloadId);
    void activeCountChanged(int count);

private:
    struct Record {
        QString id, url, name, path, mime, state, error;
        qint64 received = 0, total = -1, started = 0, ended = 0;
        bool canResume = false;
    };
    struct Live { QPointer<BrowserWidget> browser; BrowserWidget* identity = nullptr; };
    QString m_directory, m_file, m_storageError, m_lastDirectory;
    QByteArray m_diskSnapshot;
    bool m_fileExisted = false, m_storageValid = true, m_destroying = false;
    QHash<QString, Record> m_records;
    QStringList m_order;
    QHash<QString, Live> m_live;
    QSet<BrowserWidget*> m_attached;
    QHash<QString, QTreeWidgetItem*> m_items;
    QTimer m_saveTimer;
    DestinationChooser m_chooser;
    QTreeWidget* m_list;
    QLineEdit* m_search;
    QLabel* m_status;
    QLabel* m_path;
    QPushButton *m_choose, *m_pause, *m_resume, *m_cancel, *m_open, *m_folder, *m_remove;
    void requestDestination(const QString& id);
    void browserDestroyed(BrowserWidget* browser);
    void changed(const QString& id, bool persistImmediately = false);
    void updateRow(const QString& id);
    void rebuildRows();
    void updateActions();
    void filterRows();
    QString selectedId() const;
    void removeSelectedHistory();
    bool save();
    void setStorageError(const QString& error);
    static QJsonObject encode(const Record& record);
};
}
