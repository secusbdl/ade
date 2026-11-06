#pragma once
#include "attachments/AttachmentStore.h"
#include "attachments/FileReadCapture.h"
#include <QHash>
#include <QJsonObject>
#include <QObject>

namespace ade {
// Read/list access to one chat's original attachment blobs. History snapshots
// are a separate store and are never part of this filesystem mount.
class AttachmentFileBroker final:public QObject {
    Q_OBJECT
public:
    AttachmentFileBroker(const QString& agentId,const QString& dataDirectory,QObject* parent=nullptr);
    QString rootDirectory() const {return m_root;}
    FileReadSource formattedSource() const;
public slots:
    void execute(const QString& id,const QString& agentId,const QString& operation,const QJsonObject& arguments);
    void cancel(const QString& id);
    void cancelAll();
signals:
    void completed(const QString& id,const QString& output,bool error);
private:
    void perform(const QString& id,quint64 generation,const QString& operation,const QJsonObject& arguments);
    void finish(const QString& id,const QString& output,bool error);
    QString list(const QString& path,bool recursive,QString* error) const;
    AttachmentStore m_store;
    QString m_agentId,m_root,m_rootError;
    QHash<QString,quint64> m_pending;
    quint64 m_generation=0;
};
}
