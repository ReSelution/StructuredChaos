# Meson port of mimalloc's CMakeLists.txt (v3.5.x)
project(
  'libmimalloc',
  'c',
  meson_version: '>=1.1.0',
  default_options: ['c_std=c11'],
)

fs = import('fs')
pkg = import('pkgconfig')

cc = meson.get_compiler('c')
cc_id = cc.get_id()
host_sys = host_machine.system()
is_windows = host_sys == 'windows'
is_apple = host_sys == 'darwin'
is_haiku = host_sys == 'haiku'
gcc_syntax = cc.get_argument_syntax() == 'gcc' # gcc, clang, AppleClang, Intel (not -cl)
is_clang_cl = cc_id == 'clang-cl'
is_msvc_like = cc_id == 'msvc' or is_clang_cl
is_clang = cc_id == 'clang' # including AppleClang
is_gcc = cc_id == 'gcc'
is_intel = cc_id.startswith('intel')
is_mingw = is_windows and gcc_syntax

# -----------------------------------------------------------------------------
# Version (read from cmake/mimalloc-config-version.cmake, the single source)
# -----------------------------------------------------------------------------
_vtxt = fs.read('cmake/mimalloc-config-version.cmake')
mi_version_major = _vtxt.split('set(mi_version_major')[1].split(')')[0].strip()
mi_version_minor = _vtxt.split('set(mi_version_minor')[1].split(')')[0].strip()
mi_version_patch = _vtxt.split('set(mi_version_patch')[1].split(')')[0].strip()
mi_version = '@0@.@1@'.format(mi_version_major, mi_version_minor)
mi_version_full = '@0@.@1@'.format(mi_version, mi_version_patch)

mi_sources = files(
  'src/alloc-aligned.c',
  'src/alloc-posix.c',
  'src/alloc.c',
  'src/arena.c',
  'src/bitmap.c',
  'src/heap.c',
  'src/init.c',
  'src/libc.c',
  'src/options.c',
  'src/os.c',
  'src/page-map.c',
  'src/page.c',
  'src/prim/prim-tls.c',
  'src/prim/prim.c',
  'src/random.c',
  'src/sample-guarded.c',
  'src/sample-profile.c',
  'src/stats.c',
  'src/subproc.c',
  'src/theap.c',
  'src/threadlocal.c',
)

mi_cflags = []
mi_cflags_static = [] # extra flags for the static library
mi_cflags_dynamic = [] # extra flags for the shared library
mi_link_args = []
mi_deps = []
mi_defines = get_option('mi_extra_cppdefs')

# Options that are adjusted below, like in CMake
mi_secure = get_option('mi_secure')
mi_override = get_option('mi_override')
mi_use_cxx_opt = get_option('mi_use_cxx')
mi_opt_arch = get_option('mi_opt_arch').to_lower()
mi_debug = get_option('mi_debug')
mi_track = get_option('mi_track')
mi_guarded = get_option('mi_guarded')
mi_stats = get_option('mi_stats')
mi_profile = get_option('mi_profile')
mi_free_is_checked = get_option('mi_free_is_checked')
mi_allow_thp = get_option('mi_allow_thp')
mi_tls_model = get_option('mi_tls_model')
mi_win_init = get_option('mi_win_init')
mi_debug_tsan = get_option('mi_debug_tsan')
mi_debug_ubsan = get_option('mi_debug_ubsan')
mi_build_shared = get_option('mi_build_shared')
mi_build_static = get_option('mi_build_static')
mi_build_object = get_option('mi_build_object')
mi_build_tests = get_option('mi_build_tests')

# Git revision as a define (only determined at configure time)
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
# Build type (Meson: -Dbuildtype=debug|debugoptimized|release|minsize|plain)
# -----------------------------------------------------------------------------
buildtype = get_option('buildtype')
is_debug_build = buildtype == 'debug'

