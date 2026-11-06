#pragma once

#include <QString>
#include <QStringList>

namespace ade {

class AppFonts {
public:
    struct Families {
        QString body;
        QString mono;
        QStringList errors;
        bool fullyLoaded() const { return errors.isEmpty() && body == "Geist" && mono == "Roboto Mono"; }
    };

    // Call after constructing QApplication and before creating widgets. Fonts
    // are registered once for the process and never installed system-wide.
    static Families load();
};

} // namespace ade
