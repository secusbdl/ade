#include "MainWindow.h"
#include "AgentActivityMonitor.h"
#include "InstanceCoordinator.h"
#include "NotificationSounds.h"
#include "PowerRequest.h"
#include "SettingsPage.h"
#include "SideChatPane.h"
#include "SoundOutput.h"
#include <QApplication>
#include <QFileDialog>
#include <QGuiApplication>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <utility>

namespace ade {
namespace {
MainWindow::SystemEffects& effectsFactory() {
    static MainWindow::SystemEffects factory;
    return factory;
}
// Bounded like the coordinator's own buffer: 8 requests of 16 URLs.
constexpr qsizetype maxPendingInstanceUrls = InstanceCoordinator::maxBufferedRequests * InstanceCoordinator::maxUrls;
}

void MainWindow::setSystemEffectsFactory(SystemEffects effects) { effectsFactory() = std::move(effects); }

MainWindow::SystemEffects MainWindow::defaultSystemEffects() {
    const auto platform = QGuiApplication::platformName();
    // CTest hosts run real turns in offscreen windows that are never active;
    // they must neither play audio nor take power requests.
    const bool silent = platform == "offscreen" || platform == "minimal"
        || qEnvironmentVariable("ADE_DISABLE_SYSTEM_EFFECTS") == "1";
    if (silent)
        return {[] { return std::unique_ptr<ISoundOutput>(std::make_unique<NullSoundOutput>()); },
                [] { return std::unique_ptr<IPowerBackend>(std::make_unique<NullPowerBackend>()); }};
    return {[] { return std::unique_ptr<ISoundOutput>(std::make_unique<WinSoundOutput>()); },
            [] { return std::unique_ptr<IPowerBackend>(std::make_unique<WinPowerBackend>()); }};
}

void MainWindow::initializeRuntimeEffects() {
    // Runs before buildUi(): engineFor() and applyPreferences() can be reached
    // during startup selection and must find these services.
    const auto& configured = effectsFactory();
    const auto fallback = defaultSystemEffects();
    m_soundOutput = configured.sound ? configured.sound() : fallback.sound();
    if (!m_soundOutput) m_soundOutput = std::make_unique<NullSoundOutput>();
    auto power = configured.power ? configured.power() : fallback.power();
    if (!power) power = std::make_unique<NullPowerBackend>();
    m_power = std::make_unique<PowerRequestGuard>(std::move(power));
    m_sounds = new NotificationSoundService(m_store.directory() + "/imported-sound-packs", m_soundOutput.get(), {}, this);
    auto* activity = new AgentActivityMonitor(this);
    m_activity = activity;
    connect(activity, &AgentActivityMonitor::attention, this, [this](const QString& id, int event) {
        if (!m_sounds) return;
        const bool focused = isActiveWindow() && !isMinimized();
        const auto side = m_sideChats.value(id);
        const bool visible = id == m_currentId || (side && side->isVisible());
        m_sounds->notify(static_cast<AttentionEvent>(event), id, focused, visible);
    });
    connect(activity, &AgentActivityMonitor::activeCountChanged, this, [this](int active) {
        if (m_power) m_power->update(m_settings ? m_settings->preference("agent.keepAwake").toBool(true) : true, active);
    });
}

void MainWindow::connectSoundSettings() {
    SettingsPage::SoundHooks hooks;
    hooks.packs = [this] {
        QList<QPair<QString, QString>> packs;
        if (m_sounds)
            for (const auto& pack : m_sounds->packs()) packs.append({pack.id, pack.name});
        return packs;
    };
    hooks.preview = [this](const QString& pack, const QString& loudness) { return m_sounds && m_sounds->preview(pack, loudness); };
    hooks.chooseAndImport = [this] {
        SettingsPage::SoundImportOutcome outcome;
        const QPointer<MainWindow> self(this);
        const auto path = QFileDialog::getOpenFileName(this, "Use custom sound", QString(),
            "Sound files (*.mp3 *.wav *.json);;MP3 audio (*.mp3);;WAV audio (*.wav);;Sound pack JSON (*.json)");
        if (!self || path.isEmpty() || !m_sounds) {
            outcome.cancelled = true;
            return outcome;
        }
        const auto result = m_sounds->importFile(path);
        outcome.id = result.id;
        outcome.name = result.name;
        outcome.error = result.error;
        return outcome;
    };
    m_settings->setSoundHooks(std::move(hooks));
}

void MainWindow::handleInstanceRequest(const InstanceRequest& request) {
    // An accepted close is terminal: never re-show a closing window. The
    // coordinator answers later launches "closing" once the window is gone.
    if (m_closeAccepted) return;
    const QPointer<MainWindow> self(this);
    if (isMinimized()) setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    show();
    raise();
    activateWindow();
    if (!self) return;
    if (const QPointer<QMessageBox> dialog = m_sessionRecoveryDialog) {
        // A pending recovery decision owns the window; its URLs wait for it.
        dialog->raise();
        if (dialog) dialog->activateWindow();
    }
    if (!self) return;
    queueInstanceUrls(request.urls);
    // Windows may refuse foreground activation; flash the taskbar instead.
    if (self && !m_closeAccepted && !isActiveWindow()) QApplication::alert(this);
}

void MainWindow::openStartupUrls(const QList<QUrl>& urls) { queueInstanceUrls(urls); }

void MainWindow::queueInstanceUrls(const QList<QUrl>& urls) {
    if (m_closeAccepted) return;
    for (const auto& url : urls) {
        if (m_pendingInstanceUrls.size() >= maxPendingInstanceUrls) break;
        m_pendingInstanceUrls.append(url);
    }
    openPendingInstanceUrls();
}

void MainWindow::openPendingInstanceUrls() {
    if (m_closeAccepted) {
        m_pendingInstanceUrls.clear();
        return;
    }
    if (m_pendingInstanceUrls.isEmpty()) return;
    if (const QPointer<QMessageBox> dialog = m_sessionRecoveryDialog) {
        // Recovery dialogs delete themselves on close; retry after that, so a
        // close chosen in the dialog runs first and drops these URLs.
        connect(dialog, &QObject::destroyed, this, [this] { QTimer::singleShot(0, this, &MainWindow::openPendingInstanceUrls); });
        m_status->setText("Links from another launch will open after this decision.");
        return;
    }
    if (m_sessionTransition) {
        // A reload or close attempt is in progress; it ends shortly.
        QTimer::singleShot(100, this, &MainWindow::openPendingInstanceUrls);
        return;
    }
    const QPointer<MainWindow> self(this);
    const auto urls = std::exchange(m_pendingInstanceUrls, {});
    for (const auto& url : urls) {
        if (!self || m_closeAccepted) return;
        m_pages->setCurrentIndex(0);
        addBrowserTab(url);
    }
}

} // namespace ade
