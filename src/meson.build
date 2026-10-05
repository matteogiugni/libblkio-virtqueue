cargo_build = find_program('cargo-build.sh')

staticlib_name = 'libblkio.a'

staticlib_target = custom_target(staticlib_name,
  build_by_default : true,
  build_always_stale : true,
  command : [cargo_build, get_option('debug').to_string(),
             get_option('optimization'), meson.current_build_dir() / 'target',
             '@OUTPUT@'],
  console : true,
  output : [staticlib_name])

# files('libblkio.lds').full_path() only supported in Meson 1.4.0 or later
symbols = meson.current_source_dir() / 'libblkio.lds'

lib = library('blkio',
  sources: files('error-msg.c'),
  dependencies: meson.get_compiler('c').find_library('m', required: true),
  link_whole: staticlib_target,
  install: true,
  link_depends: files('libblkio.lds'),
  link_args: ['-Wl,-version-script,' + symbols],
  version: meson.project_version())

# generate pkg-config file

import('pkgconfig').generate(libraries : ['-lblkio'],
  name : 'blkio',
  description : 'Block device I/O library')
