project('vxgproxyclient', ['c', 'cpp'],   version : '1.0.4',
  meson_version : '>= 0.56.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debug',
                      'auto_features=disabled',
                      'default_library=static',
                      'cpp_std=c++11', 'cpp_link_args=-static-libstdc++' ])

if not meson.is_subproject()
  # Install the git pre-commit hook if we are in standalone build
  git_hook = run_command(join_paths(meson.source_root(),
                                    'scripts/pre-commit-hook'), 'check-installed')
  if git_hook.returncode() == 0
    message('Git pre-commit hook installed')
  else
    message('Installing Git pre-commit hook')
    run_command(join_paths(meson.source_root(),
      'scripts/pre-commit-hook'), 'install')
  endif
endif

vxgproxyclient_version = meson.project_version()

if meson.is_cross_build()
  native = false
else
  native = true
endif
have_cc = add_languages('c', required : true, native: native)
cc = meson.get_compiler('c', native: native)
have_cxx = add_languages('cpp', required : true, native: native)
cxx = meson.get_compiler('cpp', native: native)

# config.h generation, dlfcn.h and asinh() just as an example
cdata = configuration_data()
check_headers = [
    ['HAVE_DLFCN_H', 'dlfcn.h']
]

foreach h : check_headers
  if cc.has_header(h.get(1))
    cdata.set(h.get(0), 1)
  else
    cdata.set(h.get(0), 0)
  endif
endforeach

bt_lib = ''
if cc.has_header('execinfo.h')
  message('C compiler has execinfo.h')
elif get_option('backtrace_lib') != ''
  bt_lib = get_option('backtrace_lib')
  message('C compiler has no execinfo.h, external implementation ' + bt_lib +
      ' will be used')
else
  message('backtrace() will not be used')
  cdata.set('LOGURU_STACKTRACES', 0)
endif

check_functions = [
    ['HAVE_ASINH', 'asinh', '#include<math.h>'],
]

libm = cc.find_library('m', required : false)

foreach f : check_functions
  if cc.has_function(f.get(1), prefix : f.get(2), dependencies : libm)
    cdata.set(f.get(0), 1)
  endif
endforeach

# SSL related settings
cdata.set_quoted('OPENSSL_CIPHERS_LIST', get_option('OPENSSL_CIPHERS_LIST'))
if get_option('SSL_ALLOW_INSECURE_CERTS')
  cdata.set('SSL_ALLOW_INSECURE_CERTS', 1)
else
  cdata.set('SSL_ALLOW_INSECURE_CERTS', 0)
endif

cdata.set('HAVE_CPP_LIB_FILESYSTEM', cc.find_library('stdc++fs', required : false).found())

if get_option('SSL_CERTS_BUNDLE_FILE') != ''
  cdata.set_quoted('SSL_CERTS_BUNDLE_FILE', get_option('SSL_CERTS_BUNDLE_FILE'))
endif

configure_file(output : 'config.h', configuration : cdata)

# Warnings
warning_flags = [
  '-Wmissing-declarations',
  '-Wredundant-decls',
  '-Wwrite-strings',
  '-Winit-self',
  '-Wmissing-include-dirs',
  '-Wno-multichar',
  '-Wvla',
  '-Wpointer-arith',
  # FIXME:
  '-Wno-reorder',
  '-Wno-write-strings',
  '-Wno-unused-function',
  '-Wno-missing-include-dirs',
  '-Wno-unused-function',
  '-Wno-psabi',
  '-Wno-missing-declarations',
  '-Wno-unused-function'
]

warning_c_flags = [
  '-Wmissing-prototypes',
  '-Wdeclaration-after-statement',
  '-Wold-style-definition',
  '-Waggregate-return',
]

foreach extra_arg : warning_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
  if have_cxx and cxx.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'cpp')
  endif
endforeach

foreach extra_arg : warning_c_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

# Disables usage of g++5.0+ locale facets in the date library
# We use date.h header so we need to define this flag globally
if cxx.version().version_compare('< 5')
  add_project_arguments(['-DONLY_C_LOCALE=1'], language: 'cpp')
endif

core_srcs = ['src/agent/Proxy.cpp']

subdir('src')

websockets = dependency('libwebsockets',
    fallback: ['libwebsockets', 'libwebsockets_dep'], required: true)

json = dependency('libcjson',
    fallback: ['libcjson', 'libcjson_dep'], required: true)

threads = dependency('threads', required: true)

# spdlog = dependency('spdlog',
#     fallback: ['spdlog', 'spdlog_dep'], required: true)

# dl = cc.find_library('dl', required : false)
# cpp11fs = cc.find_library('stdc++fs', required : false)

deps = [ websockets, json, threads ]
# websockets, spdlog, threads, json, date, args, cpp11fs, dl

if meson.is_cross_build() and bt_lib != ''
  deps += dependency(bt_lib)
  message('Using backtrace lib @0@'.format(bt_lib))
endif

vxgproxyclient_includes = include_directories(['.', 'src/'])
vxgproxyclient = library('vxgproxyclient',
    core_srcs,
    dependencies: deps,
    include_directories: [vxgproxyclient_includes],
    install: true)
vxgproxyclient_dep = declare_dependency(
    include_directories: [vxgproxyclient_includes],
    link_with: vxgproxyclient,
    dependencies: deps)

if not meson.is_subproject()
  pkg = import('pkgconfig')
  pkg.generate(vxgproxyclient)
endif

#
# tests
#
#if meson.is_cross_build() == false and not meson.is_subproject()
#  subdir('src/tests')
#endif

incdir = include_directories('.', 'src/')

linker_args = []

foreach entry : meson.get_cross_property('linker_paths', [''])
    if entry != ''
        linker_args += '-L' + meson.source_root() / entry
    endif
endforeach

if not meson.is_subproject()

# //! [meson-example-target]
  executable('vxg-proxy-client',
      sources: [ 'example/app/src/vxg_proxy_client.cc' ],
      dependencies : [vxgproxyclient_dep],
      include_directories : incdir,
      link_args : linker_args,
      install: true
  )
endif

if get_option('documentation')
  subdir('doc/')
endif
