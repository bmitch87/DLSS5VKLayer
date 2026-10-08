#include "mainwindow.h"
#include "translations.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

// The locale comes from DLSSNR_LANG, else the `language=` line the interface writes into its
// configuration, else the session locale (see dlssnr::StartupLocale). Qt's own qtbase translation is
// picked up from the same locale, which is what turns the standard dialogs -- the file chooser, the
// Yes/No buttons -- into the same language as the rest of the window.
static void installTranslations(QApplication& app) {
    const QString wanted = dlssnr::StartupLocale();
    const QLocale locale = wanted.isEmpty() ? QLocale::system() : QLocale(wanted);
    const QString localeName = locale.name();

    // One line on stderr: invisible when the window is started from a menu, and the first thing worth
    // knowing when a localized build still looks English.
    if (locale.language() == QLocale::C || locale.language() == QLocale::English) {
        qInfo("[i18n] locale '%s' uses the built-in English strings", qPrintable(localeName));
        return;
    }

    auto* qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(qtTranslator);
    }

    for (const QString& dir : dlssnr::TranslationDirs()) {
        if (dir.isEmpty() || !QFileInfo::exists(dir)) continue;
        auto* translator = new QTranslator(&app);
        if (translator->load(locale, QStringLiteral("dlssnr"), QStringLiteral("_"), dir)) {
            app.installTranslator(translator);
            qInfo("[i18n] locale '%s': dlssnr translation loaded from %s",
                  qPrintable(localeName), qPrintable(dir));
            return;
        }
        delete translator;
    }
    qInfo("[i18n] locale '%s': no dlssnr translation found; the interface stays in English",
          qPrintable(localeName));
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    installTranslations(app);
    MainWindow w;
    w.show();
    return app.exec();
}
