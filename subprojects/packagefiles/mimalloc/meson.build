# Meson-Port der mimalloc CMakeLists.txt
project(
  'libmimalloc',
  'c',
  meson_version: '>=1.1.0',
  default_options: [
    'c_std=c11',
    'buildtype=release',
    'b_ndebug=if-release', # wie CMake: -DNDEBUG in Release-Builds
  ],
)

fs = import('fs')
pkg = import('pkgconfig')

cc = meson.get_compiler('c')
cc_id = cc.get_id()
host_sys = host_machine.system()
is_windows = host_sys == 'windows'
is_apple = host_sys == 'darwin'
is_haiku = host_sys == 'haiku'
gcc_syntax = cc.get_argument_syntax() == 'gcc' # gcc, clang, AppleClang, Intel (nicht -cl)
is_clang_cl = cc_id == 'clang-cl'
is_clang = cc_id == 'clang' # inkl. AppleClang
is_gcc = cc_id == 'gcc'
is_intel = cc_id.startswith('intel')
is_mingw = is_windows and gcc_syntax

# -----------------------------------------------------------------------------
# Version (aus cmake/mimalloc-config-version.cmake lesen, damit es nur eine Quelle gibt)
# -----------------------------------------------------------------------------
_vtxt = fs.read('cmake/mimalloc-config-version.cmake')
mi_version_major = _vtxt.split('set(mi_version_major')[1].split(')')[0].strip()
mi_version_minor = _vtxt.split('set(mi_version_minor')[1].split(')')[0].strip()
mi_version_patch = _vtxt.split('set(mi_version_patch')[1].split(')')[0].strip()
mi_version = '@0@.@1@'.format(mi_version_major, mi_version_minor)

mi_sources = files(
  'src/alloc-aligned.c',
  'src/alloc-posix.c',
  'src/alloc.c',
  'src/arena-meta.c',
  'src/arena.c',
  'src/bitmap.c',
  'src/heap.c',
  'src/init.c',
  'src/libc.c',
  'src/options.c',
  'src/os.c',
  'src/page-map.c',
  'src/page.c',
  'src/prim/prim.c',
  'src/random.c',
  'src/stats.c',
  'src/theap.c',
  'src/threadlocal.c',
)

mi_cflags = []
mi_cflags_static = [] # zusätzliche Flags für die statische Bibliothek
mi_cflags_dynamic = [] # zusätzliche Flags für die Shared Library
mi_link_args = []
mi_deps = []
mi_defines = get_option('mi_extra_cppdefs')

# Optionen in Variablen (werden unten teils überschrieben, wie in CMake)
mi_secure = get_option('mi_secure')
mi_override = get_option('mi_override')
mi_use_cxx = get_option('mi_use_cxx')
mi_opt_arch = get_option('mi_opt_arch')
mi_track_valgrind = get_option('mi_track_valgrind')
mi_track_asan = get_option('mi_track_asan')
mi_track_etw = get_option('mi_track_etw')
mi_debug_full = get_option('mi_debug_full')
mi_debug_tsan = get_option('mi_debug_tsan')
mi_debug_ubsan = get_option('mi_debug_ubsan')
mi_no_padding = get_option('mi_no_padding')
mi_build_shared = get_option('mi_build_shared')
mi_build_static = get_option('mi_build_static')
mi_build_object = get_option('mi_build_object')
mi_build_tests = get_option('mi_build_tests')

# Git-Revision als Define (wird nur beim Konfigurieren ermittelt)
if fs.exists('.git/index')
  git = find_program('git', required: false)
  if git.found()
    r = run_command(git, 'describe', check: false)
    if r.returncode() == 0
      mi_defines += 'MI_GIT_DESCRIBE=' + r.stdout().strip()
    endif
  endif
endif

# -----------------------------------------------------------------------------
# Build-Typ (Meson: -Dbuildtype=debug|debugoptimized|release|minsize|plain)
# -----------------------------------------------------------------------------
buildtype = get_option('buildtype')
is_debug_build = buildtype == 'debug'

# -----------------------------------------------------------------------------
# Architektur
# -----------------------------------------------------------------------------
cpu = host_machine.cpu_family()
if cpu == 'x86'
  mi_arch = 'x86'
elif cpu == 'x86_64'
  mi_arch = 'x64'
elif cpu == 'aarch64'
  mi_arch = 'arm64'
elif cpu == 'arm'
  mi_arch = 'arm32'
else
  mi_arch = cpu # riscv32, riscv64, ...
