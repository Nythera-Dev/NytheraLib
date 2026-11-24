project('nythera-lib', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=0',   # applied selectively to our target; avoids noise in vendored code
    'b_ndebug=if-release',
    'pkgconfig.relocatable=true',
  ],
)

cc  = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')


# Clang requires an opt-in flag to properly use experimental C++ libraries.
# GCC/libstdc++ doesn't need this since these libraries are unconditionally
# exposed.
is_libcxx = cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>' ) != ''
if is_libcxx
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

# Sanitizers are enabled via meson's built-in -Db_sanitize=address,undefined.
# lsan_suppressions.cpp is only needed when address sanitizer is active.
sanitize = get_option('b_sanitize') != 'none'

# Hardened standard-library assertions in debug builds only. A std::clamp(v, lo, hi)
# with hi < lo aborts here (distro packagers enable this in release, so users see
# the same abort), surfacing inverted-bound bugs loudly during development so they
# can be routed through util::clampOrdered. Release stays unhardened on purpose: a
# not-yet-converted raw std::clamp there should degrade to a wrong-but-non-fatal
# value rather than abort a user's session (converted sites log and return lo via
# clampOrdered). The cost is a few precondition branches (not the heavy _GLIBCXX_DEBUG).
if get_option('buildtype') != 'release'
  if is_libcxx
    add_project_arguments('-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE', language: 'cpp')
  else
    add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
  endif
endif

# ── Development build optimizations (LLD, split/compressed DWARF) ────────────
# Keep release builds and packaging untouched (release uses GCC/Clang default
# linker for LTO compatibility and ships without split DWARF).
if get_option('buildtype') != 'release'
  _user_set_linker = false
  foreach arg : get_option('c_link_args') + get_option('cpp_link_args')
    if arg.startswith('-fuse-ld=')
      _user_set_linker = true
    endif
  endforeach

  _dev_linker_args = []
  if not _user_set_linker and not (cpp.get_id() == 'gcc' and get_option('b_lto'))
    if cpp.has_link_argument('-fuse-ld=lld') and cc.has_link_argument('-fuse-ld=lld')
      _dev_linker_args = ['-fuse-ld=lld']
      add_project_link_arguments('-fuse-ld=lld', language: ['c', 'cpp'])
    endif
  endif

  if get_option('debug')
    _dev_debug_flags = ['-gsplit-dwarf', '-gz=zstd']
    add_project_arguments(cc.get_supported_arguments(_dev_debug_flags), language: 'c')
    add_project_arguments(cpp.get_supported_arguments(_dev_debug_flags), language: 'cpp')
    if cpp.has_multi_link_arguments(_dev_linker_args + ['-Wl,--compress-debug-sections=zstd'])
      add_project_link_arguments('-Wl,--compress-debug-sections=zstd', language: ['c', 'cpp'])
    endif
  endif
endif

# ── Release-only optimizations ────────────────────────────────────────────────
if get_option('buildtype') == 'release'
  add_project_arguments(
    '-ffunction-sections', '-fdata-sections',
    language: ['c', 'cpp'],
  )
  add_project_link_arguments(
    '-Wl,--gc-sections', '-Wl,--as-needed',
    language: ['c', 'cpp'],
  )
endif

if get_option('native_optimizations')
  add_project_arguments('-march=native', '-mtune=native', language: ['c', 'cpp'])
endif

wayland_client_dep  = dependency('wayland-client')
wayland_protos_dep  = dependency('wayland-protocols')
#egl_dep             = dependency('egl')
#glesv2_dep          = dependency('glesv2')
wayland_egl_dep     = dependency('wayland-egl')
freetype2_dep       = dependency('freetype2')
fontconfig_dep      = dependency('fontconfig')
cairo_dep           = dependency('cairo')
cairo_ft_dep        = dependency('cairo-ft')
pango_dep           = dependency('pango', version: '>=1.50')
pangocairo_dep      = dependency('pangocairo')
pangoft2_dep        = dependency('pangoft2')
harfbuzz_dep        = dependency('harfbuzz')
librsvg_dep         = dependency('librsvg-2.0')
xkbcommon_dep       = dependency('xkbcommon')
glib_dep            = dependency('glib-2.0')
gobject_dep         = dependency('gobject-2.0')
gio_dep             = dependency('gio-2.0')
curl_dep            = dependency('libcurl')
md4c_dep            = dependency('md4c')
nlohmann_dep        = dependency('nlohmann_json')
tomlplusplus_dep    = dependency('tomlplusplus')
dl_dep              = cpp.find_library('dl', required: false)

egl_dep = dependency('egl', required: false)
gles2_dep = dependency('glesv2', required: false)


if not egl_dep.found()
  egl_dep = dependency('wayland-egl', required: false)
endif

if not egl_dep.found()
  egl_dep = dependency('epoxy')
endif

if not gles2_dep.found()
  gles2_dep = dependency('epoxy')
endif

# ── System: stb_image_resize2 and stb_image_write (header-only) ─────────────────────────────────
if not cc.has_header('stb/stb_image_resize2.h')
  error('stb/stb_image_resize2.h not found')
