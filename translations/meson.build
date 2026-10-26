# UI translations. Each .ts is compiled to a .qm in the build tree, dropped next to nothing else --
# the GUI looks for them at <prefix>/share/dlssnr/translations (see gui/main.cpp) and falls back to
# the English source strings when the running locale has no .qm.
qt6 = import('qt6')

qt6.compile_translations(
  ts_files: 'dlssnr_zh_CN.ts',
  build_by_default: true,
)