endif
message('Architecture: ' + mi_arch)

# negative Overrides (vcpkg)
if get_option('mi_no_use_cxx')
  mi_use_cxx = false
endif
if get_option('mi_no_opt_arch')
  mi_opt_arch = false
elif mi_arch == 'arm64'
  mi_opt_arch = true # armv8.1-a standardmäßig auf arm64
endif

# -----------------------------------------------------------------------------
# C++-Kompilierung
# -----------------------------------------------------------------------------
# MSVC / clang-cl / Intel immer als C++ (moderne Atomics)
if cc_id in ['msvc', 'clang-cl'] or is_intel
  mi_use_cxx = true
elif mi_debug_ubsan and is_debug_build
  mi_use_cxx = true
endif

link_lang = 'c'
tgt_overrides = []
if mi_use_cxx
  # Meson kann die Sprache einzelner Dateien nicht umstellen; daher werden die
  # .c-Dateien per Compiler-Flag als C++ übersetzt und mit dem C++-Linker gelinkt.
  add_languages('cpp', native: false, required: true)
  cpp = meson.get_compiler('cpp')
  link_lang = 'cpp'
  tgt_overrides = ['c_std=none']
  if gcc_syntax
    mi_cflags += ['-x', 'c++', '-std=c++17']
  else
    mi_cflags += ['/TP', '/std:c++17']
  endif
  message('Use the C++ compiler to compile (mi_use_cxx=true)')
endif

# -----------------------------------------------------------------------------
# Process options
# -----------------------------------------------------------------------------#

# put -Wall early so other warnings can be disabled selectively
if is_clang
  mi_cflags += ['-Wall', '-Wextra', '-Wpedantic']
elif is_clang_cl
  mi_cflags += ['-W']
elif is_gcc
  mi_cflags += ['-Wall', '-Wextra']
elif is_intel
  mi_cflags += ['-Wall']
endif

# C++ flags
if mi_use_cxx and meson.get_compiler('cpp').get_id() == 'clang'
  mi_cflags += ['-Wno-deprecated']
endif

if mi_override
  message('Override standard malloc (mi_override=true)')
  if is_apple
    if get_option('mi_osx_zone')
      message('  Use malloc zone to override malloc (mi_osx_zone=true)')
      mi_sources += files('src/prim/osx/alloc-override-zone.c')
      mi_defines += 'MI_OSX_ZONE=1'
      if not get_option('mi_osx_interpose')
        warning(
          'zone overriding usually also needs interpose (use -Dmi_osx_interpose=true)',
        )
      endif
    endif
    if get_option('mi_osx_interpose')
      message('  Use interpose to override malloc (mi_osx_interpose=true)')
      mi_defines += 'MI_OSX_INTERPOSE=1'
      if not get_option('mi_osx_zone')
        warning(
          'interpose usually also needs zone overriding (use -Dmi_osx_zone=true)',
        )
      endif
    endif
    if mi_use_cxx and get_option('mi_osx_interpose')
      warning(
        'if dynamically overriding malloc/free, it is more reliable to build mimalloc as C code (use -Dmi_use_cxx=false)',
      )
    endif
  endif
endif

if is_windows and not get_option('mi_win_redirect')
  mi_defines += 'MI_WIN_NOREDIRECT=1'
endif

if mi_secure
  message('Set full secure build (mi_secure=true)')
  mi_defines += 'MI_SECURE=4'
endif

if mi_track_valgrind
  if not (
    cc.has_header('valgrind/valgrind.h')
    and cc.has_header('valgrind/memcheck.h')
  )
    mi_track_valgrind = false
    warning(
      'Cannot find valgrind/valgrind.h and valgrind/memcheck.h -- install valgrind first? Disabling Valgrind support.',
    )
  else
    message('Compile with Valgrind support (mi_track_valgrind=true)')
    mi_defines += 'MI_TRACK_VALGRIND=1'
  endif
endif

if mi_track_asan
  if is_apple and mi_override
    mi_track_asan = false
    warning(
      'Cannot enable address sanitizer support on macOS if mi_override is true (mi_track_asan=false)',
    )
  endif
  if mi_track_valgrind
    mi_track_asan = false
    warning(
      'Cannot enable address sanitizer support with also Valgrind support enabled (mi_track_asan=false)',
    )
  endif
  if mi_track_asan
    if not cc.has_header('sanitizer/asan_interface.h')
      mi_track_asan = false
      warning(
        'Cannot find sanitizer/asan_interface.h -- compiling without address sanitizer support',
      )
    else
      message('Compile with address sanitizer support (mi_track_asan=true)')
      mi_defines += 'MI_TRACK_ASAN=1'
      mi_cflags += '-fsanitize=address'
      mi_link_args += '-fsanitize=address'
    endif
  endif
