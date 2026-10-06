project(
  'glm',
  'cpp',
  version: '1.0.3',
  default_options: ['cpp_std=c++20'],
  meson_version: '>=1.1.0',
)

compiler = meson.get_compiler('cpp')

glm_args = []
glm_compile_args = []

# Language extensions
if get_option('lang_extensions')
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-fms-extensions'
  endif
else
  if compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += ['/Za', '/permissive-']
  endif
endif

# Disable platform/compiler auto-detection
if get_option('disable_auto_detection')
  glm_args += [
    '-DGLM_FORCE_PLATFORM_UNKNOWN',
    '-DGLM_FORCE_COMPILER_UNKNOWN',
    '-DGLM_FORCE_ARCH_UNKNOWN',
    '-DGLM_FORCE_CXX_UNKNOWN',
  ]
endif

# Fast math
if get_option('fast_math')
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-ffast-math'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/fp:fast'
  endif
else
  if compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/fp:precise'
  endif
endif

# SIMD configuration
if get_option('force_pure')
  glm_args += '-DGLM_FORCE_PURE'
  if compiler.get_id() == 'gcc'
    glm_compile_args += '-mfpmath=387'
  endif
elif get_option('simd_avx2')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-mavx2'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:AVX2'
  endif
elif get_option('simd_avx')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-mavx'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:AVX'
  endif
elif get_option('simd_sse4_2')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-msse4.2'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:SSE4.2'
  endif
elif get_option('simd_sse4_1')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-msse4.1'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:SSE2'
  endif
elif get_option('simd_ssse3')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-mssse3'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:SSE2'
  endif
elif get_option('simd_sse3')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-msse3'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:SSE2'
  endif
elif get_option('simd_sse2')
  glm_args += '-DGLM_FORCE_INTRINSICS'
  if compiler.get_id() in ['clang', 'gcc']
    glm_compile_args += '-msse2'
  elif compiler.get_id() == 'msvc' or compiler.get_argument_syntax() == 'msvc'
    glm_compile_args += '/arch:SSE2'
  endif
elif get_option('simd_neon')
  glm_args += '-DGLM_FORCE_INTRINSICS'
endif

glm_inc = include_directories('.')

if get_option('build_library')
  glm_lib = library(
    'glm',
    'glm/detail/glm.cpp',
    include_directories: glm_inc,
    cpp_args: glm_args + glm_compile_args,
    install: true,
  )

  glm_dep = declare_dependency(
    include_directories: glm_inc,
    compile_args: glm_args + glm_compile_args,
    link_with: glm_lib,
  )
else
  glm_dep = declare_dependency(include_directories: glm_inc, compile_args: glm_args + glm_compile_args)
endif
