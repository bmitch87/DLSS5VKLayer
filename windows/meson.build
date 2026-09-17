windows_inc = include_directories('../standalone_runner/third_party', '../core')
windows_cpp_link_args = [
  '-static',
  '-nostdlib++',
  '-Wl,--start-group',
  '-lstdc++',
  '-lwinpthread',
  '-Wl,--end-group',
  '-pthread',
  # version.dll: the helper reads the NGX model's own version resource, which is where
  # NVIDIA records the driver and architecture that build expects.
  '-lversion',
]

executable('dlssnr_helper',
  '../helper/main.cpp',
  '../core/ngx_snippet.cpp',
  '../core/guard.cpp',
  include_directories: windows_inc,
  link_args: windows_cpp_link_args,
  install: false,
  gui_app: true
)

executable('smoke', '../test_layer/smoke.cpp',
  include_directories: windows_inc,
  link_args: windows_cpp_link_args,
  install: false,
)

# Keep a descriptive alias for the Windows cross build's analyzer target.
if scan_build.found()
  run_target('analyze-windows',
    command: [scan_build, '--status-bugs', 'ninja', '-C', meson.project_build_root()],
  )
endif
