# Meson port of the library part of liburing's configure and src/Makefile
# (2.15): only the static liburing, without the ffi variant, the tests, the
# examples and the man pages.
project(
  'liburing',
  'c',
  version: '2.15',
  license: '(MIT OR LGPL-2.1-only) AND (MIT OR GPL-2.0-only WITH Linux-syscall-note)',
  meson_version: '>=1.1.0',
  default_options: ['c_std=gnu11', 'warning_level=0'],
)

if host_machine.system() != 'linux'
  error('liburing is for Linux only')
endif

cc = meson.get_compiler('c')
version = meson.project_version().split('.')

# The checks of configure that compat.h depends on.
has_kernel_rwf_t = cc.has_type('__kernel_rwf_t', prefix: '#include <linux/fs.h>')
has_kernel_timespec = cc.has_members(
  'struct __kernel_timespec',
  'tv_sec',
  'tv_nsec',
  prefix: '#include <linux/time_types.h>',
)
has_open_how = cc.has_type('struct open_how', prefix: '#include <linux/openat2.h>')
has_statx = cc.has_type(
  'struct statx',
  prefix: '#include <sys/types.h>\n#include <sys/stat.h>\n#include <unistd.h>\n#include <fcntl.h>\n#include <linux/stat.h>',
)
has_glibc_statx = cc.has_type(
  'struct statx',
  prefix: '#include <sys/types.h>\n#include <sys/stat.h>\n#include <unistd.h>\n#include <fcntl.h>',
)
has_futexv = cc.has_type('struct futex_waitv', prefix: '#include <linux/futex.h>')
has_idtype_t = cc.has_type('idtype_t', prefix: '#include <sys/wait.h>')
has_discard_cmd = cc.has_header_symbol('linux/blkdev.h', 'BLOCK_URING_CMD_DISCARD')

# compat.h as configure writes it.
compat = ['/* SPDX-License-Identifier: MIT */', '#ifndef LIBURING_COMPAT_H', '#define LIBURING_COMPAT_H', '']
if not has_kernel_rwf_t
  compat += ['typedef int __kernel_rwf_t;', '']
endif
compat += [
  '#if defined(__has_include)',
  '#if __has_include("linux/time_types.h")',
  '#include <linux/time_types.h>',
  '#else',
  'struct __kernel_timespec {',
  '	int64_t		tv_sec;',
  '	long long	tv_nsec;',
  '};',
  '#endif',
  '',
  '#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1',
  '#endif',
  '',
]
if has_kernel_timespec
  compat += [
    '#if !defined(__has_include)',
    '#include <linux/time_types.h>',
    '#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1',
    '#endif',
  ]
else
  compat += [
    '#if !defined(__has_include)',
    '#include <stdint.h>',
    '',
    'struct __kernel_timespec {',
    '	int64_t		tv_sec;',
    '	long long	tv_nsec;',
    '};',
    '',
    '#define UAPI_LINUX_IO_URING_H_SKIP_LINUX_TIME_TYPES_H 1',
    '#endif',
  ]
endif
if has_open_how
  compat += ['#include <linux/openat2.h>', '']
else
  compat += [
    '#include <inttypes.h>',
    '',
    'struct open_how {',
    '	uint64_t	flags;',
    '	uint64_t	mode;',
    '	uint64_t	resolve;',
    '};',
    '',
  ]
endif
if not has_glibc_statx and has_statx
  compat += ['#include <sys/stat.h>', '']
endif
if not has_futexv
  compat += [
    '#include <inttypes.h>',
    '',
    '#define FUTEX_32	2',
    '#define FUTEX_WAITV_MAX	128',
    '',
    'struct futex_waitv {',
    '	uint64_t	val;',
    '	uint64_t	uaddr;',
    '	uint32_t	flags;',
    '	uint32_t	__reserved;',
    '};',
    '',
  ]
endif
if not has_idtype_t
  compat += ['typedef enum', '{', '  P_ALL,', '  P_PID,', '  P_PGID', '} idtype_t;']
endif
if has_discard_cmd
  compat += ['#include <linux/blkdev.h>', '']
else
  compat += [
    '',
    '#include <linux/ioctl.h>',
    '',
    '#ifndef BLOCK_URING_CMD_DISCARD',
    '#define BLOCK_URING_CMD_DISCARD                        _IO(0x12, 0)',
    '#endif',
    '',
  ]
endif
compat += ['#endif', '']

# Generated under generated/liburing in the build directory, so that
# "liburing/compat.h" is found through the include directory generated.
subdir('generated/liburing')

sources = files('src/queue.c', 'src/register.c', 'src/setup.c', 'src/syscall.c', 'src/version.c')
c_args = ['-D_GNU_SOURCE', '-D_LARGEFILE_SOURCE', '-D_FILE_OFFSET_BITS=64', '-DLIBURING_INTERNAL', '-fno-stack-protector']

# As configure --enable-sanitizer: tells ASan about the memory the kernel
# writes into.
sanitize = get_option('b_sanitize')
if 'address' in sanitize or 'undefined' in sanitize
  sources += files('src/sanitize.c')
  c_args += '-DCONFIG_USE_SANITIZER'
endif

inc = include_directories('src/include', 'generated')

liburing_lib = static_library(
  'uring',
  sources,
  include_directories: [inc, include_directories('src')],
  c_args: c_args,
)

# As system headers for whoever uses it: they are C with GNU extensions,
# which -Wpedantic in C++ would warn about.
liburing_dep = declare_dependency(
  link_with: liburing_lib,
  include_directories: include_directories('src/include', 'generated', is_system: true),
)
meson.override_dependency('liburing', liburing_dep)
