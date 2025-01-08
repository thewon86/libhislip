project('libhislip', 'c',
  version : '0.1',
  license : [ 'BSD-3-Clause'],
  meson_version : '>= 0.53.2',
  default_options : [ 'warning_level=2', 'buildtype=release', 'c_std=gnu11', 'optimization=2', 'b_lundef=false' ]
)

# The tag date of the project_version(), update when the version bumps.
version_date='2022-10-20'

# Debug
debug_mode = get_option('debug_mode')

subdir('src')
subdir('man')

build_demo = get_option('build_demo')
if build_demo
  subdir('demo')
endif

pkg_mod = import('pkgconfig')
pkg_mod.generate(libraries : libhislip,
  name : 'libhislip',
  filebase : 'libhislip',
  description : 'A HiSLIP client/server library'
)