endif
if not cc.has_header('stb/stb_image_write.h')
  error('stb/stb_image_write.h not found')
endif

# ── System: libwebp (WebP decode; thumbnail and screenshot encode; demux for animations) ─
libwebp_dep       = dependency('libwebp', required: true)
libwebpdemux_dep  = dependency('libwebpdemux', required: true)

# ── System: libjxl (JPEG XL decode and screenshot encode) ─────────────────────
libjxl_dep          = dependency('libjxl', required: true)
libjxl_threads_dep  = dependency('libjxl_threads', required: true)

# Keep the existing system dependency option for the library's third-party code.
if get_option('system_libraries')
  if not cc.has_header('wuffs-v0.4.c')
    error('system_libraries: wuffs-v0.4.c not found')
  endif
  _wuffs_inc = []
  fzy_dep = cc.find_library('fzy', has_headers: ['fzy/match.h'])
  mcu_dep = cpp.find_library('material-color-utilities', has_headers: ['cpp/cam/hct.h'])
else
  _wuffs_inc = [include_directories('third_party/wuffs', is_system: true)]

  # ── Vendored: fzy matcher (C fuzzy scoring) ──────────────────────────────────
  # match.h sits under include/fzy/ to match the system install layout.
  _fzy_inc = include_directories('third_party/fzy/include', is_system: true)
  _fzy_lib = static_library('fzy',
    'third_party/fzy/src/match.c',
    include_directories: include_directories('third_party/fzy/include/fzy', is_system: true),
    c_args: ['-Wno-pedantic', '-Wno-conversion'],
    override_options: ['warning_level=0'],
    pic: true,
  )
  fzy_dep = declare_dependency(
    link_whole: _fzy_lib,
    include_directories: _fzy_inc,
  )

  # ── Vendored: Material Color Utilities (Google) ───────────────────────────────
  # Upstream uses #include "cpp/..." prefixes, so the include root is the
  # material_color_utilities/ dir itself. Built as a quiet static library.
  _mcu_sources = files(
    'third_party/material_color_utilities/cpp/blend/blend.cc',
    'third_party/material_color_utilities/cpp/cam/cam.cc',
    'third_party/material_color_utilities/cpp/cam/hct.cc',
    'third_party/material_color_utilities/cpp/cam/hct_solver.cc',
    'third_party/material_color_utilities/cpp/cam/viewing_conditions.cc',
    'third_party/material_color_utilities/cpp/contrast/contrast.cc',
    'third_party/material_color_utilities/cpp/dislike/dislike.cc',
    'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_color.cc',
    'third_party/material_color_utilities/cpp/dynamiccolor/dynamic_scheme.cc',
    'third_party/material_color_utilities/cpp/dynamiccolor/material_dynamic_colors.cc',
    'third_party/material_color_utilities/cpp/palettes/tones.cc',
    'third_party/material_color_utilities/cpp/quantize/celebi.cc',
    'third_party/material_color_utilities/cpp/quantize/lab.cc',
    'third_party/material_color_utilities/cpp/quantize/wsmeans.cc',
    'third_party/material_color_utilities/cpp/quantize/wu.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_content.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_expressive.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_fidelity.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_fruit_salad.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_monochrome.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_neutral.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_rainbow.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_tonal_spot.cc',
    'third_party/material_color_utilities/cpp/scheme/scheme_vibrant.cc',
    'third_party/material_color_utilities/cpp/score/score.cc',
    'third_party/material_color_utilities/cpp/temperature/temperature_cache.cc',
    'third_party/material_color_utilities/cpp/utils/utils.cc',
  )
  _mcu_lib = static_library('material_color_utilities',
    _mcu_sources,
    include_directories: include_directories('third_party/material_color_utilities', is_system: true),
    cpp_args: ['-Wno-pedantic', '-Wno-conversion', '-Wno-shadow', '-Wno-unused-parameter'],
    override_options: ['warning_level=0'],
    pic: true,
  )
  mcu_dep = declare_dependency(
    link_whole: _mcu_lib,
    include_directories: include_directories('third_party/material_color_utilities', is_system: true),
  )

endif

