#include "translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace dlssnr {

QString ConfigFilePath() {
    const QString base = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config");
    return base + "/dlssnr/config.ini";
}

QStringList TranslationDirs() {
    QStringList dirs;
    const QString env = qEnvironmentVariable("DLSSNR_TRANSLATIONS");
    if (!env.isEmpty()) dirs << env;
    const QString appDir = QCoreApplication::applicationDirPath();
    dirs << appDir                                    // a build tree, .qm beside the binary
         << appDir + "/../translations"               // a build tree, .qm in the build dir
         << appDir + "/../share/dlssnr/translations"  // a user or system install
         << QDir::homePath() + "/.local/share/dlssnr/translations"
         << QStringLiteral("/usr/local/share/dlssnr/translations")
         << QStringLiteral("/usr/share/dlssnr/translations");
    return dirs;
}

QStringList AvailableTranslationLocales() {
    QStringList locales;
    for (const QString& dir : TranslationDirs()) {
        if (dir.isEmpty()) continue;
        const QDir folder(dir);
        const QStringList files = folder.entryList({QStringLiteral("dlssnr_*.qm")}, QDir::Files);
        for (const QString& file : files) {
            // dlssnr_<locale>.qm
            const QString locale = file.mid(7, file.size() - 7 - 3);
            if (!locale.isEmpty() && !locales.contains(locale)) locales << locale;
        }
    }
    locales.sort();
    return locales;
}

QString ConfiguredLanguage() {
    QFile f(ConfigFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.startsWith(QLatin1String("language="))) continue;
        QString value = line.section('=', 1).trimmed();
        if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
            value = value.mid(1, value.size() - 2);
        return value;
    }
    return QString();
}

QString StartupLocale() {
    // The environment wins over the stored configuration, the order DLSSNR_SHM and DLSSNR_LOG use:
    // it is what makes a one-off `DLSSNR_LANG=en dlssnr-gui` possible when the interface is set to
    // another language.
    const QString forced = qEnvironmentVariable("DLSSNR_LANG").trimmed();
    if (!forced.isEmpty()) return forced;

    const QString configured = ConfiguredLanguage();
    if (!configured.isEmpty()) return configured;

    QString env = qEnvironmentVariable("LC_ALL");
    if (env.isEmpty()) env = qEnvironmentVariable("LC_MESSAGES");
    if (env.isEmpty()) env = qEnvironmentVariable("LANG");
    return env.section('.', 0, 0).section('@', 0, 0);  // zh_CN.UTF-8 -> zh_CN
}

}  // namespace dlssnr
