#pragma once
#include "GitService.h"
#include <QObject>
#include <QPointer>
#include <functional>
#include <array>

namespace ade {
struct WorktreeScriptDraft {
    QString text, baseline, path, error;
    GitScriptRevision revision;
    bool conflict=false;
    bool dirty() const { return text!=baseline; }
};
class WorktreeSettingsController final : public QObject {
    Q_OBJECT
public:
    explicit WorktreeSettingsController(GitService *sharedService,QObject *parent=nullptr);
    ~WorktreeSettingsController() override;
    void setMutationGuard(std::function<bool(const QString &,GitOperation)> guard);
    void refresh();
    QStringList repositoryIds() const;
    GitSetupRepository repository(const QString &id) const;
    bool repositoryAvailable(const QString &id) const;
    QString selectedRepository() const { return m_selected; }
    GitSetupVariant selectedVariant() const { return m_variant; }
    void selectRepository(const QString &id);
    void selectVariant(GitSetupVariant variant);
    WorktreeScriptDraft draft(const QString &id,GitSetupVariant variant) const;
    void setDraft(const QString &id,GitSetupVariant variant,const QString &text);
    void resetDraft(const QString &id,GitSetupVariant variant);
    bool hasUnsavedDrafts() const;
    void discardDrafts();
    bool isLoading() const { return m_refreshId!=0; }
    bool isMutating() const { return m_mutationId!=0||m_authorizing; }
    bool canSave(const QString &id,GitSetupVariant variant) const;
    bool canRemove(const QString &id,const QString &path) const;
    QString message() const { return m_message; }
    quint64 repositoryRevision() const { return m_repositoryRevision; }
    quint64 save(const QString &id,GitSetupVariant variant);
    quint64 remove(const QString &id,const QString &path);
    static QString scriptTemplate(GitSetupVariant variant);
signals:
    void changed();
    void mutationStarted(const QString &path,ade::GitOperation operation);
    void mutationFinished(const QString &path,ade::GitOperation operation,bool success);
    void worktreeRemovalStarted(const QString &path);
    void worktreeRemovalFinished(const QString &path,bool success);
private:
    struct Repository { GitSetupRepository info;std::array<WorktreeScriptDraft,2> drafts;bool available=false; };
    struct Mutation { QString repository,path,submitted;GitSetupVariant variant=GitSetupVariant::PowerShell;GitOperation operation=GitOperation::SaveSetupScript; };
    QPointer<GitService> m_service;
    QMap<QString,Repository> m_repositories;
    QStringList m_order;
    QString m_selected,m_message;
#ifdef Q_OS_WIN
    GitSetupVariant m_variant=GitSetupVariant::PowerShell;
#else
    GitSetupVariant m_variant=GitSetupVariant::Posix;
#endif
    quint64 m_refreshId=0,m_mutationId=0,m_generation=0,m_refreshGeneration=0,m_repositoryRevision=0;
    bool m_refreshAgain=false,m_authorizing=false;
    Mutation m_mutation;
    std::function<bool(const QString &,GitOperation)> m_guard;
    void complete(quint64 id,const GitResult &result);
    void acceptRepositories(const QList<GitSetupRepository> &repositories);
    void finishMutation(bool success);
    bool authorize(const QString &path,GitOperation operation);
};
}
