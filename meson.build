project('dlss5vklayer', 'cpp',
  version: '0.2.5',
  default_options: [
    'cpp_std=c++17',
    'warning_level=3',
    'optimization=2',
  ],
)

add_project_arguments('-Wno-unused-parameter', language: 'cpp')

scan_build = find_program('scan-build', required: false)
if scan_build.found()
  run_target('analyze',
    command: [scan_build, '--status-bugs', 'ninja', '-C', meson.project_build_root()],
  )
endif

host_system = host_machine.system()
host_cpu = host_machine.cpu_family()

if host_system == 'linux' and host_cpu == 'x86_64'
  subdir('layer_linux')
  subdir('tools')
  subdir('gui')
  subdir('test_gui')
elif host_system == 'linux' and host_cpu in ['x86', 'i686']
  subdir('layer_linux')
elif host_system == 'windows' and host_cpu == 'x86_64'
  subdir('windows')
else
  error('unsupported build target: @0@/@1@'.format(host_system, host_cpu))
endif
