qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Widgets'])
gui_inc = include_directories('../common', '../layer_linux/src')

gui_sources = files(
  'main.cpp',
  'mainwindow.cpp',
  'passdialog.cpp',
  'shm_binder.cpp',
  '../layer_linux/src/hotkey.cpp',
  '../common/runner_discovery.cpp',
)

gui_qt_sources = qt6.preprocess(
  moc_headers: ['mainwindow.h', 'passdialog.h', 'shm_binder.h'],
  include_directories: gui_inc,
  dependencies: qt6_dep,
)

executable('dlssnr_gui', gui_sources + gui_qt_sources,
  include_directories: gui_inc,
  dependencies: qt6_dep,
  link_args: [
    '-static-libstdc++', '-static-libgcc',
    '-pie', '-Wl,-z,nocopyreloc', '-Wl,-z,indirect-extern-access',
  ],
  install: false,
)
