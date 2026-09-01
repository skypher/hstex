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
zlib = dependency('zlib')
threads = dependency('threads')
core_dependencies = [zlib, threads]
libdeflate_dep = dependency('libdeflate', required: get_option('libdeflate'))
if libdeflate_dep.found()
  core_dependencies += [libdeflate_dep]
endif
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

# A build told which way its branches go is built from a profile taken over
# the engine alone; the tests were not run under it, and a test that has no
# profile of its own is not a fault. See tools/build-pgo.sh.
if get_option('b_pgo') == 'use'
  add_project_arguments(
    cc.get_supported_arguments('-Wno-missing-profile'),
    language: 'c',
  )
endif

# Under link-time optimization gcc inlines the hyphenation loops into the
# paragraph builder, vectorizes their byte stores, and then reads a
# sixteen-byte store as running past the 64-byte array the caller passes --
# a length hstex_engine_hyphenate_word refuses before any of it runs. What
# it is warning about is its own generated code seen imprecisely, and
# -Werror makes it fatal, so the warning is off where -flto is on.
if get_option('b_lto')
  lto_quiet = cc.get_supported_arguments('-Wno-stringop-overflow')
  add_project_arguments(lto_quiet, language: 'c')
  # The pass that raises it runs when the link runs, so the link is where
  # the answer has to be given too.
  add_project_link_arguments(lto_quiet, language: 'c')
endif

configuration = configuration_data()
configuration.set_quoted('HSTEX_VERSION', meson.project_version())
configuration.set10('HSTEX_HAVE_LIBDEFLATE', libdeflate_dep.found())
configure_file(output: 'hstex_config.h', configuration: configuration)

public_include = include_directories('include')
generated_include = include_directories('.')

core = static_library(
  'hstex_core',
  files(
    'src/catcode.c',
    'src/engine.c',
    'src/filedb.c',
    'src/format.c',
    'src/input.c',
    'src/lex.c',
    'src/mouth.c',
    'src/pk.c',
    'src/scan.c',
    'src/source.c',
    'src/symbol.c',
    'src/type1.c',
  ),
  include_directories: [public_include, generated_include],
  dependencies: core_dependencies,
  install: false,
)

hstex = executable(
  'hstex',
  'src/main.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
  install: true,
)

hstex_pdflatex = executable(
  'hstex-pdflatex',
  'src/pdflatex.c',
  include_directories: generated_include,
  install: true,
)

pdflatex_driver_test = find_program('tests/pdflatex/run-driver.sh')
test(
  'pdflatex_driver',
  pdflatex_driver_test,
  args: [hstex_pdflatex.full_path(), hstex.full_path()],
)

scan_test = executable(
  'test_scan',
  'tests/test_scan.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('scan', scan_test)

input_test = executable(
  'test_input',
  'tests/test_input.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('input', input_test)

catcode_test = executable(
  'test_catcode',
  'tests/test_catcode.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('catcode', catcode_test)

symbol_test = executable(
  'test_symbol',
  'tests/test_symbol.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('symbol', symbol_test)

mouth_test = executable(
  'test_mouth',
  'tests/test_mouth.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('mouth', mouth_test)

pk_test = executable(
  'test_pk',
  'tests/test_pk.c',
  include_directories: [public_include, generated_include, include_directories('src')],
  link_with: core,
  dependencies: core_dependencies,
)
test('pk', pk_test)

type1_test = executable(
  'test_type1',
  'tests/test_type1.c',
  include_directories: [
    public_include,
    generated_include,
    include_directories('src'),
  ],
  link_with: core,
  dependencies: core_dependencies,
)
test('type1', type1_test)

source_test = executable(
  'test_source',
  'tests/test_source.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('source', source_test)

filedb_test = executable(
  'test_filedb',
  'tests/test_filedb.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('filedb', filedb_test)

engine_test = executable(
  'test_engine',
  'tests/test_engine.c',
  include_directories: [public_include, generated_include],
  link_with: core,
  dependencies: core_dependencies,
)
test('engine', engine_test, env: ['HSTEX_FONT_WORKERS=4'])
test('engine_serial_fonts', engine_test, env: ['HSTEX_FONT_WORKERS=1'])

install_headers(
  'include/hstex/catcode.h',
  'include/hstex/engine.h',
  'include/hstex/filedb.h',
  'include/hstex/input.h',
  'include/hstex/lex.h',
  'include/hstex/mouth.h',
  'include/hstex/scan.h',
  'include/hstex/source.h',
  'include/hstex/symbol.h',
  'include/hstex/token.h',
  subdir: 'hstex',
)

install_data(
  'README.md',
  'SOURCE_POLICY.md',
  'docs/COMPATIBILITY.md',
  'docs/RELEASING.md',
  install_dir: get_option('datadir') / 'doc' / meson.project_name(),
)

install_data(
  'docs/hstex-pdflatex.1',
  install_dir: get_option('mandir') / 'man1',
)
