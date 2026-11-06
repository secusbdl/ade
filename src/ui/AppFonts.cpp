#include "AppFonts.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QResource>

// Q_INIT_RESOURCE must be outside namespace ade so the generated resource
// symbol resolves when this source is linked into an executable or a library.
static void initializeAppFontResources() {
    Q_INIT_RESOURCE(resources);
}

namespace ade {

AppFonts::Families AppFonts::load() {
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return {{}, {}, {QStringLiteral("Create QApplication before loading application fonts.")}};

    static const Families families = [] {
        initializeAppFontResources();
        Families result;
        result.body = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
        result.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        struct FontAsset { const char *path; const char *family; };
        const FontAsset assets[] = {
            {":/fonts/Geist-Variable.ttf", "Geist"},
            {":/fonts/Geist-Italic-Variable.ttf", "Geist"},
            {":/fonts/RobotoMono-Variable.ttf", "Roboto Mono"},
            {":/fonts/RobotoMono-Italic-Variable.ttf", "Roboto Mono"}
        };
        bool bodyLoaded = true;
        bool monoLoaded = true;
        for (const auto &asset : assets) {
            const auto path = QString::fromLatin1(asset.path);
            const auto expected = QString::fromLatin1(asset.family);
            const int id = QFontDatabase::addApplicationFont(path);
            const bool loaded = id >= 0 && QFontDatabase::applicationFontFamilies(id).contains(expected);
            if (!loaded) {
                if (id >= 0) QFontDatabase::removeApplicationFont(id);
                result.errors.append(QStringLiteral("Could not register %1 from %2.").arg(expected,path));
            }
            if (expected == "Geist") bodyLoaded = bodyLoaded && loaded;
            else monoLoaded = monoLoaded && loaded;
        }
        if (bodyLoaded) result.body = QStringLiteral("Geist");
        if (monoLoaded) result.mono = QStringLiteral("Roboto Mono");
        return result;
    }();
    return families;
}

} // namespace ade
