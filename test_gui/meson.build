qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Widgets'])
test_inc = include_directories('../common', '../gui')

test_qt_sources = qt6.preprocess(
  moc_headers: ['../gui/shm_binder.h'],
  include_directories: test_inc,
  dependencies: qt6_dep,
)

binder_test = executable('binder_test',
  'binder_test.cpp',
  '../gui/shm_binder.cpp',
  test_qt_sources,
  include_directories: test_inc,
  dependencies: qt6_dep,
  link_args: [
    '-static-libstdc++', '-static-libgcc',
    '-pie', '-Wl,-z,nocopyreloc', '-Wl,-z,indirect-extern-access',
  ],
  install: false,
)

test('binder_test', binder_test,
  env: ['QT_QPA_PLATFORM=offscreen'],
)