# -----------------------------------------------------------------------------
# Architecture
# -----------------------------------------------------------------------------
mi_arch_bits = '@0@'.format(cc.sizeof('void*') * 8)
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
message('Architecture: @0@ (@1@ bit)'.format(mi_arch, mi_arch_bits))

if mi_opt_arch == 'default'
  # enabled by default for arm64 (armv8.1-a has fast atomics)
  mi_opt_arch = mi_arch == 'arm64' ? 'on' : 'off'
endif

# -----------------------------------------------------------------------------
# C++ compilation
# -----------------------------------------------------------------------------
if mi_use_cxx_opt == 'default'
  # MSVC / clang-cl / Intel always as C++ (modern atomics); ubsan needs C++
  mi_use_cxx = is_msvc_like or is_intel or (mi_debug_ubsan and is_debug_build)
else
  mi_use_cxx = mi_use_cxx_opt == 'on'
endif

link_lang = 'c'
tgt_overrides = []
if mi_use_cxx
  # Meson cannot switch the language of single files, so the .c files are
  # compiled as C++ through a compiler flag and linked with the C++ linker.
  add_languages('cpp', native: false, required: true)
  cpp = meson.get_compiler('cpp')
  link_lang = 'cpp'
  tgt_overrides = ['c_std=none']
  if gcc_syntax
    mi_cflags += ['-x', 'c++', '-std=c++17']
  else
    # /EHsc is part of CMake's default C++ flags; the sources use try/catch
    mi_cflags += ['/TP', '/std:c++17', '/EHsc']
  endif
  message('Use the C++ compiler to compile (mi_use_cxx=on)')
  if cc_id == 'intel'
    mi_cflags += '-Kc++'
  elif is_clang_cl or (cc_id == 'msvc' and cc.version().version_compare('>=19.14'))
    mi_cflags += '/Zc:__cplusplus'
  endif
endif

# -----------------------------------------------------------------------------
# Process options
# -----------------------------------------------------------------------------

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

if mi_use_cxx and cpp.get_id() == 'clang'
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
        'if dynamically overriding malloc/free, it is more reliable to build mimalloc as C code (use -Dmi_use_cxx=off)',
      )
    endif
  endif
endif

if is_windows and not get_option('mi_win_redirect')
  mi_defines += 'MI_WIN_NOREDIRECT=1'
endif

if mi_secure == 'full'
  message('Use full secure build (mi_secure=full, may be expensive)')
  mi_defines += 'MI_SECURE=5'
elif mi_secure == 'on'
  message('Use secure build (mi_secure=on)')
  mi_defines += 'MI_SECURE=4'
endif
is_secure = mi_secure != 'off'

if mi_track == 'valgrind'
  if not (
    cc.has_header('valgrind/valgrind.h')
    and cc.has_header('valgrind/memcheck.h')
  )
    mi_track = 'off'
    warning(
      'Cannot find valgrind/valgrind.h and valgrind/memcheck.h -- install valgrind first? Disabling Valgrind support (mi_track=off)',
    )
  else
    message('Compile with Valgrind support (mi_track=valgrind)')
    mi_defines += 'MI_TRACK_VALGRIND=1'
  endif
elif mi_track == 'asan'
  if is_apple and mi_override
    mi_track = 'off'
    warning(
      'Cannot enable address sanitizer support on macOS if mi_override is true (mi_track=off)',
    )
  elif not cc.has_header('sanitizer/asan_interface.h')
    mi_track = 'off'
    warning(
      'Cannot find sanitizer/asan_interface.h -- compiling without address sanitizer support (mi_track=off)',
    )
  else
    message('Compile with address sanitizer support (mi_track=asan)')
    mi_defines += 'MI_TRACK_ASAN=1'
    mi_cflags += '-fsanitize=address'
    mi_link_args += '-fsanitize=address'
  endif
elif mi_track == 'etw'
  if not is_windows
    mi_track = 'off'
    warning('Can only enable ETW support on Windows (mi_track=off)')
  else
    message('Compile with Windows event tracing support (mi_track=etw)')
    mi_defines += 'MI_TRACK_ETW=1'
  endif
