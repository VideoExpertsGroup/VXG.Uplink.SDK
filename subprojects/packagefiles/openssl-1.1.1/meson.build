project('openssl', 'c',
  meson_version : '>=0.56.0', version: '1.1.1f'
)

openssl_dep = disabler()
if not dependency('openssl', fallback: [], required: false).found()
  # When we failed to find openssl in the system using pkg-config or cmake
  # we are trying to find libs and headers manually.
  cc = meson.get_compiler('c')
  ssl_dep = dependency('ssl', required: false)
  # if pkg-config is not available, try heuristically grabbing libs
  if not ssl_dep.found()
      ssl_dep = cc.find_library('ssl', has_headers: ['openssl/ssl.h'], required: false)
  endif
  crypto_dep = dependency('crypto', required: false)
  # if pkg-config is not available, try heuristically grabbing libs
  if not crypto_dep.found()
      crypto_dep = cc.find_library('crypto', has_headers: ['openssl/crypto.h'], required: false)
  endif

  if crypto_dep.found() and ssl_dep.found()
    openssl_dep = declare_dependency(version: 'Unknown-Libs', dependencies: [ssl_dep, crypto_dep])
  endif
endif

# If we didn't find openssl using pkg-config or cmake and we didn't find the libs and headers manually we building it by
# our own
if not openssl_dep.found()
  mod = import('unstable_external_project')

  configure_opts = []

  if not meson.is_cross_build()
    # Native
    configure_opts += host_machine.system() + '-' + host_machine.cpu()
  else
    # Cross compile
    if host_machine.cpu_family() == 'arm'
        configure_opts += 'linux-armv4'
    elif host_machine.cpu_family() == 'aarch64'
        configure_opts += 'linux-aarch64'
    elif host_machine.cpu_family() == 'mips'
        configure_opts += 'linux-mips32'
        # This is important line, openssl will not work without specifying
        # kernel syscall number for getrandom, random seed source initializaiton
        # will fail.
        # This was obtained from the openssl on device using 'openssl version -f'
        configure_opts += '-D__NR_getrandom=4353'
    else
      error('Platform @0@ not supported'.format(host_machine.cpu_family()))
    endif
  endif

  configure_opts += [
      'no-shared',
      'no-unit-test',
      'no-dynamic-engine',
      '--config=/etc/ssl/openssl.cnf',
      '--openssldir=/usr/lib/ssl-1.1',
      '--with-rand-seed=getrandom',
      '--prefix=@PREFIX@',
      '--libdir=@LIBDIR@',
      '-Ddummy_incdir=@INCLUDEDIR@',
  ]

  p = mod.add_project('Configure',
    configure_options: configure_opts, cross_configure_options: [], verbose: true)

  crypto_dep = p.dependency('crypto')
  ssl_dep = p.dependency('ssl')

  openssl_dep = declare_dependency(
    include_directories: include_directories('include'),
    dependencies: [ssl_dep, crypto_dep],
    version: meson.project_version()
  )
endif