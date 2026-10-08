project(
  'simdutf',
  'cpp',
  version: '9.2.1',
  license: 'Apache-2.0 OR MIT',
  default_options: ['cpp_std=c++17'],
  meson_version: '>=1.1.0',
)

# ABI version, independent of the project version (SIMDUTF_LIB_VERSION in CMake)
simdutf_lib_version = '36.0.0'
simdutf_lib_soversion = '36'

compiler = meson.get_compiler('cpp')
compiler_id = compiler.get_id()
is_windows = host_machine.system() == 'windows'
install_lib = not meson.is_subproject()

# Defines that consumers of the headers must see as well
simdutf_public_args = []
# Defines and flags for the library itself only
simdutf_private_args = []

if get_option('logging')
  simdutf_public_args += '-DSIMDUTF_LOGGING=1'
endif
if get_option('static_initialization')
  simdutf_public_args += '-DSIMDUTF_USE_STATIC_INITIALIZATION=1'
endif

# Features are on by default, only disabling one needs a define
foreach feature : [
  'utf8',
  'utf16',
  'utf32',
  'ascii',
  'latin1',
  'base64',
  'detect_encoding',
]
  if not get_option('feature_' + feature)
    simdutf_public_args += '-DSIMDUTF_FEATURE_@0@=0'.format(feature.to_upper())
  endif
endforeach

foreach impl : [
  'icelake',
  'haswell',
  'westmere',
  'arm64',
  'ppc64',
  'rvv',
  'lsx',
  'lasx',
  'fallback',
]
  choice = get_option('impl_' + impl)
  if choice != 'auto'
    simdutf_private_args += '-DSIMDUTF_IMPLEMENTATION_@0@=@1@'.format(
      impl.to_upper(),
      choice == 'enabled' ? 1 : 0,
    )
  endif
endforeach

# Workaround for GCC's poor code generation for unaligned AVX loads/stores
if compiler_id == 'gcc' and host_machine.cpu_family() in ['x86', 'x86_64']
  simdutf_private_args += [
    '-mno-avx256-split-unaligned-load',
    '-mno-avx256-split-unaligned-store',
  ]
endif

if compiler_id == 'gcc' and host_machine.cpu_family() in ['ppc', 'ppc64']
  simdutf_private_args += ['-maltivec', '-mvsx', '-mcpu=power8']
endif

simdutf_dll_build_args = []
simdutf_dll_use_args = []
if is_windows and get_option('default_library') == 'shared'
  simdutf_dll_build_args += '-DSIMDUTF_BUILDING_WINDOWS_DYNAMIC_LIBRARY=1'
  simdutf_dll_use_args += '-DSIMDUTF_USING_WINDOWS_DYNAMIC_LIBRARY=1'
endif

simdutf_inc = include_directories('include')
simdutf_src_inc = include_directories('src')
# simdutf.cpp includes all other sources itself (unity build)
simdutf_sources = files('src/simdutf.cpp')

simdutf_lib = library(
  'simdutf',
  simdutf_sources,
  include_directories: [simdutf_inc, simdutf_src_inc],
  cpp_args: simdutf_public_args + simdutf_private_args + simdutf_dll_build_args,
  version: simdutf_lib_version,
  soversion: simdutf_lib_soversion,
  pic: true,
  install: install_lib,
)

simdutf_dep = declare_dependency(
  include_directories: simdutf_inc,
  compile_args: simdutf_public_args + simdutf_dll_use_args,
  link_with: simdutf_lib,
  version: meson.project_version(),
)

meson.override_dependency('simdutf', simdutf_dep)

# Variant without a C++ runtime dependency, meant for the C interface (simdutf_c.h)
if get_option('nostdlibcxx')
  if not (compiler.has_multi_arguments('-fno-exceptions', '-fno-rtti')
  and compiler.has_link_argument('-nostdlib++'))
    error('nostdlibcxx requires -fno-exceptions, -fno-rtti and -nostdlib++ (GCC or Clang)')
  endif

  simdutf_nostdlibcxx_lib = static_library(
    'simdutf-nostdlibcxx',
    simdutf_sources,
    include_directories: [simdutf_inc, simdutf_src_inc],
    cpp_args: simdutf_public_args
    + simdutf_private_args
    + ['-DSIMDUTF_NO_LIBCXX=1', '-fno-exceptions', '-fno-rtti'],
    pic: true,
  )

  simdutf_nostdlibcxx_dep = declare_dependency(
    include_directories: simdutf_inc,
    compile_args: simdutf_public_args,
    link_with: simdutf_nostdlibcxx_lib,
    version: meson.project_version(),
  )

  meson.override_dependency('simdutf-nostdlibcxx', simdutf_nostdlibcxx_dep)
endif

if install_lib
  install_headers('include/simdutf.h', 'include/simdutf_c.h')
  install_subdir('include/simdutf', install_dir: get_option('includedir'))

  import('pkgconfig').generate(
    simdutf_lib,
    name: 'simdutf',
    description: 'Fast Unicode validation, transcoding and processing',
    url: 'https://github.com/simdutf/simdutf',
    extra_cflags: simdutf_public_args + simdutf_dll_use_args,
  )
endif