endif

if get_option('mi_see_asm')
  message('Generate assembly listings (mi_see_asm=true)')
  if is_msvc_like
    mi_cflags += '-FA'
  else
    mi_cflags += '-save-temps=obj'
    if is_clang and cc.version().version_compare('>14')
      mi_cflags += '-Wno-gnu-line-marker'
    endif
  endif
endif

if get_option('mi_skip_collect_on_exit')
  message('Skip collecting memory on program exit (mi_skip_collect_on_exit=true)')
  mi_defines += 'MI_SKIP_COLLECT_ON_EXIT=1'
endif

if mi_debug == 'default'
  mi_debug = is_debug_build ? 'internal' : 'off'
endif
if mi_debug == 'full'
  message(
    'Set debug level to full assertion and internal invariant checking (mi_debug=full, expensive)',
  )
  mi_defines += 'MI_DEBUG=3'
elif mi_debug == 'internal'
  message(
    'Set debug level to internal assertion and invariant checking (mi_debug=internal)',
  )
  mi_defines += 'MI_DEBUG=2'
elif mi_debug == 'on'
  message('Set debug level to assertion checking (mi_debug=on)')
  mi_defines += 'MI_DEBUG=1'
else
  message('Disable assertion checks (mi_debug=off)')
  mi_defines += 'NDEBUG=1'
endif
is_mi_debug = mi_debug != 'off'

if mi_guarded == 'default'
  mi_guarded = is_mi_debug ? 'full' : 'off'
endif
if mi_guarded == 'full'
  message('Enable fine-grained sampling for guard pages (mi_guarded=full)')
  mi_defines += 'MI_GUARDED=2'
elif mi_guarded == 'on'
  message('Enable sampled guard pages (mi_guarded=on)')
  mi_defines += 'MI_GUARDED=1'
else
  mi_defines += 'MI_GUARDED=0'
endif
is_guarded = mi_guarded != 'off'

if get_option('mi_no_padding')
  message('Suppress any padding of heap blocks (mi_no_padding=true)')
  mi_defines += 'MI_PADDING=0'
elif get_option('mi_padding')
  message('Enable explicit padding of heap blocks (mi_padding=true)')
  mi_defines += 'MI_PADDING=1'
endif

if mi_free_is_checked == 'default'
  mi_free_is_checked = is_secure ? 'on' : 'off'
endif
if mi_free_is_checked == 'on'
  message('Check validity of pointers passed to mi_free (mi_free_is_checked=on)')
  mi_defines += 'MI_FREE_IS_CHECKED=1'
elif is_secure
  message('Check validity of pointers passed to mi_free (mi_secure is enabled)')
elif mi_free_is_checked == 'shared'
  message(
    'Check validity of pointers passed to mi_free only in the shared library (mi_free_is_checked=shared)',
  )
  mi_cflags_dynamic += '-DMI_FREE_IS_CHECKED=1'
endif

if mi_stats == 'default'
  mi_stats = is_mi_debug ? 'full' : 'on'
endif
if mi_stats == 'full'
  message(
    'Enable detailed statistics including precise requested bytes (expensive) (mi_stats=full)',
  )
  mi_defines += 'MI_STATS=2'
elif mi_stats == 'on'
  message('Enable detailed statistics (mi_stats=on)')
  mi_defines += 'MI_STATS=1'
else
  message('Disable detailed statistics (mi_stats=off)')
  mi_defines += 'MI_STATS=0'
endif

if mi_profile == 'default'
  mi_profile = is_mi_debug ? 'full' : 'on'
endif
if mi_profile == 'full'
  message('Enable fine-grained sampled profiling (mi_profile=full)')
  mi_defines += 'MI_PROFILE=2'
elif mi_profile == 'on'
  message('Enable sampled profiling (mi_profile=on)')
  mi_defines += 'MI_PROFILE=1'
else
  message('Disable profiling (mi_profile=off)')
  mi_defines += 'MI_PROFILE=0'
