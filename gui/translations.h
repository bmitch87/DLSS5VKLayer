#pragma once
#include <QString>
#include <QStringList>

// Where the interface's compiled translations live, what languages are installed, and which one the
// interface should start in. The GUI and main() share this so the language shown, the language
// offered in the menu, and the language the .qm search finds can never disagree.
namespace dlssnr {

// The configuration file the interface reads and writes (~/.config/dlssnr/config.ini).
QString ConfigFilePath();

// Folders a compiled dlssnr_<locale>.qm can live in, tried in this order.
QStringList TranslationDirs();

// Installed translation locales ("zh_CN"), one per dlssnr_<locale>.qm found, sorted and deduplicated.
// English is not in here: its strings are the built-in ones.
QStringList AvailableTranslationLocales();

// The `language=` value from the configuration. Empty means "follow the system", which is also what
// an absent line means.
QString ConfiguredLanguage();

// The locale the interface should start in: DLSSNR_LANG, else the configured language, else the
// session locale. An empty result means the built-in English strings.
QString StartupLocale();

}  // namespace dlssnr