wayland_scanner = find_program('wayland-scanner')
wayland_protos_dir = wayland_protos_dep.get_variable('pkgdatadir')
_protocol_headers = []
_protocol_sources = []
foreach protocol : [
  ['xdg-output-unstable-v1', wayland_protos_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
  ['xdg-shell', wayland_protos_dir / 'stable/xdg-shell/xdg-shell.xml'],
  ['xdg-activation-v1', wayland_protos_dir / 'staging/xdg-activation/xdg-activation-v1.xml'],
  ['ext-background-effect-v1', wayland_protos_dir / 'staging/ext-background-effect/ext-background-effect-v1.xml'],
  ['fractional-scale-v1', wayland_protos_dir / 'staging/fractional-scale/fractional-scale-v1.xml'],
  ['viewporter', wayland_protos_dir / 'stable/viewporter/viewporter.xml'],
  ['text-input-unstable-v3', 'protocols/text-input-unstable-v3.xml'],
  ['cursor-shape-v1', 'protocols/cursor-shape-v1.xml'],
]
  name = protocol[0]
  xml = protocol[1]
  header = custom_target(name + '-client-header',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    install: true,
    install_dir: get_option('includedir') / 'nythera-lib',
  )
  _protocol_headers += header
  _protocol_sources += custom_target(name + '-client-source',
    input: xml,
    output: name + '-client-protocol.c',
    command: [wayland_scanner, 'public-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
endforeach

# wlr-layer-shell: 'namespace' is a C++ keyword; rename it in the generated
# header only (private-code is compiled as C so it's fine as-is).
_wlr_ls_xml = 'protocols/wlr-layer-shell-unstable-v1.xml'
_wlr_ls_h = custom_target('wlr-layer-shell-unstable-v1-client-header',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.h',
  install: true,
  install_dir: get_option('includedir') / 'nythera-lib',
  command: [
    'sh', '-c',
    '"$1" client-header "$2" "$3" && sed -i s/namespace/name_space/g "$3"',
    '_', wayland_scanner.full_path(), '@INPUT@', '@OUTPUT@',
  ],
)
_wlr_ls_c = custom_target('wlr-layer-shell-unstable-v1-client-source',
  input:   _wlr_ls_xml,
  output:  'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'public-code', '@INPUT@', '@OUTPUT@'],
  depends: _wlr_ls_h,
)
_protocol_sources += _wlr_ls_c
_protocol_headers += _wlr_ls_h


_nythera_lib_sources = files(
  'src/notification/notification_filter.cpp',
  'src/system/app_identity.cpp',
  'src/system/icon_resolver.cpp',
  'src/system/internal_app_metadata.cpp',
  'src/time/time_format.cpp',
  'src/system/freedesktop_key_file.cpp',
  'src/app/main_loop.cpp',
  'src/system/day_night_schedule.cpp',
  'src/shell/surface/shadow.cpp',
  'src/ui/tooltip/tooltip_manager.cpp',
  'src/util/fuzzy_match.cpp',
  'src/app/single_instance_lock.cpp',
  'src/config/atomic_file.cpp',
  'src/config/config_export.cpp',
  'src/config/config_merge.cpp',
  'src/config/config_migrations.cpp',
  'src/config/config_origins.cpp',
  'src/config/config_overrides.cpp',
  'src/config/config_service.cpp',
  'src/config/config_types.cpp',
  'src/config/widget_config.cpp',
  'src/config/schema/config_schema.cpp',
  'src/config/schema/config_sections.cpp',
  'src/config/state_store.cpp',
  'src/core/deferred_call.cpp',
  'src/core/process/process_fds.cpp',
  'src/core/files/file_watcher.cpp',
  'src/core/input/key_chord.cpp',
  'src/core/log.cpp',
  'src/core/process/process.cpp',
  'src/core/files/resource_paths.cpp',
  'src/core/inotify/inotify.cpp',
  'src/core/timer_manager.cpp',
  'src/core/ui_phase.cpp',
  'src/i18n/i18n.cpp',
  'src/i18n/language_tag.cpp',
  'src/i18n/i18n_service.cpp',
  'src/net/http_client.cpp',
  'src/net/uri.cpp',
  'src/net/url_open.cpp',
  'src/render/animation/animation.cpp',
  'src/render/animation/animation_manager.cpp',
  'src/render/animation/motion_service.cpp',
  'src/render/animation/surface_haze.cpp',
  'src/render/backend/gles_framebuffer.cpp',
  'src/render/backend/gles_render_backend.cpp',
  'src/render/backend/gles_texture_manager.cpp',
  'src/render/backend/render_backend.cpp',
  'src/render/core/async_texture_cache.cpp',
  'src/render/core/color.cpp',
  'src/render/core/image_file_loader.cpp',
  'src/render/core/shader_program.cpp',
  'src/render/core/stb_image_resize_impl.cpp',
  'src/render/core/thumbnail_service.cpp',
  'src/render/core/image_decoder.cpp',
  'src/render/core/heif_library.cpp',
  'src/render/core/image_encoder.cpp',
  'src/render/core/image_orientation.cpp',
  'src/render/core/image_resize.cpp',
  'src/render/programs/glyph_program.cpp',
  'src/render/programs/image_program.cpp',
  'src/render/programs/lockscreen_transition_program.cpp',
  'src/render/programs/linear_gradient_program.cpp',
  'src/render/programs/rect_program.cpp',
  'src/render/programs/screen_corner_program.cpp',
  'src/render/programs/screen_frame_program.cpp',
  'src/render/programs/countdown_ring_program.cpp',
  'src/render/programs/spinner_program.cpp',
  'src/render/programs/audio_spectrum_program.cpp',
  'src/render/programs/fancy_audio_visualizer_program.cpp',
  'src/render/programs/effect_program.cpp',
  'src/render/programs/graph_program.cpp',
  'src/render/programs/blur_program.cpp',
  'src/render/programs/wallpaper_program.cpp',
  'src/render/programs/wallpaper_mask_program.cpp',
  'src/render/core/blur_cache.cpp',
  'src/render/core/cached_layer.cpp',
  'src/render/core/shared_texture_cache.cpp',
  'src/render/gl_shared_context.cpp',
  'src/render/render_context.cpp',
  'src/render/render_target.cpp',
  'src/render/scene/input_area.cpp',
  'src/render/scene/input_dispatcher.cpp',
  'src/render/scene/fancy_audio_visualizer_node.cpp',
  'src/render/scene/graph_node.cpp',
  'src/render/scene/node.cpp',
  'src/render/text/cairo_text_renderer.cpp',
  'src/render/text/font_registry.cpp',
  'src/render/text/font_weight_catalog.cpp',
  'src/render/text/cairo_glyph_renderer.cpp',
  'src/render/wallpaper_renderer.cpp',
  'src/core/files/directory_scanner.cpp',
  'src/ui/dialogs/color_picker_dialog.cpp',
  'src/ui/dialogs/color_picker_dialog_popup.cpp',
  'src/ui/dialogs/dialog_popup_host.cpp',
  'src/ui/dialogs/file_dialog.cpp',
  'src/ui/dialogs/file_dialog_popup.cpp',
  'src/ui/dialogs/file_dialog_view.cpp',
  'src/ui/dialogs/file_entry_row.cpp',
  'src/ui/dialogs/file_entry_tile.cpp',
  'src/ui/dialogs/glyph_picker_dialog.cpp',
  'src/ui/dialogs/glyph_picker_dialog_popup.cpp',
  'src/ui/dialogs/layer_popup_host.cpp',
  'src/ui/dialogs/search_picker_popup.cpp',
  'src/ui/visuals/audio_visualizer.cpp',
  'src/ui/visuals/fancy_audio_visualizer.cpp',
  'src/ui/controls/box.cpp',
  'src/ui/controls/button.cpp',
  'src/ui/controls/calendar_view.cpp',
  'src/ui/controls/drag_source.cpp',
  'src/ui/controls/drop_zone.cpp',
  'src/ui/controls/checkbox.cpp',
  'src/ui/controls/color_picker.cpp',
  'src/ui/controls/collapsible.cpp',
  'src/ui/controls/context_menu.cpp',
  'src/ui/controls/context_menu_popup.cpp',
  'src/ui/controls/countdown_ring.cpp',
  'src/ui/controls/flex.cpp',
  'src/ui/controls/glyph.cpp',
  'src/ui/controls/graph.cpp',
  'src/ui/controls/glyph_picker.cpp',
  'src/render/text/glyph_registry.cpp',
  'src/ui/controls/grid_view.cpp',
  'src/ui/controls/grid_tile.cpp',
  'src/ui/controls/image.cpp',
  'src/ui/controls/lockscreen_transition_cover.cpp',
  'src/ui/controls/input.cpp',
  'src/core/input/keybind_matcher.cpp',
  'src/ui/controls/keybind_recorder.cpp',
  'src/ui/controls/label.cpp',
  'src/ui/controls/markdown_view.cpp',
  'src/ui/controls/list_editor.cpp',
  'src/ui/controls/color_swatch_preview.cpp',
  'src/ui/controls/progress_bar.cpp',
  'src/ui/controls/radio_button.cpp',
  'src/ui/controls/scrollbar.cpp',
  'src/ui/controls/scroll_view.cpp',
  'src/ui/controls/roving_list_nav.cpp',
  'src/ui/controls/search_picker.cpp',
  'src/ui/controls/range_slider.cpp',
  'src/ui/controls/screen_corner.cpp',
  'src/ui/controls/segmented.cpp',
  'src/ui/controls/select.cpp',
  'src/ui/controls/select_dropdown_popup.cpp',
  'src/ui/controls/separator.cpp',
  'src/ui/controls/slider.cpp',
  'src/ui/controls/spacer.cpp',
  'src/ui/controls/spinner.cpp',
  'src/ui/controls/stepper.cpp',
  'src/ui/controls/toggle.cpp',
  'src/ui/controls/virtual_grid_view.cpp',
  'src/ui/controls/virtual_list_view.cpp',
  'src/ui/builders.cpp',
  'src/ui/app_icon_colorization.cpp',
  'src/ui/palette.cpp',
  'src/ui/palette_swatch_preview.cpp',
  'src/ui/popup_chrome.cpp',
  'src/ui/scroll_into_view.cpp',
  'src/ui/split_pane_focus.cpp',
  'src/ui/style.cpp',
  'src/ui/drag_drop_controller.cpp',
  'src/util/checksum.cpp',
  'src/util/clamp.cpp',
  'src/wayland/native_data_transfer.cpp',
  'src/wayland/text_input_service.cpp',
  'src/wayland/layer_surface.cpp',
  'src/wayland/output_probe.cpp',
  'src/wayland/popup_surface.cpp',
  'src/wayland/subsurface.cpp',
  'src/wayland/surface.cpp',
  'src/wayland/toplevel_surface.cpp',
  'src/wayland/wayland_connection.cpp',
  'src/wayland/wayland_seat.cpp',
  'src/theme/scheme.cpp',
  'src/theme/color.cpp',
  'src/theme/contrast.cpp',
  'src/theme/image_loader.cpp',
  'src/theme/palette_generator.cpp',
  'src/theme/palette_transform.cpp',
  'src/theme/m3_schemes.cpp',
  'src/theme/custom_schemes.cpp',
  'src/theme/builtin_palettes.cpp',
  'src/theme/community_palettes.cpp',
  'src/theme/custom_palettes.cpp',
  'src/theme/fixed_palette.cpp',
  'src/theme/fixed_palette_json.cpp',
  'src/theme/theme_service.cpp',
  'src/cli/completions.cpp',
  'src/cli/parse.cpp',
  'src/cli/help.cpp',
)


if sanitize
  _nythera_lib_sources += files('src/core/lsan_suppressions.cpp')
endif

_nythera_system_dependencies = [
  wayland_client_dep,
  egl_dep,
  gles2_dep,
  wayland_egl_dep,
  freetype2_dep,
  fontconfig_dep,
  cairo_dep,
  cairo_ft_dep,
  pango_dep,
  pangocairo_dep,
  pangoft2_dep,
  harfbuzz_dep,
  librsvg_dep,
  xkbcommon_dep,
  glib_dep,
  gobject_dep,
  gio_dep,
  dependency('gio-unix-2.0'),
  curl_dep,
  nlohmann_dep,
  tomlplusplus_dep,
  md4c_dep,
  libwebp_dep,
  libwebpdemux_dep,
  libjxl_dep,
  libjxl_threads_dep,
] + (dl_dep.found() ? [dl_dep] : [])

_nythera_compile_definitions = [
  '-DNYTHERA_INSTALL_PREFIX="' + get_option('prefix') + '"',
  '-DNYTHERA_INSTALL_DATADIR="' + get_option('datadir') + '"',
  '-DNYTHERA_VERSION="' + meson.project_version() + '"',
]

_nythera_cpp_args = [
  '-Wall', '-Wextra', '-Wno-missing-field-initializers', '-Wpedantic', '-Wconversion', '-Wshadow',
] + _nythera_compile_definitions


nythera_lib = shared_library('nythera_lib',
  sources: _nythera_lib_sources + _protocol_sources + _protocol_headers,
  dependencies: _nythera_system_dependencies + [fzy_dep, mcu_dep],
  include_directories: [include_directories('src', '.')] + _wuffs_inc,
  cpp_args: _nythera_cpp_args,
  version: meson.project_version(),
  soversion: '0',
  install: true,
)

# Shell's template renderer also uses the library's Material Color Utilities.
# Install its headers with the implementation it was linked against.
pkg_extra_cflags = []
pkg_extra_libraries = []
if not get_option('system_libraries')
  install_subdir('third_party/material_color_utilities/cpp',
    install_dir: get_option('includedir') / 'nythera-lib' / 'detail',
    exclude_files: [
      'blend/blend.cc',
      'cam/cam.cc',
      'cam/hct.cc',
      'cam/hct_solver.cc',
      'cam/viewing_conditions.cc',
      'contrast/contrast.cc',
      'dislike/dislike.cc',
      'dynamiccolor/dynamic_color.cc',
      'dynamiccolor/dynamic_scheme.cc',
      'dynamiccolor/material_dynamic_colors.cc',
      'palettes/tones.cc',
      'quantize/celebi.cc',
      'quantize/lab.cc',
      'quantize/wsmeans.cc',
      'quantize/wu.cc',
      'scheme/scheme_content.cc',
      'scheme/scheme_expressive.cc',
      'scheme/scheme_fidelity.cc',
      'scheme/scheme_fruit_salad.cc',
      'scheme/scheme_monochrome.cc',
      'scheme/scheme_neutral.cc',
      'scheme/scheme_rainbow.cc',
      'scheme/scheme_tonal_spot.cc',
      'scheme/scheme_vibrant.cc',
      'score/score.cc',
      'temperature/temperature_cache.cc',
      'utils/utils.cc',
    ],
  )
  pkg_extra_cflags += ['-isystem${includedir}/nythera-lib/detail']
else
  pkg_extra_libraries += [mcu_dep]
endif

pkg = import('pkgconfig')
pkg.generate(nythera_lib,
  filebase: 'nythera-lib',
  name: 'NytheraLib',
  description: 'Shared infrastructure for Nythera Shell and Files',
  subdirs: 'nythera-lib',
  requires: [wayland_client_dep, egl_dep, gles2_dep, cairo_dep, pango_dep,
             xkbcommon_dep, gio_dep, curl_dep, nlohmann_dep, tomlplusplus_dep],
  extra_cflags: pkg_extra_cflags,
  libraries: pkg_extra_libraries,
)

install_headers(
  'src/app/main_loop.h',
  'src/app/poll_source.h',
  'src/app/single_instance_lock.h',
  subdir: 'nythera-lib/app',
)

install_headers(
  'src/calendar/calendar_types.h',
  subdir: 'nythera-lib/calendar',
)

install_headers(
  'src/cli/completions.h',
  'src/cli/help.h',
  'src/cli/parse.h',
  'src/cli/schema.h',
  'src/cli/schema_config.h',
  'src/cli/schema_dmenu.h',
  'src/cli/schema_firefox_theme.h',
  'src/cli/schema_msg.h',
  'src/cli/schema_plugins.h',
  'src/cli/schema_root.h',
  'src/cli/schema_theme.h',
  subdir: 'nythera-lib/cli',
)

install_headers(
  'src/config/atomic_file.h',
  'src/config/color_spec.h',
  'src/config/config_credentials.h',
  'src/config/config_export.h',
  'src/config/config_limits.h',
  'src/config/config_merge.h',
  'src/config/config_migrations.h',
  'src/config/config_notification_sink.h',
  'src/config/config_origins.h',
  'src/config/config_service.h',
  'src/config/config_types.h',
  'src/config/state_store.h',
  'src/config/widget_config.h',
  'src/config/widget_setting_value.h',
  subdir: 'nythera-lib/config',
)

install_headers(
  'src/config/schema/config_schema.h',
  'src/config/schema/config_sections.h',
  'src/config/schema/diagnostics.h',
  'src/config/schema/engine.h',
  'src/config/schema/field.h',
  'src/config/schema/ranges.h',
  subdir: 'nythera-lib/config/schema',
)

install_headers(
  'src/core/deferred_call.h',
  'src/core/frame_rate_limiter.h',
  'src/core/log.h',
  'src/core/random.h',
  'src/core/scoped_timer.h',
  'src/core/text_clipboard.h',
  'src/core/timer_manager.h',
  'src/core/toml.h',
  'src/core/ui_phase.h',
  subdir: 'nythera-lib/core',
)

install_headers(
  'src/core/files/directory_scanner.h',
  'src/core/files/display_text.h',
  'src/core/files/file_watcher.h',
  'src/core/files/resource_paths.h',
  subdir: 'nythera-lib/core/files',
)

install_headers(
  'src/core/inotify/inotify.h',
  subdir: 'nythera-lib/core/inotify',
)

install_headers(
  'src/core/input/key_chord.h',
  'src/core/input/key_modifiers.h',
  'src/core/input/key_symbols.h',
  'src/core/input/keybind_matcher.h',
  'src/core/input/shortcut_keysym.h',
  subdir: 'nythera-lib/core/input',
)

install_headers(
  'src/core/process/process.h',
  'src/core/process/process_fds.h',
  subdir: 'nythera-lib/core/process',
)

install_headers(
  'src/filemanager/directory_metadata.h',
  'src/filemanager/file_columns.h',
  subdir: 'nythera-lib/filemanager',
)

install_headers(
  'src/i18n/i18n.h',
  'src/i18n/i18n_service.h',
  'src/i18n/language_tag.h',
  subdir: 'nythera-lib/i18n',
)

install_headers(
  'src/ipc/ipc_invocation_context.h',
  subdir: 'nythera-lib/ipc',
)

install_headers(
  'src/launcher/launcher_provider.h',
  subdir: 'nythera-lib/launcher',
)

install_headers(
  'src/net/http_client.h',
  'src/net/http_client_poll_source.h',
  'src/net/uri.h',
  'src/net/url_open.h',
  subdir: 'nythera-lib/net',
)

install_headers(
  'src/notification/notification.h',
  'src/notification/notification_filter.h',
  'src/notification/notification_sink.h',
  'src/notification/notifications.h',
  subdir: 'nythera-lib/notification',
)

install_headers(
  'src/render/animation/animation.h',
  'src/render/animation/animation_manager.h',
  'src/render/animation/motion_service.h',
  'src/render/animation/surface_haze.h',
  subdir: 'nythera-lib/render/animation',
)

install_headers(
  'src/render/backend/gles_framebuffer.h',
  'src/render/backend/gles_render_backend.h',
  'src/render/backend/gles_texture_manager.h',
  'src/render/backend/render_backend.h',
  subdir: 'nythera-lib/render/backend',
)

install_headers(
  'src/render/core/async_texture_cache.h',
  'src/render/core/blur_cache.h',
  'src/render/core/cached_layer.h',
  'src/render/core/color.h',
  'src/render/core/heif_library.h',
  'src/render/core/image_decoder.h',
  'src/render/core/image_encoder.h',
  'src/render/core/image_file_loader.h',
  'src/render/core/image_orientation.h',
  'src/render/core/image_resize.h',
  'src/render/core/image_source_log.h',
  'src/render/core/lockscreen_transition_types.h',
  'src/render/core/mat3.h',
  'src/render/core/render_styles.h',
  'src/render/core/renderer.h',
  'src/render/core/screen_frame_geometry.h',
  'src/render/core/shader_program.h',
  'src/render/core/shared_texture_cache.h',
  'src/render/core/snapshot_backdrop.h',
  'src/render/core/surface_haze.h',
  'src/render/core/texture_handle.h',
  'src/render/core/texture_manager.h',
  'src/render/core/thumbnail_service.h',
  'src/render/core/wallpaper_types.h',
  subdir: 'nythera-lib/render/core',
)

install_headers(
  'src/render/gl_shared_context.h',
  'src/render/render_context.h',
  'src/render/render_target.h',
  'src/render/wallpaper_renderer.h',
  subdir: 'nythera-lib/render',
)

install_headers(
  'src/render/programs/audio_spectrum_program.h',
  'src/render/programs/blur_program.h',
  'src/render/programs/countdown_ring_program.h',
  'src/render/programs/effect_program.h',
  'src/render/programs/fancy_audio_visualizer_program.h',
  'src/render/programs/glyph_program.h',
  'src/render/programs/graph_program.h',
  'src/render/programs/image_program.h',
  'src/render/programs/linear_gradient_program.h',
  'src/render/programs/lockscreen_transition_program.h',
  'src/render/programs/nebula_glsl.h',
  'src/render/programs/rect_program.h',
  'src/render/programs/screen_corner_program.h',
  'src/render/programs/screen_frame_program.h',
  'src/render/programs/spinner_program.h',
  'src/render/programs/wallpaper_mask_program.h',
  'src/render/programs/wallpaper_program.h',
  'src/render/programs/wallpaper_sampling_glsl.h',
  subdir: 'nythera-lib/render/programs',
)

install_headers(
  'src/render/scene/audio_spectrum_node.h',
  'src/render/scene/countdown_ring_node.h',
  'src/render/scene/effect_node.h',
  'src/render/scene/fancy_audio_visualizer_node.h',
  'src/render/scene/glyph_node.h',
  'src/render/scene/graph_node.h',
  'src/render/scene/image_node.h',
  'src/render/scene/input_area.h',
  'src/render/scene/input_dispatcher.h',
  'src/render/scene/lockscreen_transition_node.h',
  'src/render/scene/node.h',
  'src/render/scene/rect_node.h',
  'src/render/scene/screen_corner_node.h',
  'src/render/scene/screen_frame_node.h',
  'src/render/scene/spinner_node.h',
  'src/render/scene/text_node.h',
  'src/render/scene/wallpaper_node.h',
  subdir: 'nythera-lib/render/scene',
)

install_headers(
  'src/render/text/cairo_glyph_renderer.h',
  'src/render/text/cairo_text_renderer.h',
  'src/render/text/font_registry.h',
  'src/render/text/font_weight_catalog.h',
  'src/render/text/glyph_registry.h',
  subdir: 'nythera-lib/render/text',
)

install_headers(
  'src/scripting/plugin_id.h',
  subdir: 'nythera-lib/scripting',
)

install_headers(
  'src/shell/surface/shadow.h',
  subdir: 'nythera-lib/shell/surface',
)

install_headers(
  'src/system/app_identity.h',
  'src/system/day_night_schedule.h',
  'src/system/desktop_entry.h',
  'src/system/freedesktop_key_file.h',
  'src/system/icon_resolver.h',
  'src/system/internal_app_metadata.h',
  'src/system/sysmon_threshold_profile.h',
  'src/system/window_state.h',
  subdir: 'nythera-lib/system',
)

install_headers(
  'src/theme/builtin_palettes.h',
  'src/theme/color.h',
  'src/theme/community_palettes.h',
  'src/theme/contrast.h',
  'src/theme/custom_palettes.h',
  'src/theme/fixed_palette.h',
  'src/theme/fixed_palette_json.h',
  'src/theme/image_loader.h',
  'src/theme/palette.h',
  'src/theme/palette_generator.h',
  'src/theme/palette_transform.h',
  'src/theme/scheme.h',
  'src/theme/theme_service.h',
  subdir: 'nythera-lib/theme',
)

install_headers(
  'src/time/time_format.h',
  subdir: 'nythera-lib/time',
)

install_headers(
  'src/ui/app_icon_colorization.h',
  'src/ui/builders.h',
  'src/ui/drag_drop_controller.h',
  'src/ui/palette.h',
  'src/ui/palette_swatch_preview.h',
  'src/ui/popup_chrome.h',
  'src/ui/popup_parent.h',
  'src/ui/scroll_into_view.h',
  'src/ui/signal.h',
  'src/ui/split_pane_focus.h',
  'src/ui/style.h',
  'src/ui/text_input_client.h',
  subdir: 'nythera-lib/ui',
)

install_headers(
  'src/ui/controls/box.h',
  'src/ui/controls/button.h',
  'src/ui/controls/calendar_view.h',
  'src/ui/controls/checkbox.h',
  'src/ui/controls/collapsible.h',
  'src/ui/controls/color_picker.h',
  'src/ui/controls/color_swatch_preview.h',
  'src/ui/controls/context_menu.h',
  'src/ui/controls/context_menu_geometry.h',
  'src/ui/controls/context_menu_popup.h',
  'src/ui/controls/countdown_ring.h',
  'src/ui/controls/drag_source.h',
  'src/ui/controls/drop_zone.h',
  'src/ui/controls/flex.h',
  'src/ui/controls/glyph.h',
  'src/ui/controls/glyph_picker.h',
  'src/ui/controls/graph.h',
  'src/ui/controls/grid_tile.h',
  'src/ui/controls/grid_view.h',
  'src/ui/controls/image.h',
  'src/ui/controls/input.h',
  'src/ui/controls/keybind_recorder.h',
  'src/ui/controls/label.h',
  'src/ui/controls/list_editor.h',
  'src/ui/controls/lockscreen_transition_cover.h',
  'src/ui/controls/markdown_view.h',
  'src/ui/controls/progress_bar.h',
  'src/ui/controls/radio_button.h',
  'src/ui/controls/range_slider.h',
  'src/ui/controls/roving_list_nav.h',
  'src/ui/controls/screen_corner.h',
  'src/ui/controls/scroll_view.h',
  'src/ui/controls/scrollbar.h',
  'src/ui/controls/search_picker.h',
  'src/ui/controls/segmented.h',
  'src/ui/controls/select.h',
  'src/ui/controls/select_dropdown_popup.h',
  'src/ui/controls/select_popup_context.h',
  'src/ui/controls/separator.h',
  'src/ui/controls/slider.h',
  'src/ui/controls/spacer.h',
  'src/ui/controls/spinner.h',
  'src/ui/controls/stepper.h',
  'src/ui/controls/surface_background.h',
  'src/ui/controls/toggle.h',
  'src/ui/controls/virtual_grid_view.h',
  'src/ui/controls/virtual_list_view.h',
  subdir: 'nythera-lib/ui/controls',
)

install_headers(
  'src/ui/dialogs/color_picker_dialog.h',
  'src/ui/dialogs/color_picker_dialog_popup.h',
  'src/ui/dialogs/dialog_background_artwork.h',
  'src/ui/dialogs/dialog_popup_host.h',
  'src/ui/dialogs/file_artwork.h',
  'src/ui/dialogs/file_dialog.h',
  'src/ui/dialogs/file_dialog_popup.h',
  'src/ui/dialogs/file_dialog_view.h',
  'src/ui/dialogs/file_entry_appearance.h',
  'src/ui/dialogs/file_entry_row.h',
  'src/ui/dialogs/file_entry_tile.h',
  'src/ui/dialogs/file_icon.h',
  'src/ui/dialogs/file_view_metrics.h',
  'src/ui/dialogs/glyph_picker_dialog.h',
  'src/ui/dialogs/glyph_picker_dialog_popup.h',
  'src/ui/dialogs/layer_popup_host.h',
  'src/ui/dialogs/search_picker_popup.h',
  subdir: 'nythera-lib/ui/dialogs',
)

install_headers(
  'src/ui/tooltip/tooltip_content.h',
  'src/ui/tooltip/tooltip_manager.h',
  subdir: 'nythera-lib/ui/tooltip',
)

install_headers(
  'src/ui/visuals/audio_visualizer.h',
  'src/ui/visuals/fancy_audio_visualizer.h',
  subdir: 'nythera-lib/ui/visuals',
)

install_headers(
  'src/util/base64.h',
  'src/util/checksum.h',
  'src/util/clamp.h',
  'src/util/file_utils.h',
  'src/util/fuzzy_match.h',
  'src/util/string_utils.h',
  'src/util/sys_utils.h',
  subdir: 'nythera-lib/util',
)

install_headers(
  'src/wayland/ext_foreign_toplevels.h',
  'src/wayland/layer_surface.h',
  'src/wayland/native_data_transfer.h',
  'src/wayland/output_probe.h',
  'src/wayland/output_scale.h',
  'src/wayland/popup_surface.h',
  'src/wayland/subsurface.h',
  'src/wayland/surface.h',
  'src/wayland/text_input_service.h',
  'src/wayland/toplevel_surface.h',
  'src/wayland/wayland_connection.h',
  'src/wayland/wayland_seat.h',
  'src/wayland/wayland_toplevels.h',
  subdir: 'nythera-lib/wayland',
)

install_headers(
  'src/wayland/hyprland/focus_grab_service.h',
  'src/wayland/hyprland/popup_grab_host.h',
  subdir: 'nythera-lib/wayland/hyprland',
)

install_data('LICENSE',
  install_dir: get_option('datadir') / 'licenses' / 'nythera-lib',
)
if not get_option('system_libraries')
  install_data('third_party/fzy/LICENSE', 'third_party/material_color_utilities/LICENSE',
    'third_party/wuffs/LICENSE-APACHE', 'third_party/wuffs/LICENSE-MIT',
    install_dir: get_option('datadir') / 'licenses' / 'nythera-lib',
    rename: ['fzy.txt', 'material-color-utilities.txt', 'wuffs-apache.txt', 'wuffs-mit.txt'],
  )
endif
