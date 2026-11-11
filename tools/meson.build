common_inc = include_directories('../common')

executable('runner_probe',
  '../common/runner_discovery.cpp',
  'runner_probe.cpp',
  include_directories: common_inc,
  link_args: ['-static', '-static-libstdc++', '-static-libgcc'],
  install: false,
)

executable('dlssnr-shmctl',
  'shmctl.cpp',
  include_directories: common_inc,
  link_args: ['-static', '-static-libstdc++', '-static-libgcc'],
  install: false,
)

# Helper-only preload: close native descriptors exported through Wine.
shared_library('dlssnr_unix_fd_bridge',
  'unix_fd_bridge.c',
  c_args: ['-std=c11'],
  dependencies: dependency('threads'),
  install: false,
)
