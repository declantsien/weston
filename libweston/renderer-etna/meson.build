if not get_option('renderer-etna')
	subdir_done()
endif

config_h.set('ENABLE_ETNA', '1')

srcs_renderer_etna = [
	'etna-renderer.c',
]

dep_libdrm_etnaviv = dependency('libdrm_etnaviv', version: '>= 2.4.108')

deps_renderer_etna = [
	dep_pixman,
	dep_libdrm,
	dep_libdrm_etnaviv,
	dep_libweston_private,
	dep_libdrm_headers,
]

plugin_etna = shared_library(
	'etna-renderer',
	srcs_renderer_etna,
	include_directories: common_inc,
	dependencies: deps_renderer_etna,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'etna-renderer.so=@0@;'.format(plugin_etna.full_path())
