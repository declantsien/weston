if get_option('shell-ivi')
	srcs_shell_ivi = [
		'ivi-shell.c',
		'ivi-layout.c',
		'ivi-layout-transition.c',
		ivi_application_server_protocol_h,
		ivi_application_protocol_c,
		input_method_unstable_v1_server_protocol_h,
		input_method_unstable_v1_protocol_c,
	]
	plugin_shell_ivi = shared_library(
		'ivi-shell',
		srcs_shell_ivi,
		include_directories: common_inc,
		dependencies: [
			dep_libm,
			dep_libexec_weston,
			dep_libweston_public,
			dep_libshared
		],
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston,
		install_rpath: '$ORIGIN'
	)
	env_modmap += 'ivi-shell.so=@0@;'.format(plugin_shell_ivi.full_path())

	install_headers('ivi-layout-export.h', subdir: 'weston')

	srcs_ivi_hmi = [
		'hmi-controller.c',
		ivi_hmi_controller_server_protocol_h,
		ivi_hmi_controller_protocol_c,
	]
	plugin_ivi_hmi = shared_library(
		'hmi-controller',
		srcs_ivi_hmi,
		include_directories: common_inc,
		dependencies: [
			dep_libexec_weston,
			dep_libweston_public,
			dep_libshared
		],
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston,
		install_rpath: '$ORIGIN'
	)
	env_modmap += 'hmi-controller.so=@0@;'.format(plugin_ivi_hmi.full_path())

	swig = find_program('swig')
	lua_api = custom_target('lua-api.c',
		input: 'lua-controller.i',
		output: ['lua-api.c'],
		command: [swig, '-lua', '-o', '@OUTPUT@', '@INPUT@']
	)

	dep_lua = dependency('lua-5.4')
	srcs_ivi_lua = [
		'lua-controller.c',
		lua_api,
	]
	plugin_ivi_lua = shared_library(
		'lua-controller',
		srcs_ivi_lua,
		include_directories: common_inc,
		dependencies: [
			dep_libexec_weston,
			dep_libweston_public,
			dep_libshared,
			dep_lua,
		],
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston,
		install_rpath: '$ORIGIN'
	)
	env_modmap += 'lua-controller.so=@0@;'.format(plugin_ivi_lua.full_path())

	lua_controller_config = configuration_data()
	lua_controller_config.set('westondatadir', dir_data / 'weston')
	configure_file(
		input: 'lua-controller.ini.in',
		output: 'lua-controller.ini',
		configuration: lua_controller_config
	)
	install_data(
		'script.lua',
		install_dir: dir_data / 'weston'
	)

	ivi_test_config = configuration_data()
	ivi_test_config.set('bindir', dir_bin)
	ivi_test_config.set('westondatadir', dir_data / 'weston')
	configure_file(
		input: 'weston.ini.in',
		output: 'weston.ini',
		configuration: ivi_test_config
	)
endif
