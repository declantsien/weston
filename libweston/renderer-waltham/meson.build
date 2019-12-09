srcs_renderer_waltham = [
	'waltham-renderer.c',
]

deps_renderer_waltham = [
	dep_pixman,
	dep_libweston,
	dependency('waltham'),
	dep_wayland_server,
	dependency('gstreamer-1.0'),
	dependency('gstreamer-app-1.0'),
	dependency('gstreamer-video-1.0'),
	dependency('gstreamer-allocators-1.0'),
	dependency('gbm')
]

wrenderer = shared_library(
	'waltham-renderer',
	srcs_renderer_waltham,
	include_directories: include_directories('../..', '../../shared','.'),
	dependencies: deps_renderer_waltham,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
env_modmap += 'waltham-renderer.so=@0@;'.format(wrenderer.full_path())
