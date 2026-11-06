#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <atomic>
#include <memory>
#include "attachments/FileReadCapture.h"
#include "mutations/FileMutationExecutor.h"

namespace ade {

class ToolExecutor final : public QObject {
    Q_OBJECT
public:
    explicit ToolExecutor(QObject *parent = nullptr);
    ~ToolExecutor() override;
    bool setWorkspace(const QString &path);
    QString workspace() const { return m_workspace; }
    bool setAppsDirectory(const QString &path);
    QString appsDirectory() const { return m_appsDirectory; }
    QString resolvePath(const QString &path, QString *error = nullptr) const;
    static bool requiresApproval(const QString &name);
    static QJsonArray definitions();
    void setFileReadCommitter(FileReadCommitter committer) { if(m_id.isEmpty())m_fileReadCommitter=std::move(committer); }
    void setAttachmentReadSource(FileReadSource source) { if(m_id.isEmpty())m_attachmentReadSource=std::move(source); }
    void setFileMutationExecutor(FileMutationExecutor executor) { if(m_id.isEmpty())m_fileMutationExecutor=std::move(executor); }
    void setShellReadSource(FileReadSource source) { if(m_id.isEmpty())m_shellReadSource=std::move(source); }
    bool hasShellReadSource() const { return bool(m_shellReadSource); }
    bool hasAttachmentReadSource() const { return bool(m_attachmentReadSource); }
    void execute(const QString &id, const QString &name, const QJsonObject &arguments){executeWithProvenance(id,name,arguments,{});}
    void executeWithProvenance(const QString &id, const QString &name, const QJsonObject &arguments,const QString& mutationProvenanceId);
    void cancel();

signals:
    // Host-created identity for one mutation worker, including a late commit
    // after its ordinary request has been cancelled or replaced.
    void mutationStarted(QString invocationId, QString requestId);
    void mutationSettled(QString invocationId, QString requestId, QString output,
                         bool error, QJsonObject metadata);
    void completed(QString id, QString output, bool error, QJsonObject metadata = {});
    void fileChanged(QString path, QByteArray before, QByteArray after);
    void processOutput(QString id, QString output);

private:
    void finish(const QString &output, bool error = false, bool truncate = true, const QJsonObject &metadata = {});
    QString rootForPath(const QString &requested, QString *relative = nullptr) const;
    void executeFileTool(const QString &name, const QJsonObject &arguments);
    void executeCommand(const QJsonObject &arguments);
    void drainProcess();
    void closeJob();
    QString m_workspace;
    QString m_appsDirectory;
    QString m_id;
    QString m_mutationProvenanceId;
    QProcess *m_process = nullptr;
    QTimer m_processTimeout;
    QByteArray m_processOutput;
    bool m_outputTruncated = false;
    bool m_commandTimedOut = false;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_fileReadCancelled;
    FileReadCommitter m_fileReadCommitter;
    FileReadSource m_attachmentReadSource;
    FileReadSource m_shellReadSource;
    FileMutationExecutor m_fileMutationExecutor;
    void *m_job = nullptr;
};

} // namespace ade
