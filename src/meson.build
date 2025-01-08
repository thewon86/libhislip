config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set10('DEBUG', debug_mode)
configure_file(output: 'config.h', configuration: config_h)


libhislip_sources = [
  'client.c',
  'message.c',
  'server.c',
  'session.c',
  'tcp.c',
]

libhislip_deps = [
  dependency('threads', required: true),
]

libhislip_c_args = [
  '-fvisibility=hidden',
  '-D_GNU_SOURCE',
]

libhislip = shared_library(
  'hislip',
  libhislip_sources,
  dependencies: libhislip_deps,
  install: true,
  c_args: libhislip_c_args,
  link_args: ['-fvisibility=hidden', '-Wl,-init,init'],
  version: '1.0.0',
)

install_headers(['hislip/client.h','hislip/server.h', 'hislip/common.h'], subdir: 'hislip')
