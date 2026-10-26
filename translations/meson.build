# UI translations. Each .ts is compiled to a .qm in the build tree, dropped next to nothing else --
# the GUI looks for them at <prefix>/share/dlssnr/translations (see gui/main.cpp) and falls back to
# the English source strings when the running locale has no .qm.
qt6 = import('qt6')

# lrelease ships in qt6-l10n-tools (Debian/Ubuntu), qt6-linguist (Fedora) and qt6-tools (Arch),
# none of which a GUI-only build normally pulls in. Translations are an extra, not a requirement:
# without the compiler the interface simply keeps its built-in English strings, so the build must
# not die over them -- same optionality install.sh already gives the packaged .qm files.
lrelease = find_program(
  'lrelease-qt6', 'lrelease', 'lrelease6',
  '/usr/lib/qt6/bin/lrelease',   # Debian/Ubuntu
  '/usr/lib64/qt6/bin/lrelease', # Fedora and friends
  required: false,
)

if lrelease.found()
  qt6.compile_translations(
    ts_files: 'dlssnr_zh_CN.ts',
    build_by_default: true,
  )
else
  warning('lrelease not found: skipping UI translations, the interface stays in English')
endif
