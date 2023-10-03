project('test_app', ['c', 'cpp'],
  version : '1.0.4',
  meson_version : '>= 0.56.0',
  default_options : [ 'warning_level=1',
                      'buildtype=debug',
                      'auto_features=disabled',
                      'default_library=static',
                      'cpp_std=c++11', 'cpp_link_args=-static-libstdc++' ])

vxgproxyclient = dependency('vxgproxyclient')
# args = dependency('args')

dep = [vxgproxyclient]

# //! [meson-example-target]
vxg_proxy_client = executable('vxg-proxy-client', 'src/vxg_proxy_client.cc',
  install : true, dependencies: dep)