endif

mi_free_use_pagemap = get_option('mi_free_use_pagemap')
if host_sys == 'wasi'
  message(
    'Enabling mi_free_use_pagemap since the target (WASI) does not support large OS aligned allocations well',
  )
  mi_free_use_pagemap = true
endif
if mi_free_use_pagemap
  message('Always use the pagemap in mi_free (mi_free_use_pagemap=true)')
  mi_defines += 'MI_FREE_USE_PAGEMAP=1'
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
      'Can only use undefined-behavior sanitizer with a C++ build (mi_use_cxx=on)',
    )
    mi_debug_ubsan = false
  elif cpp.get_id() != 'clang'
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

if host_sys in ['linux', 'android']
  if mi_allow_thp == 'default'
    mi_allow_thp = host_sys == 'android' ? 'off' : 'full'
  endif
  if mi_allow_thp == 'full'
    message(
      'Enable full transparent huge pages support, where purging avoids splitting huge pages (mi_allow_thp=full)',
    )
    mi_defines += 'MI_DEFAULT_ALLOW_THP=2'
  elif mi_allow_thp == 'on'
    message(
      'Enable transparent huge pages support (mi_allow_thp=on) (also consider using full for better performance)',
    )
    mi_defines += 'MI_DEFAULT_ALLOW_THP=1'
  else
    message('Disable transparent huge pages support (mi_allow_thp=off)')
    mi_defines += 'MI_DEFAULT_ALLOW_THP=0'
  endif
endif

if get_option('mi_libc_musl')
  message('Assume using musl libc (mi_libc_musl=true)')
  mi_defines += 'MI_LIBC_MUSL=1'
endif

if mi_tls_model == 'pthreads'
  message('Use TLS model based on pthreads (mi_tls_model=pthreads)')
  mi_defines += 'MI_TLS_MODEL_PTHREADS=1'
elif mi_tls_model == 'fixed'
  message('Use TLS model based on fixed thread local slots (mi_tls_model=fixed)')
  message(
    'Note: fixed TLS slots do not allow multiple mimalloc instances in a single process.',
  )
  mi_defines += 'MI_TLS_MODEL_FIXED=1'
elif mi_tls_model == 'local'
  message('Use TLS model based on thread local (mi_tls_model=local)')
  if is_apple
    warning(
      'On macOS mi_tls_model=local does not work reliably when dynamically overriding mimalloc. mi_tls_model=pthreads is recommended instead.',
    )
  endif
  mi_defines += 'MI_TLS_MODEL_LOCAL=1'
elif mi_tls_model == 'local_dynamic'
  message(
    'Use TLS model based on dlopen compatible dynamic thread locals (mi_tls_model=local_dynamic)',
  )
  mi_defines += 'MI_TLS_MODEL_LOCAL=1'
elif mi_tls_model == 'win32'
  message(
    'Use TLS model based on direct Win32 TlsAlloc\'d slots (mi_tls_model=win32)',
  )
  mi_defines += 'MI_TLS_MODEL_WIN32=1'
endif

if get_option('mi_tls_recurse_guard')
  message('Use TLS recursion guard (for mi_tls_model=local only)')
  mi_defines += 'MI_TLS_RECURSE_GUARD=1'
endif

if mi_win_init == 'raw_dllmain'
  message(
    'Use raw Dll main entry on Windows to initialize and finalize mimalloc (mi_win_init=raw_dllmain)',
  )
  message(
    'Note: this can lead to link errors if other libraries also try to define a raw Dll main entry point',
  )
  mi_defines += 'MI_WIN_INIT_USE_RAW_DLLMAIN=1'
elif mi_win_init == 'tls_dllmain'
  message(
    'Use Dll main entry with TLS entries on Windows to initialize and finalize mimalloc (mi_win_init=tls_dllmain)',
  )
  mi_defines += 'MI_WIN_INIT_USE_TLS_DLLMAIN=1'