endif

if mi_track_etw
  if not is_windows
    mi_track_etw = false
    warning('Can only enable ETW support on Windows (mi_track_etw=false)')
  endif
  if mi_track_valgrind or mi_track_asan
    mi_track_etw = false
    warning(
      'Cannot enable ETW support with also Valgrind or ASAN support enabled (mi_track_etw=false)',
    )
  endif
  if mi_track_etw
    message('Compile with Windows event tracing support (mi_track_etw=true)')
    mi_defines += 'MI_TRACK_ETW=1'
  endif
endif

if get_option('mi_guarded')
  message('Compile guard pages behind certain object allocations (mi_guarded=true)')
  mi_defines += 'MI_GUARDED=1'
  if not mi_no_padding
    message('  Disabling padding due to guard pages (mi_no_padding=true)')
    mi_no_padding = true
  endif
endif

if get_option('mi_see_asm')
  message('Generate assembly listings (mi_see_asm=true)')
  if cc_id == 'msvc' or is_clang_cl
    mi_cflags += '-FA'
  else
    mi_cflags += '-save-temps'
    if is_clang and cc.version().version_compare('>14')
      mi_cflags += '-Wno-gnu-line-marker'
    endif
  endif
endif

if get_option('mi_check_full')
  message('The mi_check_full option is deprecated, use mi_debug_full instead')
  mi_debug_full = true
endif

if get_option('mi_skip_collect_on_exit')
  message('Skip collecting memory on program exit (mi_skip_collect_on_exit=true)')
  mi_defines += 'MI_SKIP_COLLECT_ON_EXIT=1'
endif

if mi_debug_full
  message(
    'Set debug level to full assertion and internal invariant checking (mi_debug_full=true, expensive)',
  )
  mi_defines += 'MI_DEBUG=3'
elif get_option('mi_debug_internal')
  message(
    'Set debug level to internal assertion and invariant checking (mi_debug_internal=true)',
  )
  mi_defines += 'MI_DEBUG=2'
elif get_option('mi_debug')
  message('Set debug level to assertion checking (mi_debug=true)')
  mi_defines += 'MI_DEBUG=1'
elif is_debug_build
  message(
    'Set debug level to internal assertion and invariant checking (buildtype=debug)',
  )
  mi_defines += 'MI_DEBUG=2'
endif

if mi_no_padding
  message('Suppress any padding of heap blocks (mi_no_padding=true)')
  mi_defines += 'MI_PADDING=0'
elif get_option('mi_padding')
  message('Enable explicit padding of heap blocks (mi_padding=true)')
  mi_defines += 'MI_PADDING=1'
endif

if get_option('mi_xmalloc')
  message('Enable abort() calls on memory allocation failure (mi_xmalloc=true)')
  mi_defines += 'MI_XMALLOC=1'
endif

if get_option('mi_show_errors')
  message(
    'Enable printing of error and warning messages by default (mi_show_errors=true)',
  )
  mi_defines += 'MI_SHOW_ERRORS=1'
endif

if mi_debug_tsan
  if is_clang
    message('Build with thread sanitizer (mi_debug_tsan=true)')
    mi_defines += 'MI_TSAN=1'
    mi_cflags += ['-fsanitize=thread', '-g', '-O1']
    mi_link_args += '-fsanitize=thread'
  else
    warning(
      'Can only use thread sanitizer with clang (mi_debug_tsan=true but ignored)',
    )
    mi_debug_tsan = false
  endif
endif

if mi_debug_ubsan
  if not is_debug_build
    warning(
      'Can only use undefined-behavior sanitizer with a debug build (buildtype='
      + buildtype
      + ')',
    )
    mi_debug_ubsan = false
  elif not mi_use_cxx
    warning(
      'Can only use undefined-behavior sanitizer with a C++ build (mi_use_cxx=true)',
    )
    mi_debug_ubsan = false
  elif meson.get_compiler('cpp').get_id() != 'clang'
    warning(
      'Can only use undefined-behavior sanitizer with clang++ (mi_debug_ubsan=true but ignored)',
    )
    mi_debug_ubsan = false
  else
    message('Build with undefined-behavior sanitizer (mi_debug_ubsan=true)')
    mi_defines += 'MI_UBSAN=1'
    mi_cflags += ['-fsanitize=undefined', '-g', '-fno-sanitize-recover=undefined']
    mi_link_args += '-fsanitize=undefined'
  endif
