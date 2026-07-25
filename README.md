# vaxp-background

A hardware-accelerated desktop background manager designed for X11 and Wayland. It supports static images, looping video wallpapers, and audio-reactive OpenGL shaders.

## Architecture & Features

- **Display Abstraction**: Provides native backends for both Wayland (using `wl_surface`) and X11. EGL is used for OpenGL context creation and buffer swapping.
- **Zero-Copy Video Rendering**: Implements a highly optimized GStreamer pipeline (`vaapidecodebin` -> `glupload`). Video frames are decoded via VA-API and imported directly into OpenGL textures using DMA-BUF and `EGLImage`. This bypasses system RAM entirely, maintaining minimal CPU usage (~2-3% of a single core) even at high framerates.
- **Audio-Reactive Shaders**: A background thread continuously captures the default PulseAudio/PipeWire sink, calculating the RMS volume level. This float value is passed as a uniform to the fragment shader, enabling audio-driven visual effects (such as UV distortion or color-space manipulation).
- **Dynamic Configuration**: Uses `inotify` to monitor the configuration file. Changes to the background file path or audio effect index trigger real-time transitions without requiring a process restart.
- **Layer Management**: Implements a double-buffered layer system to handle smooth crossfade transitions between different images or videos.

## Dependencies

Ensure the following libraries and development headers are installed:
- C11 Compiler, Meson, Ninja
- Wayland (`wayland-client`, `wayland-egl`)
- X11 (`x11`, `xcomposite`, `xext`, `xfixes`)
- EGL & OpenGL
- GStreamer 1.0 (`gstreamer-1.0`, `gstreamer-gl-1.0`, `gstreamer-video-1.0`)
- GStreamer Plugins: Base, Bad, and VA-API (for hardware decoding)
- PulseAudio (`libpulse-simple`, `libpulse`)
- `iniparser`

## Build Instructions

```sh
meson setup build
ninja -C build
```

## Execution

```sh
./build/vaxp-background
```

## Configuration

The program monitors a configuration file (typically via `iniparser` format). 
When the `Wallpaper` path or `AudioEffect` index is modified, the application dynamically reloads the specified asset and stops/starts the audio analysis thread as needed to conserve resources.

---

*This project was developed by a team from the vaxp organization as part of the ecosystem.*