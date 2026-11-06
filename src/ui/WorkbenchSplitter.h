#pragma once
#include <QResizeEvent>
#include <QSettings>
#include <QSplitter>
#include <QTimer>

namespace ade {
// Visibility is independent of tab lifetime. Percentages describe the whole
// workbench, matching the source layout's content/file-tree preferences.
class WorkbenchSplitter final : public QSplitter {
    Q_OBJECT
  public:
    explicit WorkbenchSplitter(QString settingsFile, QWidget* parent = nullptr)
        : QSplitter(Qt::Horizontal, parent), m_settingsFile(std::move(settingsFile)) {
        setObjectName("workspaceSplitter");
        setHandleWidth(2);
        setChildrenCollapsible(false);
        QSettings settings(m_settingsFile, QSettings::IniFormat);
        m_collapsed = settings.value("contentCollapsed", false).toBool();
        m_filesVisible = settings.value("fileTreeVisible", false).toBool();
        m_contentPercent = readPercent(settings, "contentPanelPercent", 70, 5, 80);
        m_filesPercent = readPercent(settings, "fileTreePanelPercent", 15, 15, 45);
        m_hasSavedPercent = settings.contains("contentPanelPercent");
        connect(this, &QSplitter::splitterMoved, this, [this] {
            if (m_applying || count() != 3)
                return;
            const auto values = sizes();
            const double available = values[0] + values[1] + values[2];
            if (available <= 0)
                return;
            if (contentVisible())
                m_contentPercent = qBound(5.0, 100 * values[1] / available, 80.0);
            if (m_filesVisible)
                m_filesPercent = qBound(15.0, 100 * values[2] / available, 45.0);
            persist();
        });
    }
    void initialize(QWidget* chat, QWidget* content, QWidget* files) {
        addWidget(chat);
        addWidget(content);
        addWidget(files);
        for (auto* child : {chat, content, files}) {
            child->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            child->setMinimumSize(0, 0);
        }
        apply();
    }
    bool contentVisible() const {
        return m_hasContent && !m_collapsed;
    }
    bool contentCollapsed() const {
        return m_collapsed;
    }
    bool fileTreeVisible() const {
        return m_filesVisible;
    }
    bool fileTreeExposed() const {
        return m_filesVisible && !exclusiveContent() && !m_fullscreen;
    }
    bool exclusiveContent() const {
        return contentVisible() && width() < qRound(520 * m_scale);
    }
    double contentPercent() const {
        return m_contentPercent;
    }
    void savePreferences() const {
        persist();
    }
    void setWorkbenchFullscreen(bool enabled) {
        m_fullscreen = enabled;
        apply();
    }
    void setUiScale(double scale) {
        m_scale = qBound(.7, scale, 1.3);
        apply();
    }
    void restoreLegacyState(const QByteArray& state) {
        if (m_hasSavedPercent || state.isEmpty() || count() != 3)
            return;
        // The old layout had two panes. Read its split without creating a tab.
        QSplitter probe;
        QWidget first, second;
        probe.addWidget(&first);
        probe.addWidget(&second);
        probe.resize(1000, 500);
        if (probe.restoreState(state)) {
            const auto values = probe.sizes();
            if (values.size() == 2 && values[0] + values[1] > 0)
                m_contentPercent = qBound(5.0, 100.0 * values[1] / (values[0] + values[1]), 80.0);
        }
        apply();
    }
    QSize minimumSizeHint() const override {
        return {0, 0};
    }
  public slots:
    void setHasContent(bool hasContent) {
        if (m_hasContent == hasContent)
            return;
        m_hasContent = hasContent;
        apply();
        emit visibilityChanged();
    }
    void showContent() {
        setContentCollapsed(false);
    }
    void toggleContent() {
        if (m_hasContent)
            setContentCollapsed(!m_collapsed);
    }
    void setContentCollapsed(bool collapsed) {
        if (m_collapsed == collapsed)
            return;
        m_collapsed = collapsed;
        apply();
        persist();
        emit visibilityChanged();
    }
    void toggleFileTree() {
        setFileTreeVisible(exclusiveContent() || !m_filesVisible);
    }
    void setFileTreeVisible(bool visible) {
        const bool leaveContent = visible && exclusiveContent();
        if (m_filesVisible == visible && !leaveContent)
            return;
        if (leaveContent)
            m_collapsed = true;
        m_filesVisible = visible;
        apply();
        persist();
        emit visibilityChanged();
    }
  signals:
    void visibilityChanged();

  protected:
    void resizeEvent(QResizeEvent* event) override {
        const bool wasExclusive = contentVisible() && event->oldSize().width() < qRound(520 * m_scale);
        QSplitter::resizeEvent(event);
        apply();
        if (wasExclusive != exclusiveContent())
            emit visibilityChanged();
    }

  private:
    static double readPercent(QSettings& settings, const QString& key, double fallback, double low,
                              double high) {
        bool ok = false;
        const auto number = settings.value(key, fallback).toDouble(&ok);
        return ok && qIsFinite(number) ? qBound(low, number, high) : fallback;
    }
    void apply() {
        if (count() != 3 || m_applying)
            return;
        m_applying = true;
        widget(0)->setVisible(!exclusiveContent() && !m_fullscreen);
        widget(1)->setVisible(contentVisible());
        widget(2)->setVisible(fileTreeExposed());
        if (m_fullscreen || exclusiveContent()) {
            setSizes({0, width(), 0});
            m_applying = false;
            return;
        }
        const double files = m_filesVisible ? m_filesPercent : 0,
                     content = contentVisible() ? m_contentPercent : 0;
        const double chat = qMax(20.0, 100 - files - content), total = chat + files + content;
        const int available = qMax(1, width() - handleWidth() * int(contentVisible() + m_filesVisible));
        // Native controls must remain reachable at the source's 350px minimum.
        // At narrow widths panes may be smaller than the desktop preferred split.
        const int auxiliaryMinimum = qRound(96 * m_scale) * int(contentVisible() + m_filesVisible);
        widget(0)->setMinimumWidth(
            qMin(qMax(qRound(240 * m_scale), available / 5), qMax(1, available - auxiliaryMinimum)));
        widget(1)->setMinimumWidth(contentVisible() ? qMin(qRound(96 * m_scale), qMax(1, available / 4)) : 0);
        widget(2)->setMinimumWidth(m_filesVisible ? qMin(qRound(96 * m_scale), qMax(1, available / 4)) : 0);
        setSizes({qRound(available * chat / total), qRound(available * content / total),
                  qRound(available * files / total)});
        m_applying = false;
    }
    void persist() const {
        QSettings settings(m_settingsFile, QSettings::IniFormat);
        settings.setValue("contentCollapsed", m_collapsed);
        settings.setValue("fileTreeVisible", m_filesVisible);
        settings.setValue("contentPanelPercent", m_contentPercent);
        settings.setValue("fileTreePanelPercent", m_filesPercent);
    }
    QString m_settingsFile;
    double m_contentPercent = 70, m_filesPercent = 15, m_scale = 1;
    bool m_hasContent = false, m_collapsed = false, m_filesVisible = false, m_applying = false,
         m_hasSavedPercent = false, m_fullscreen = false;
};
} // namespace ade
