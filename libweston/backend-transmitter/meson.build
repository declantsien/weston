if not get_option('backend-transmitter')
	subdir_done()
endif

config_h.set('BUILD_TRANSMITTER_COMPOSITOR', '1')

srcs_transmitter = [
	'transmitter.c',
	'transmitter-fb.c',
	'transmitter-input.c',
	linux_dmabuf_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_server_protocol_h,
	presentation_time_server_protocol_h,
]

deps_transmitter = [
	dep_libweston,
	dep_session_helper,
	dep_libdrm,
	dep_libinput_backend,
	dependency('libudev', version: '>= 136'),
	dependency('waltham', version: '>=0.1.0'),
]

dep_gbm = dependency('gbm', required: true)
	if dep_gbm.version().version_compare('>= 17.1')
		config_h.set('HAVE_GBM_MODIFIERS', '1')
	endif
	if dep_gbm.version().version_compare('>= 17.2')
		config_h.set('HAVE_GBM_FD_IMPORT', '1')
	endif
	deps_transmitter += dep_gbm

if dep_libdrm.version().version_compare('>= 2.4.71')
	config_h.set('HAVE_DRM_ADDFB2_MODIFIERS', '1')
endif

plugin_transmitter = shared_library(
	'transmitter-backend',
	srcs_transmitter,
	include_directories: include_directories('../..', '../../shared','../../libweston/renderer-waltham'),
	dependencies: deps_transmitter,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'transmitter-backend.so=@0@;'.format(plugin_transmitter.full_path())

install_headers(backend_transmitter_h, subdir: dir_include_libweston_install)
