project('unordered_dense', 'cpp',
    version: '4.8.1',
    license: 'MIT',
    default_options : [
        'cpp_std=c++17', 
        'warning_level=3',
        'werror=true', 
        'b_ndebug=true', # otherwise absl is really slow!
    ])

unordered_dense_dep = declare_dependency(
  include_directories: include_directories('include')
)