endif

if host_sys in ['linux', 'android'] and get_option('mi_no_thp')
  message('Disable transparent huge pages support (mi_no_thp=true)')
  mi_defines += 'MI_NO_THP=1'
endif

if get_option('mi_libc_musl')
  message('Assume using musl libc (mi_libc_musl=true)')
  mi_defines += 'MI_LIBC_MUSL=1'
endif

if get_option('mi_win_use_fls')
  message(
    'Use the Fiber API to detect thread termination (deprecated) (mi_win_use_fls=true)',
  )
  mi_defines += 'MI_WIN_USE_FLS=1'
endif

if get_option('mi_win_direct_tls')
  message(
    'Use only direct TLS slots on Windows -- only works if the program uses less than 64 TlsAlloc slots in total!',
  )
  mi_defines += 'MI_WIN_DIRECT_TLS=1'
endif

# SV39-MMU erkennen (Issue #939, #949) -- nur bei nativen Linux-Builds
if host_sys == 'linux' and not meson.is_cross_build() and fs.is_file('/proc/cpuinfo')
  r = run_command(
    'grep',
    '-Eq', '^mmu[[:space:]]+:[[:space:]]+sv39$',
    '/proc/cpuinfo',
    check: false,
  )
  if r.returncode() == 0
    message('Set virtual address bits to 39 (SV39 MMU detected)')
    mi_defines += 'MI_DEFAULT_VIRTUAL_ADDRESS_BITS=39'
  endif
endif

# Compiler-Flags
if gcc_syntax and not is_intel
  mi_cflags += ['-Wno-unknown-pragmas', '-fvisibility=hidden']
  if not mi_use_cxx
    mi_cflags += '-Wstrict-prototypes'
  endif
  if is_clang
    mi_cflags += '-Wno-static-in-inline'
  endif
endif
if is_intel and gcc_syntax
  mi_cflags += '-fvisibility=hidden'
endif

if gcc_syntax and not is_haiku
  if get_option('mi_local_dynamic_tls')
    mi_cflags += '-ftls-model=local-dynamic'
  elif get_option('mi_libc_musl')
    # mit musl local-dynamic für den statischen Build, siehe Issue #644
    mi_cflags_static += '-ftls-model=local-dynamic'
    mi_cflags_dynamic += '-ftls-model=initial-exec'
    message('Use local dynamic TLS for the static build (since mi_libc_musl=true)')
  else
    mi_cflags += '-ftls-model=initial-exec'
  endif
endif

if gcc_syntax and mi_override
  mi_cflags += '-fno-builtin-malloc'
endif

# Compiler and architecture specific flags
mi_opt_arch_flags = []
if gcc_syntax and not is_haiku and mi_opt_arch
  if mi_arch == 'x64'
    mi_opt_arch_flags = ['-march=haswell', '-mavx2'] # fast bit scan (seit 2013)
  elif mi_arch == 'arm64'
    mi_opt_arch_flags = ['-march=armv8.1-a'] # fast atomics (seit 2016)
  endif
endif

if (cc_id == 'msvc' and cc.version().version_compare('>=19.14')) or is_clang_cl
  mi_cflags += '/Zc:__cplusplus'
  if mi_opt_arch and not is_clang_cl
    if mi_arch == 'x64'
      mi_opt_arch_flags = ['/arch:AVX2']
    elif mi_arch == 'arm64'
      mi_opt_arch_flags = ['/arch:armv8.1']
    endif
  endif
endif

if is_mingw
  mi_cflags += '-D_WIN32_WINNT=0x600' # Issue #976
endif

if mi_opt_arch_flags.length() > 0
  mi_cflags += mi_opt_arch_flags
  message(
    'Architecture specific optimization is enabled (with @0@) (mi_opt_arch=true)'.format(mi_opt_arch_flags),
  )
  if get_option('mi_opt_simd')
    mi_defines += 'MI_OPT_SIMD=1'
    message('SIMD instructions are enabled (mi_opt_simd=true)')
  endif
elif get_option('mi_opt_simd')
  message(
    'SIMD instructions are not enabled (either mi_opt_arch=false or this architecture has no SIMD support)',
  )
endif

# Zusätzliche Bibliotheken
if is_windows
  foreach l : ['psapi', 'shell32', 'user32', 'advapi32', 'bcrypt']
    mi_deps += cc.find_library(l)
  endforeach
