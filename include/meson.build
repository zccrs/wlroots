subdir('wlr')

exclude_files = ['meson.build', 'config.h.in', 'version.h.in']
if not features.get('x11-backend')
	exclude_files += 'backend/x11.h'
endif
if not features.get('xwayland')
	exclude_files += 'xwayland.h'
else
	subdir('xwayland')
endif
if not features.get('xdg-foreign')
	exclude_files += [
		'types/wlr_xdg_foreign_v1.h',
		'types/wlr_xdg_foreign_v2.h',
		'types/wlr_xdg_foreign_registry.h',
	]
endif

header_dir = get_option('includedir') / get_option('includedir-subdir')
private_header_dir = header_dir

install_subdir('wlr',
	install_dir: header_dir,
	exclude_files: exclude_files,
)

#private headers
install_subdir('backend',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)

install_subdir('render',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)

install_subdir('types',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)

install_subdir('util',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)

install_subdir('xcursor',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)

install_subdir('xwayland',
	install_dir: private_header_dir,
	exclude_files: exclude_files,
)
