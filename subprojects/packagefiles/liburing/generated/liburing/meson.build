configure_file(
  input: 'compat.h.in',
  output: 'compat.h',
  configuration: configuration_data({'CONTENT': '\n'.join(compat)}),
)
configure_file(
  input: 'io_uring_version.h.in',
  output: 'io_uring_version.h',
  configuration: configuration_data({'MAJOR': version[0], 'MINOR': version[1]}),
)