else
  mi_deps += dependency('threads')
  foreach l : ['rt', 'atomic']
    d = cc.find_library(l, required: false)
    if d.found()
      mi_deps += d
    endif
  endforeach
endif

# -----------------------------------------------------------------------------
# Installations- und Ausgabenamen
# -----------------------------------------------------------------------------
libdir = get_option('libdir')
if get_option('mi_install_toplevel')
  mi_install_objdir = libdir
  mi_install_incsubdir = ''
else
  mi_install_objdir = libdir / ('mimalloc-' + mi_version)
  mi_install_incsubdir = 'mimalloc-' + mi_version
endif

mi_libname = 'mimalloc'
if mi_secure
  mi_libname += '-secure'
endif
if mi_track_valgrind
  mi_libname += '-valgrind'
endif
if mi_track_asan
  mi_libname += '-asan'
endif

# Meson-Buildtypen auf die CMake-Namen abbilden (die Quellen geben ihn aus)
buildtype_lc = {
  'debug': 'debug',
  'debugoptimized': 'relwithdebinfo',
  'release': 'release',
  'minsize': 'minsizerel',
  'plain': 'none',
}.get(buildtype, buildtype)
mi_defines += 'MI_CMAKE_BUILD_TYPE=' + buildtype_lc
if buildtype_lc in ['release', 'relwithdebinfo', 'minsizerel', 'none']
  mi_defines += 'MI_BUILD_RELEASE'
else
  mi_libname += '-' + buildtype_lc
endif

mi_def_args = []
foreach d : mi_defines
  mi_def_args += '-D' + d
endforeach
mi_c_args = mi_cflags + mi_def_args

override_args = mi_override ? ['-DMI_MALLOC_OVERRIDE'] : []
# Unter Windows lässt sich malloc nur als DLL überschreiben
override_args_static = (mi_override and not is_windows) ? ['-DMI_MALLOC_OVERRIDE'] : []

message('')
message('Library name     : ' + mi_libname)
message('Version          : @0@.@1@'.format(mi_version, mi_version_patch))
message('Build type       : ' + buildtype_lc)
message(
  'Compiler         : '
  + (mi_use_cxx ? meson.get_compiler('cpp').get_id() + ' (C++)' : cc_id),
)
message('Compiler flags   : @0@'.format(mi_cflags))
message('Compiler defines : @0@'.format(mi_defines))
message('Link args        : @0@'.format(mi_link_args))
message('')

# Mit Sanitizern werden deren Symbole erst im Programm aufgelöst; Meson linkt
# Shared Libraries standardmäßig mit --no-undefined (CMake nicht), daher abschalten.
if mi_track_asan or mi_debug_tsan or mi_debug_ubsan
  tgt_overrides += 'b_lundef=false'
endif

inc = include_directories('include')

# -----------------------------------------------------------------------------
# Haupt-Targets
# -----------------------------------------------------------------------------

# Shared Library
if mi_build_shared
  shared_link_args = mi_link_args
  name_prefix = []
  if is_windows and get_option('mi_win_redirect')
    if is_mingw
      name_prefix = ''
    endif
    if get_option('mi_win_redirect_arm64ec')
      redirect_suffix = '-arm64ec'
    elif mi_arch == 'x64'
      redirect_suffix = ''
    elif mi_arch == 'x86'
      redirect_suffix = '32'
    else
      redirect_suffix = '-' + mi_arch
    endif
    redirect_name = 'mimalloc-redirect' + redirect_suffix
    shared_link_args += meson.current_source_dir() / 'bin' / redirect_name + '.lib'
    # DLL ins Build-Verzeichnis kopieren und mit installieren
    fs.copyfile(
      'bin' / redirect_name + '.dll',
      install: true,
      install_dir: get_option('bindir'),
    )
  endif

  mimalloc_shared = shared_library(
    mi_libname,
    mi_sources,
    c_args: mi_c_args
    + mi_cflags_dynamic
    + override_args
    + ['-DMI_SHARED_LIB', '-DMI_SHARED_LIB_EXPORT'],
    link_args: shared_link_args,
    dependencies: mi_deps,
    include_directories: inc,
    version: mi_version,
    soversion: mi_version_major,
    name_prefix: name_prefix,
    link_language: link_lang,
    override_options: tgt_overrides,
    install: true,
  )
endif

