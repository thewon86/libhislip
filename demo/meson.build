cc = meson.get_compiler('c')
libpthread_dep = cc.find_library('pthread', required : false)

demo_deps = [libpthread_dep]

executable('hs-client',
           'client.c',
            dependencies: demo_deps,
            include_directories: include_directories('../src'),
            link_with: libhislip,
            install: false,
)

executable('hs-server',
           'server.c',
            dependencies: demo_deps,
            include_directories: include_directories('../src'),
            link_with: libhislip,
            install: false,
)
