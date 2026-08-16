project(
  'hstex',
  'c',
  version: '0.0.1',
  meson_version: '>=1.3.0',
  default_options: [
    'c_std=c17',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
warning_args = cc.get_supported_arguments(
  '-Wconversion',
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wformat=2',
)
add_project_arguments(
  warning_args,
  '-D_POSIX_C_SOURCE=200809L',
  language: 'c',
)

configuration = configuration_data()
configuration.set_quoted('HSTEX_VERSION', meson.project_version())
configure_file(output: 'hstex_config.h', configuration: configuration)

public_include = include_directories('include')
generated_include = include_directories('.')

core = static_library(
  'hstex_core',
  files(
    'src/catcode.c',
    'src/input.c',
    'src/lex.c',
    'src/mouth.c',
    'src/scan.c',
    'src/source.c',
    'src/symbol.c',
  ),
  include_directories: [public_include, generated_include],
  install: false,
)

hstex = executable(
  'hstex',
  'src/main.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  install: true,
)

scan_test = executable(
  'test_scan',
  'tests/test_scan.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('scan', scan_test)

input_test = executable(
  'test_input',
  'tests/test_input.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('input', input_test)

catcode_test = executable(
  'test_catcode',
  'tests/test_catcode.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('catcode', catcode_test)

symbol_test = executable(
  'test_symbol',
  'tests/test_symbol.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('symbol', symbol_test)

mouth_test = executable(
  'test_mouth',
  'tests/test_mouth.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('mouth', mouth_test)

source_test = executable(
  'test_source',
  'tests/test_source.c',
  include_directories: [public_include, generated_include],
  link_with: core,
)
test('source', source_test)

install_headers(
  'include/hstex/catcode.h',
  'include/hstex/input.h',
  'include/hstex/lex.h',
  'include/hstex/mouth.h',
  'include/hstex/scan.h',
  'include/hstex/source.h',
  'include/hstex/symbol.h',
  'include/hstex/token.h',
  subdir: 'hstex',
)
