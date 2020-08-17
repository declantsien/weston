if not get_option('backend-headless')
	subdir_done()
endif

config_h.set('BUILD_HEADLESS_COMPOSITOR', '1')

srcs_headless = [
	'headless.c',
	presentation_time_server_protocol_h,
]

deps_headless = [
	dep_libweston_private,
	dep_libdrm_headers,
	dep_libdl
]

if get_option('renderer-gl')
	dep_gbm = dependency('gbm', required: false)
	if dep_gbm.found()
	      config_h.set('BUILD_HEADLESS_GBM', '1')
	endif
	deps_headless += dep_gbm
endif

plugin_headless = shared_library(
	'headless-backend',
	srcs_headless,
	include_directories: common_inc,
	dependencies: deps_headless,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
)
env_modmap += 'headless-backend.so=@0@;'.format(plugin_headless.full_path())
install_headers(backend_headless_h, subdir: dir_include_libweston_install)
