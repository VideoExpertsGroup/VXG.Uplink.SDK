prg_install = find_program('install')

doxygen = find_program('doxygen', required : false)
if not doxygen.found()
    error('Program "doxygen" not found or not executable. Try building with -Ddocumentation=false')
endif
dot = find_program('dot', required : false)
if not dot.found()
    error('Program "dot" not found or not executable. Try building with -Ddocumentation=false')
endif
pdflatex = find_program('pdflatex', required : false)
if not pdflatex.found()
    warning('Program "pdflatex" not found or not executable. No pdf documenation will be generated.')
endif

mainpage = vcs_tag(command : ['git', 'log', '-1', '--format=%h'],
            fallback : 'unknown',
            input : 'mainpage.md',
            output : 'mainpage.md',
            replace_string: '__GIT_VERSION__')

src_doxygen = [
    # source files
    '../example/app/src/vxg_proxy_client.cc',
    '../src/agent/Proxy.h',
    '../src/agent/Proxy.cpp',
    # markdown pages
    'build-system.md',
    'app-dev.md',
    'compile.md',
    # examples
    '../meson.build'
]

files_str = ''
foreach f : src_doxygen
    files_str += meson.current_source_dir() + f
    files_str += ' '
endforeach
files_str += meson.current_build_dir() + 'mainpage.md'

doc_config = configuration_data()
doc_config.set('PACKAGE_NAME', meson.project_name())
doc_config.set('PACKAGE_VERSION', meson.project_version())
doc_config.set('TOP_BUILDDIR', meson.current_build_dir())
doc_config.set('INPUT', files_str)
doc_config.set('INC_STRIP', join_paths(meson.project_source_root(), 'src'))
# TODO: Make example
# doc_config.set('EXAMPLES', meson.current_source_dir() + '../examples/')
doc_config.set('TOP_SRCDIR', meson.current_source_dir())

doxyfile = configure_file(input : 'libvxgproxyclient.doxygen.in',
            output : 'libvxgproxyclient.doxygen',
            configuration : doc_config)

datadir = join_paths(get_option('datadir'), 'doc', 'libvxgproxyclient')
html_docs = custom_target('doxygen-html',
        input : [ doxyfile, mainpage ] + src_doxygen,
        output : [ 'html', 'latex' ],
        command : [ doxygen, doxyfile ],
        install : false,
        install_dir : datadir,
        depends: [ mainpage ],
        build_by_default : true)

if pdflatex.found()
    pdf_docs = custom_target('doxygen-pdf',
        output: [ 'doc.pdf' ],
        command: [ 'make', '-C', meson.current_build_dir() / 'latex' ],
        depends: [ html_docs ],
        install : false,
        install_dir : datadir,
        build_by_default : true)
endif