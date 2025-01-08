mandir = join_paths(get_option('prefix'), get_option('mandir'))
man3dir = join_paths(mandir, 'man3')

conf = configuration_data()
conf.set('version', meson.project_version())
conf.set('version_date', version_date)

manpage_hs_init = configure_file(
     input: files('hs_init.3.in'),
     output: 'hs_init.3',
     configuration: conf,
)

manpages = [
            manpage_hs_init,
            ]

install_man(
     manpages,
     install_dir: man3dir,
)