elif mi_win_init == 'fls'
  message(
    'Use the Fiber API to detect thread termination (deprecated) (mi_win_init=fls)',
  )
  mi_defines += 'MI_WIN_INIT_USE_FLS=1'
endif

if mi_arch.startswith('riscv')
  if cc.has_header('asm/hwprobe.h')
    mi_defines += 'MI_HAS_ASM_HWPROBEH=1'
  endif
  if cc.has_header('sys/hwprobe.h')
    mi_defines += 'MI_HAS_SYS_HWPROBEH=1'
  endif
endif

if not is_windows and cc.has_header('execinfo.h')
  mi_defines += 'MI_HAS_EXECINFOH=1'
endif

# Compiler flags
if gcc_syntax and not is_intel
  mi_cflags += ['-Wno-unknown-pragmas', '-fvisibility=hidden']
  if not mi_use_cxx
    mi_cflags += '-Wstrict-prototypes'
  endif
  if is_clang
    mi_cflags += '-Wno-static-in-inline'
    mi_cflags += cc.get_supported_arguments('-mno-outline')
    if is_apple and mi_arch == 'arm64'
      mi_cflags += ['-Xclang', '-fno-split-cold-code']
    endif
  endif
endif
if is_intel and gcc_syntax
  mi_cflags += '-fvisibility=hidden'
endif

if gcc_syntax and not is_haiku
  if mi_tls_model == 'local_dynamic'
    mi_cflags += '-ftls-model=local-dynamic'
  elif get_option('mi_libc_musl')
    # with musl use local-dynamic for the static build, see issue #644
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
if mi_opt_arch == 'on'
  if mi_arch == 'arm64'
    # Apple M1+ supports armv8.3-a (which includes LDAPR), elsewhere default
    # to armv8.1-a (which has fast atomics)
    mi_opt_arch = is_apple ? 'armv8.3' : 'armv8.1'
  elif mi_arch == 'arm32'
    mi_opt_arch = 'armv8.1'
  elif mi_arch == 'x64'
    mi_opt_arch = 'avx2'
  elif mi_arch.startswith('riscv')
    mi_opt_arch = 'rv@0@gcb_zacas'.format(mi_arch_bits) # fast bit scan and atomics (since 2023)
  else
    mi_opt_arch = 'off'
  endif
endif

mi_opt_arch_flags = []
if mi_opt_arch != 'off'
  if gcc_syntax and not is_haiku
    if mi_opt_arch == 'avx2'
      mi_opt_arch_flags = ['-march=haswell', '-mavx2'] # fast bit scan (since 2013)
    elif mi_opt_arch.startswith('armv8') or mi_opt_arch.startswith('armv9')
      mi_opt_arch_flags = ['-march=@0@-a'.format(mi_opt_arch)] # fast atomics
    elif mi_opt_arch.startswith('rva')
      mi_opt_arch_flags = ['-march=@0@u@1@'.format(mi_opt_arch, mi_arch_bits)]
    else
      mi_opt_arch_flags = ['-march=' + mi_opt_arch]
    endif
  elif cc_id == 'msvc' and cc.version().version_compare('>=19.14')
    if mi_opt_arch == 'avx2'
      mi_opt_arch_flags = ['/arch:AVX2']
    elif mi_opt_arch.startswith('armv8') or mi_opt_arch.startswith('armv9')
      mi_opt_arch_flags = ['/arch:' + mi_opt_arch] # fast atomics
    else
      warning('unknown mi_opt_arch=@0@ (ignored)'.format(mi_opt_arch))
    endif
  endif
endif

if is_mingw
  mi_cflags += '-D_WIN32_WINNT=0x600' # issue #976
endif

if mi_opt_arch_flags.length() > 0
  mi_cflags += mi_opt_arch_flags
  message(
    'Architecture specific optimization is enabled (with @0@) (mi_opt_arch=@1@)'.format(
      ' '.join(mi_opt_arch_flags),
      mi_opt_arch,
    ),
  )
  if get_option('mi_opt_simd')
    mi_defines += 'MI_OPT_SIMD=1'
    message('SIMD instructions are enabled (mi_opt_simd=true)')
  endif
