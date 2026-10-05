project('libblkio', ['c', 'rust'],
  meson_version: '>=0.61.0',
  license : 'MIT OR Apache-2.0',
  version : run_command(['./package-version.py'], check : true).stdout().strip(),
  default_options : ['buildtype=release'],
)

find_program('cargo', required: true)

add_project_arguments(
    '-D_GNU_SOURCE',
    language : 'c',
)

if not (meson.is_subproject() and get_option('subproject-docs').disabled())
  subdir('docs')
endif
subdir('include')
subdir('src') # requires: include

libblkio_dep = declare_dependency(include_directories: inc,
                                  link_with: lib)

if not (meson.is_subproject() and get_option('subproject-examples').disabled())
  subdir('examples') # requires: include, src
endif

if not (meson.is_subproject() and get_option('subproject-tests').disabled())
  subdir('tests') # requires: include, src
endif
