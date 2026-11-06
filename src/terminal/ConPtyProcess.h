#pragma once
#include <QObject>
#include <QSize>
#include <memory>

namespace ade {
// A real Windows pseudoconsole. No QProcess pipe emulation or external runtime.
class ConPtyProcess final : public QObject {
    Q_OBJECT
public:
    explicit ConPtyProcess(QObject *parent=nullptr);
    ~ConPtyProcess() override;
    bool start(const QString &workingDirectory, const QStringList &command={}, QSize cells={100,30}, QString *error=nullptr);
    bool write(const QByteArray &bytes);
    bool resize(QSize cells, QString *error=nullptr);
    void stop();
    bool isRunning() const;
    quint32 processId() const;
    QString workingDirectory() const;
    static QStringList defaultShell();
signals:
    void outputReady(const QByteArray &bytes);
    void exited(quint32 exitCode);
    void errorOccurred(const QString &message);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
