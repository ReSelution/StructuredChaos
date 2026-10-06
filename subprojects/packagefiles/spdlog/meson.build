project(
  'spdlog',
  'cpp',
  version: '1.17.0',
  license: 'MIT',
  default_options: ['cpp_std=c++11'],
  meson_version: '>=1.1.0',
)

compiler = meson.get_compiler('cpp')
is_windows = host_machine.system() == 'windows'
is_msvc_like = compiler.get_argument_syntax() == 'msvc'
install_lib = not meson.is_subproject()

fmt_backend = get_option('fmt')
fmt_bundled = fmt_backend == 'bundled'
build_shared = get_option('default_library') == 'shared'

# Defines and flags that consumers must see as well
spdlog_public_args = []
# Defines and flags for the compiled library only
spdlog_private_args = []
spdlog_deps = [dependency('threads')]
spdlog_overrides = []

if fmt_backend == 'std'
  spdlog_public_args += '-DSPDLOG_USE_STD_FORMAT'
  spdlog_overrides += 'cpp_std=c++20'
elif fmt_backend == 'external'
  spdlog_public_args += '-DSPDLOG_FMT_EXTERNAL'
  spdlog_deps += dependency('fmt')
elif fmt_backend == 'external_header_only'
  spdlog_public_args += ['-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY']
  spdlog_deps += dependency('fmt').partial_dependency(compile_args: true, includes: true)
endif

# Options that map one-to-one onto a define of the same name
foreach opt : [
  'no_exceptions',
  'no_tz_offset',
  'prevent_child_fd',
  'no_thread_id',
  'no_tls',
  'no_atomic_levels',
  'disable_default_logger',
]
  if get_option(opt)
    spdlog_public_args += '-DSPDLOG_' + opt.to_upper()
  endif
endforeach

if is_windows
  if get_option('wchar_support')
    spdlog_public_args += '-DSPDLOG_WCHAR_TO_UTF8_SUPPORT'
  endif
  if get_option('wchar_console')
    spdlog_public_args += '-DSPDLOG_UTF8_TO_WCHAR_CONSOLE'
  endif
  if get_option('wchar_filenames')
    spdlog_public_args += '-DSPDLOG_WCHAR_FILENAMES'
  endif
endif

if host_machine.system() == 'linux' and get_option('clock_coarse')
  spdlog_public_args += '-DSPDLOG_CLOCK_COARSE'
endif

if host_machine.system() == 'android'
  spdlog_deps += compiler.find_library('log')
endif

if is_msvc_like
  spdlog_public_args += '/Zc:__cplusplus'
  if get_option('msvc_utf8')
    spdlog_public_args += '/utf-8'
  endif
endif

if get_option('no_exceptions')
  if fmt_bundled
    spdlog_public_args += '-DFMT_USE_EXCEPTIONS=0'
  endif
  if is_msvc_like
    spdlog_private_args += ['/EHs-c-', '-D_HAS_EXCEPTIONS=0']
  else
    spdlog_private_args += '-fno-exceptions'
  endif
endif

# The header-only variant needs this define too, the compiled one keeps it private
spdlog_fwrite_args = []
if get_option('fwrite_unlocked')
  fwrite_symbol = is_windows ? '_fwrite_nolock' : 'fwrite_unlocked'
  if compiler.has_header_symbol('stdio.h', fwrite_symbol)
    spdlog_fwrite_args += '-DSPDLOG_FWRITE_UNLOCKED'
  endif
endif

spdlog_inc = include_directories('include', is_system: get_option('system_includes'))

spdlog_header_only_dep = declare_dependency(
  include_directories: spdlog_inc,
  compile_args: spdlog_public_args + spdlog_fwrite_args,
  dependencies: spdlog_deps,
  version: meson.project_version(),
)

if get_option('compile_library')
  spdlog_sources = files(
    'src/async.cpp',
    'src/cfg.cpp',
    'src/color_sinks.cpp',
    'src/file_sinks.cpp',
    'src/spdlog.cpp',
    'src/stdout_sinks.cpp',
  )
  if fmt_bundled
    spdlog_sources += files('src/bundled_fmtlib_format.cpp')
  endif

  spdlog_lib_public_args = ['-DSPDLOG_COMPILED_LIB']
  if build_shared
    spdlog_lib_public_args += '-DSPDLOG_SHARED_LIB'
    # Selects dllexport over dllimport in common.h (set implicitly by CMake)
    spdlog_private_args += '-Dspdlog_EXPORTS'
    if fmt_bundled
      spdlog_lib_public_args += '-DFMT_SHARED'
      spdlog_private_args += '-DFMT_LIB_EXPORT'
    endif
    if compiler.get_id() == 'msvc'
      spdlog_lib_public_args += ['/wd4251', '/wd4275']
    endif
  endif

  spdlog_lib = library(
    'spdlog',
    spdlog_sources,
    include_directories: spdlog_inc,
    cpp_args: spdlog_public_args
    + spdlog_lib_public_args
    + spdlog_fwrite_args
    + spdlog_private_args,
    dependencies: spdlog_deps,
    version: meson.project_version(),
    soversion: '1.17',
    override_options: spdlog_overrides,
    install: install_lib,
  )

  spdlog_dep = declare_dependency(
    include_directories: spdlog_inc,
    compile_args: spdlog_public_args + spdlog_lib_public_args,
    link_with: spdlog_lib,
    dependencies: spdlog_deps,
    version: meson.project_version(),
  )
else
  spdlog_dep = spdlog_header_only_dep
endif

meson.override_dependency('spdlog', spdlog_dep)
meson.override_dependency('spdlog_header_only', spdlog_header_only_dep)

if install_lib
  install_subdir(
    'include/spdlog',
    install_dir: get_option('includedir'),
    exclude_directories: fmt_bundled ? [] : ['fmt/bundled'],
  )

  if get_option('compile_library')
    import('pkgconfig').generate(
      spdlog_lib,
      name: 'libspdlog',
      filebase: 'spdlog',
      description: 'Fast C++ logging library.',
      url: 'https://github.com/gabime/spdlog',
      extra_cflags: spdlog_public_args + spdlog_lib_public_args,
    )
  endif
endif
