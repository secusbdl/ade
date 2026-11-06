#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QWidget>
#include <QPointer>
#include <QUrl>
#include <functional>

class QButtonGroup;
class QLabel;
class QScrollArea;
class QVBoxLayout;
class QPushButton;

namespace ade {

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QString dataDir, QWidget *parent = nullptr);
    QJsonValue preference(const QString &path, const QJsonValue &fallback = {}) const;
    QJsonObject preferences() const { return m_preferences; }
    QString currentSection() const { return m_section; }
    QString storageError() const { return m_storageError; }
    static QJsonObject defaultPreferences();
    bool setPreference(const QString &path, const QJsonValue &value);
    bool addSearchEngine(const QString &name, const QString &keyword, const QString &url);
    bool setSitePermission(const QString &permission, const QUrl &origin, const QString &setting);
    void completeBrowsingDataClear(bool success, const QString &message = {});
    void setContextWorkspace(const QString& workspace){m_contextWorkspace=workspace;}
    void setWorktreePageFactory(std::function<QWidget*(QWidget*)> factory);
    void reportConversationError(const QString& error);
    void reportProviderImportResult(const QString& message, bool error) { showStatus(message,error); }
    void completePluginConnectionCheck(quint64 requestId, const QString& pluginId,
                                       bool success, const QString& safeMessage);

public slots:
    void selectSection(const QString &section);
    void reload();

signals:
    void backRequested();
    void providersRequested();
    void providerEditRequested(const QString& providerId);
    void addProviderRequested();
    void providerKeyImportRequested();
    void preferencesChanged();
    void restoreConversationRequested(const QString &id);
    void deleteConversationRequested(const QString &id);
    void openUrlRequested(const QUrl &url);
    void clearBrowsingDataRequested(const QJsonObject &options);
    void pluginCredentialsChanged();
    void pluginConnectionCheckRequested(quint64 requestId, QString pluginId);
    void pluginConnectionCheckCancelled(quint64 requestId);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildNavigation();
    void buildContent();
    void buildGeneral(QVBoxLayout *layout);
    void buildProviders(QVBoxLayout *layout);
    void buildSkills(QVBoxLayout *layout);
    void buildWorktrees(QVBoxLayout *layout);
    void buildPlugins(QVBoxLayout *layout);
    void invalidatePluginConnectionCheck();
    void buildArchived(QVBoxLayout *layout);
    void buildPersonalization(QVBoxLayout *layout);
    void buildBrowsing(QVBoxLayout *layout);
    void buildClearData(QVBoxLayout *layout);
    void buildHistory(QVBoxLayout *layout);
    void buildAccount(QVBoxLayout *layout);
    void buildAbout(QVBoxLayout *layout);
    void buildUtilityModels(QVBoxLayout *layout, const QString &path, const QString &title);
    void editModelPreset(const QString &id = {});
    bool savePreferences(const QJsonObject &preferences);
    void showStatus(const QString &text, bool error = false);
    void applyAppearance();
    QString m_dataDir;
    QString m_contextWorkspace;
    QString m_section = QStringLiteral("models-providers");
    QString m_plugin;
    quint64 m_pluginViewGeneration = 0, m_nextPluginCheck = 0, m_pluginCheckRequest = 0;
    QString m_pluginCheckId, m_pluginCheckRegion;
    QByteArray m_pluginCheckRevision;
    QPointer<QLabel> m_pluginCheckStatus;
    QPointer<QPushButton> m_pluginCheckButton;
    QString m_storageError;
    QString m_baseStyleSheet;
    bool m_storageValid = true;
    bool m_clearingData = false;
    int m_pluginColumns = 0;
    QString m_archivedQuery,m_archivedSelection;
    int m_archivedLimit = 25;
    bool m_preferencesFileExisted = false;
    QByteArray m_persistedPreferences;
    QJsonObject m_preferences;
    std::function<QWidget*(QWidget*)> m_worktreePageFactory;
    QButtonGroup *m_navigation = nullptr;
    QScrollArea *m_scroll = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace ade
