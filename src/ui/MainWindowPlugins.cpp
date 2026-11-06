#include "MainWindow.h"
#include "SettingsPage.h"
#include "plugins/PluginConnectionService.h"
#include <QFileDialog>

namespace ade {
void MainWindow::initializePluginConnections() {
    auto* service = new PluginConnectionService(this);
    connect(m_settings, &SettingsPage::pluginConnectionCheckRequested, service,
            [this, service](quint64 requestId, const QString& pluginId) {
        service->checkSaved(requestId, pluginId, m_store.directory(),
                           m_settings->preference("plugins.posthog.region", "us").toString());
    });
    connect(m_settings, &SettingsPage::pluginConnectionCheckCancelled, service, &PluginConnectionService::cancel);
    connect(service, &PluginConnectionService::finished, m_settings, &SettingsPage::completePluginConnectionCheck);
    connect(m_settings, &SettingsPage::providerKeyImportRequested, this, [this] {
        const QPointer<MainWindow> self(this);
        const auto path = QFileDialog::getOpenFileName(this, "Import API key file", {},
            "Key files (*.env *.txt *.md);;All files (*)");
        if (!self || path.isEmpty()) return;
        importKeys(path);
    });
}
}
