common_inc = include_directories('../common')
dl_dep = meson.get_compiler('cpp').find_library('dl', required: false)
thread_dep = dependency('threads')

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

# The three tools below are committed but were in no build file, so the regen
# scripts' "build glsl2spv first" could not be followed and the two synthetic
# frame drivers had to be compiled by hand. None of them is installed or
# shipped; they are developer tools.

# glsl2spv dlopen()s libshaderc at run time, so unlike the two above it cannot be
# statically linked -- a static binary has no dynamic loader to call.
executable('glsl2spv',
  'glsl2spv.cpp',
  dependencies: [dl_dep],
  install: false,
)

executable('shm_frames',
  'shm_frames.cpp',
  include_directories: common_inc,
  dependencies: [thread_dep],
  install: false,
)

executable('soak_passes',
  'soak_passes.cpp',
  include_directories: common_inc,
  dependencies: [thread_dep],
  install: false,
)
