layer_sources = files(
  'src/layer.cpp',
  'src/shader_vk.cpp',
  'src/dlssnr_pass.cpp',
  'src/composition.cpp',
  'src/capture.cpp',
  'src/scaler_vk.cpp',
  'src/hotkey.cpp',
)

layer_inc = include_directories('src', '../standalone_runner/third_party')
layer_args = []
if host_machine.cpu_family() in ['x86', 'i686']
  if not get_option('layer32')
    error('the 32-bit layer is required for this build target')
  endif
  layer_args += ['-DDLSSNR_LAYER_32']
endif

shared_library('VkLayer_NV_dlssnr', layer_sources,
  name_prefix: 'lib',
  include_directories: layer_inc,
  c_args: layer_args + ['-fPIC'],
  cpp_args: layer_args + ['-fPIC'],
  link_args: [
    '-static-libstdc++',
    '-static-libgcc',
    '-Wl,--version-script=' + meson.current_source_dir() / 'dlssnr.map',
    '-Wl,-Bsymbolic',
  ],
  dependencies: dependency('threads'),
  install: false,
)
