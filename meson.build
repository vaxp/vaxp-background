project('vaxp-background', 'c',
  version : '2.0.0',
  default_options : ['warning_level=2', 'c_std=gnu11'])

cc = meson.get_compiler('c')

# ── Core ─────────────────────────────────────────────────────────────────────
m_dep  = cc.find_library('m',  required : true)
dl_dep = cc.find_library('dl', required : false)

# ── X11 + OpenGL/GLX ─────────────────────────────────────────────────────────
x11_dep    = dependency('x11')
xrandr_dep = dependency('xrandr')
gl_dep     = dependency('gl')

# ── Wayland + EGL ─────────────────────────────────────────────────────────────
wayland_client_dep = dependency('wayland-client')
wayland_egl_dep    = dependency('wayland-egl')
egl_dep            = dependency('egl')

# ── GStreamer (image + video decoding + GL zero-copy) ───────────────────────
gst_dep     = dependency('gstreamer-1.0')
gst_app_dep = dependency('gstreamer-app-1.0')
gst_gl_dep  = dependency('gstreamer-gl-1.0')
gst_vid_dep = dependency('gstreamer-video-1.0')

# ── VA-API (for DMA-BUF surface export) ──────────────────────────────────────
libva_dep   = dependency('libva', required : true)

# ── wayland-scanner — generate C bindings for wlr-layer-shell ─────────────────
wayland_scanner = find_program('wayland-scanner', required : true)

wlr_layer_shell_xml = 'protocol/wlr-layer-shell-unstable-v1.xml'

wlr_layer_shell_header = custom_target(
  'wlr-layer-shell-client-header',
  input  : wlr_layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

wlr_layer_shell_code = custom_target(
  'wlr-layer-shell-code',
  input  : wlr_layer_shell_xml,
  output : 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

xdg_shell_xml = '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'

xdg_shell_header = custom_target(
  'xdg-shell-client-header',
  input  : xdg_shell_xml,
  output : 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

xdg_shell_code = custom_target(
  'xdg-shell-code',
  input  : xdg_shell_xml,
  output : 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# ── Combined dependency list ───────────────────────────────────────────────────
deps = [
  m_dep, dl_dep,
  x11_dep, xrandr_dep, gl_dep,
  wayland_client_dep, wayland_egl_dep, egl_dep,
  gst_dep, gst_app_dep, gst_gl_dep, gst_vid_dep,
  libva_dep,
]

# Include dirs: project include/ + builddir (for generated protocol headers)
inc = include_directories('include', '.')

# ── vaxp-background ───────────────────────────────────────────────────────────
bg_srcs = [
  'src/main.c',
  'src/backend_x11.c',
  'src/backend_wayland.c',
  'src/layer_manager.c',
  'src/wallpaper.c',
  'src/video_wallpaper.c',
  'src/desktop_config.c',
  'vaxp_canvas_opengl.c',
  # Generated Wayland protocol glue
  wlr_layer_shell_code,
  wlr_layer_shell_header,
  xdg_shell_code,
  xdg_shell_header,
]

executable('vaxp-background',
  bg_srcs,
  include_directories : inc,
  dependencies : deps,
  install : true)

# ── vaxp-setbg (GTK wallpaper picker — intentionally kept with GTK) ────────────
setbg_srcs = [
  'src/vaxp-setbg.c',
]

setbg_deps = [
  dependency('gtk+-3.0'),
  dependency('gio-2.0'),
]

executable('vaxp-setbg',
  setbg_srcs,
  include_directories : inc,
  dependencies : setbg_deps,
  install : true)