# Statische Bibliothek
if mi_build_static
  mimalloc_static = static_library(
    mi_libname,
    mi_sources,
    c_args: mi_c_args + mi_cflags_static + override_args_static + ['-DMI_STATIC_LIB'],
    dependencies: mi_deps,
    include_directories: inc,
    pic: true,
    override_options: tgt_overrides,
    install: true,
    install_dir: mi_install_objdir,
  )
endif

# Header
install_headers(
  'include/mimalloc.h',
  'include/mimalloc-override.h',
  'include/mimalloc-new-delete.h',
  'include/mimalloc-stats.h',
  subdir: mi_install_incsubdir,
)

# Einzelne Objektdatei (src/static.c) für vorhersagbares statisches Überschreiben.
# Meson hat keine Object-Libraries; die Objekte sind über `mimalloc_obj`
# (extract_all_objects) für andere Targets nutzbar, z.B. executable(..., objects: mimalloc_obj).
if mi_build_object
  mimalloc_obj_lib = static_library(
    'mimalloc-obj',
    'src/static.c',
    c_args: mi_c_args + mi_cflags_static + override_args_static,
    include_directories: inc,
    pic: true,
    override_options: tgt_overrides,
    install: false,
  )
  mimalloc_obj = mimalloc_obj_lib.extract_all_objects(recursive: false)
endif

# Dependency-Objekte für die Nutzung als Meson-Subprojekt
if mi_build_shared
  mimalloc_dep = declare_dependency(link_with: mimalloc_shared, include_directories: inc)
  meson.override_dependency('mimalloc', mimalloc_dep)
endif
if mi_build_static
  mimalloc_static_dep = declare_dependency(
    link_with: mimalloc_static,
    include_directories: inc,
    dependencies: mi_deps,
    compile_args: ['-DMI_STATIC_LIB'],
  )
  if not mi_build_shared
    meson.override_dependency('mimalloc', mimalloc_static_dep)
  endif
endif

# pkg-config
if mi_build_shared or mi_build_static
  pkg.generate(
    mi_build_shared ? mimalloc_shared : mimalloc_static,
    name: 'mimalloc',
    filebase: 'mimalloc',
    description: 'A compact general purpose allocator with excellent performance',
    version: '@0@.@1@'.format(mi_version, mi_version_patch),
    subdirs: mi_install_incsubdir != '' ? [mi_install_incsubdir] : [],
  )
endif

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if mi_build_tests
  test_link = []
  if mi_build_static and not mi_debug_tsan
    test_link = [mimalloc_static]
  elif mi_build_shared
    test_link = [mimalloc_shared]
  else
    message('cannot build TSAN tests without mi_build_shared being enabled')
  endif

  if test_link.length() > 0
    foreach name : ['api', 'api-fill', 'stress']
      exe = executable(
        'mimalloc-test-' + name,
        'test/test-' + name + '.c',
        c_args: mi_c_args,
        link_args: mi_link_args,
        include_directories: inc,
        link_with: test_link,
        dependencies: mi_deps,
        link_language: link_lang,
        override_options: tgt_overrides,
      )
      test('test-' + name, exe, timeout: 300)
    endforeach
  endif

  # dynamischer Override-Test
  if mi_build_shared and not (mi_track_asan or mi_debug_tsan or mi_debug_ubsan)
    if is_windows
      exe = executable(
        'mimalloc-test-stress-dynamic',
        'test/test-stress.c',
        c_args: mi_c_args + ['-DUSE_STD_MALLOC=1', '-DMI_LINK_VERSION=1'],
        include_directories: inc,
        link_with: mimalloc_shared,
        dependencies: mi_deps,
        link_language: link_lang,
        override_options: tgt_overrides,
      )
      test(
        'test-stress-dynamic',
        exe,
        env: {'MIMALLOC_VERBOSE': '1'},
        timeout: 300,
      )
    else
      exe = executable(
        'mimalloc-test-stress-dynamic',
        'test/test-stress.c',
        c_args: mi_c_args + ['-DUSE_STD_MALLOC=1'],
        include_directories: inc,
        dependencies: mi_deps, # pthreads, Issue 1158
        link_language: link_lang,
        override_options: tgt_overrides,
      )
      preload_var = is_apple ? 'DYLD_INSERT_LIBRARIES' : 'LD_PRELOAD'
      test(
        'test-stress-dynamic',
        exe,
        env: {'MIMALLOC_VERBOSE': '1', preload_var: mimalloc_shared.full_path()},
        depends: mimalloc_shared,
        timeout: 300,
      )
    endif
  endif
endif
