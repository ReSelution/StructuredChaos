project(
  'entt',
  'cpp',
  version: '4.0.0',
  license: 'MIT',
  default_options: ['cpp_std=c++20'],
  meson_version: '>=1.1.0',
)

compiler = meson.get_compiler('cpp')

entt_args = []
entt_link_args = []

if get_option('use_libcpp')
  if compiler.has_argument('-stdlib=libc++') and compiler.has_link_argument('-stdlib=libc++')
    entt_args += '-stdlib=libc++'
    entt_link_args += '-stdlib=libc++'
  else
    warning('use_libcpp is set but libc++ is not available')
  endif
endif

# Options that map one-to-one onto a define of the same name
foreach opt : [
  'use_atomic',
  'no_exception',
  'disable_assert',
  'no_eto',
  'no_mixin',
  'standard_cpp',
  'use_stl',
]
  if get_option(opt)
    entt_args += '-DENTT_' + opt.to_upper()
  endif
endforeach

# Only pass values that differ from the defaults in config.h
if get_option('id_type') != ''
  entt_args += '-DENTT_ID_TYPE=' + get_option('id_type')
endif
if get_option('sparse_page') != 4096
  entt_args += '-DENTT_SPARSE_PAGE=@0@'.format(get_option('sparse_page'))
endif
if get_option('packed_page') != 1024
  entt_args += '-DENTT_PACKED_PAGE=@0@'.format(get_option('packed_page'))
endif

entt_natvis_dir = 'src' / 'entt' / 'natvis'
entt_natvis = []
foreach name : [
  'config',
  'container',
  'core',
  'entity',
  'graph',
  'locator',
  'meta',
  'poly',
  'process',
  'resource',
  'signal',
]
  entt_natvis += entt_natvis_dir / name + '.natvis'
endforeach

if get_option('include_natvis')
  if compiler.get_argument_syntax() == 'msvc'
    foreach f : entt_natvis
      entt_link_args += '/NATVIS:' + meson.current_source_dir() / f
    endforeach
  else
    warning('include_natvis is set but natvis files are not supported by this compiler')
  endif
endif

entt_dep = declare_dependency(
  include_directories: include_directories('src'),
  compile_args: entt_args,
  link_args: entt_link_args,
  version: meson.project_version(),
)

meson.override_dependency('entt', entt_dep)

if not meson.is_subproject()
  install_subdir(
    'src/entt',
    install_dir: get_option('includedir'),
    exclude_directories: ['natvis'],
  )
  install_data(files(entt_natvis), install_dir: get_option('includedir') / 'entt' / 'natvis')

  import('pkgconfig').generate(
    name: 'entt',
    description: 'Gaming meets modern C++ - a fast and reliable entity-component system (ECS) and much more',
    url: 'https://github.com/skypjack/entt',
    extra_cflags: entt_args,
  )
endif