elif get_option('mi_opt_simd')
  message(
    'SIMD instructions are not enabled (either mi_opt_arch=off or this architecture has no SIMD support)',
  )
endif

# Extra libraries
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
# Install and output names
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
if is_secure
  mi_libname += '-secure'
endif
if mi_track == 'valgrind'
  mi_libname += '-valgrind'
endif
if mi_track == 'asan'
  mi_libname += '-asan'
endif

# Map the Meson build types to the CMake names (the sources print them)
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
# CMake also passes the define to the static builds on Windows, where it
# replaces the allocation functions of a statically linked CRT. That code
# needs a Windows SDK that defines _CRT_HYBRIDPATCHABLE, which not every SDK
# does, so on Windows malloc is only overridden by the DLL here.
override_args_static = (mi_override and not is_windows) ? ['-DMI_MALLOC_OVERRIDE'] : []

message('')
message('Library name     : ' + mi_libname)
message('Version          : ' + mi_version_full)
message('Build type       : ' + buildtype_lc)
message(
  'Compiler         : ' + (mi_use_cxx ? cpp.get_id() + ' (C++)' : cc_id),
)
message('Compiler flags   : @0@'.format(mi_cflags))
message('Compiler defines : @0@'.format(mi_defines))
message('Link args        : @0@'.format(mi_link_args))
message('')

# With sanitizers their symbols are only resolved in the program; Meson links
# shared libraries with --no-undefined by default (CMake does not), so turn
# that off.
if mi_track == 'asan' or mi_debug_tsan or mi_debug_ubsan
  tgt_overrides += 'b_lundef=false'
endif

inc = include_directories('include')

# -----------------------------------------------------------------------------
# Main targets
# -----------------------------------------------------------------------------

# Shared library
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
    # copy the DLL into the build directory and install it as well
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

# Static library
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

# Headers
install_headers(
  'include/mimalloc.h',
  'include/mimalloc-override.h',
  'include/mimalloc-new-delete.h',
  'include/mimalloc-stats.h',
  subdir: mi_install_incsubdir,
)

# Single object file (src/static.c) for predictable static overriding.
# Meson has no object libraries; other targets can use the objects through
# `mimalloc_obj` (extract_all_objects), e.g. executable(..., objects: mimalloc_obj).
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

# Dependency objects for use as a Meson subproject
if mi_build_shared
  mimalloc_dep = declare_dependency(
    link_with: mimalloc_shared,
    include_directories: inc,
    version: mi_version_full,
  )
  meson.override_dependency('mimalloc', mimalloc_dep)
endif
if mi_build_static
  mimalloc_static_dep = declare_dependency(
    link_with: mimalloc_static,
    include_directories: inc,
    dependencies: mi_deps,
    compile_args: ['-DMI_STATIC_LIB'],
    version: mi_version_full,
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
    version: mi_version_full,
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
    foreach name : [
      'api',
      'api-fill',
      'stress-heaps',
      'stress-subprocs',
      'stress',
      'profile',
    ]
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
      if is_guarded and name != 'profile'
        test(
          'test-' + name,
          exe,
          env: {'MIMALLOC_GUARDED_SAMPLE_RATE': '1024'},
          timeout: 300,
        )
      elif name == 'stress-heaps'
        test(
          'test-' + name,
          exe,
          env: {
            'MIMALLOC_ARENA_EAGER_COMMIT': '0',
            'MIMALLOC_PAGE_COMMIT_ON_DEMAND': '0',
          },
          timeout: 300,
        )
      elif name != 'profile' or mi_profile != 'off'
        test('test-' + name, exe, timeout: 300)
      endif
    endforeach
  endif

  # dynamic override test
  if mi_build_shared and mi_override and not (mi_track == 'asan' or mi_debug_tsan or mi_debug_ubsan)
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
        dependencies: mi_deps, # pthreads, issue 1158
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
