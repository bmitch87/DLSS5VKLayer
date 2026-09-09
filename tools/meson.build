common_inc = include_directories('../common')

executable('runner_probe',
  '../common/runner_discovery.cpp',
  'runner_probe.cpp',
  include_directories: common_inc,
  install: false,
)

executable('dlssnr-shmctl',
  'shmctl.cpp',
  include_directories: common_inc,
  install: false,
)
