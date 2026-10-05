# mxl-browser-source — Implementation Plan

Records how `SPEC.md` is implemented: pins, spike results, layout, deviations, progress.

## 1. Pins

| Component | Pin |
| --- | --- |
| CEF | `144.0.21+g4f5b28c+chromium-144.0.7559.248`, Linux x64 minimal (412 022 647 bytes, SHA-1 `e6233049cec12ba108b4b920b499e2a082ffd0ae`) |
| MXL | `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`, fabrics off |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` |

## 2. Spike results (lab host, NVIDIA A16, driver 595.84, 2026-10-05)

A minimal windowless CEF program (`spike/` in the session scratchpad; to be added as `tests/tools/cef_spike.cpp`) drove a 1080p page with one external BeginFrame every 20 ms in an Ubuntu 24.04 container, without an X server (`--ozone-platform=headless`). 20 s measured after 5 s warm-up.

| Mode | Renderer reported by WebGL | Paints per BeginFrame | BeginFrame → OnPaint p50 / p95 | CPU (all CEF processes) |
| --- | --- | --- | --- | --- |
| software (`disable-gpu, disable-gpu-compositing, use-gl=disabled`) | no WebGL | 1 (animated page) | 1.2 / 1.4 ms | 0.19 cores |
| software + `Invalidate` before each BeginFrame | no WebGL | 2 (an immediate copy + the new frame) | 0.05 / 1.7 ms | 0.21 cores |
| static page, no `Invalidate` | – | 0 (no damage, no paint) | – | 0.02 cores |
| static page, `Invalidate` | – | 1 (immediate) | 0.02 ms | 0.03 cores |
| SwiftShader (`use-angle=swiftshader`) | SwiftShader | ~1 | 19 / 20 ms | 6.9 cores |
| ANGLE `gl-egl`, NVIDIA vendor file missing | **llvmpipe** | 1 | 7.9 / 8.8 ms | 3.1 cores |
| **ANGLE `gl-egl` with `10_nvidia.json`** | **NVIDIA A16, OpenGL ES 3.2** | **1** | **5.3 / 15.7 ms** | **0.34 cores** |
| ANGLE Vulkan with `nvidia_icd.json` | NVIDIA A16 (Vulkan) | 0/2 at 29 of 1000 | 9.7 / 19 ms | 0.59 cores |

Conclusions:
- **S1 GPU backend:** `--use-gl=angle --use-angle=gl-egl` with headless Ozone. The NVIDIA container toolkit injects `libEGL_nvidia.so.0` but not the glvnd vendor file; the image ships `/usr/share/glvnd/egl_vendor.d/10_nvidia.json` (`{"file_format_version":"1.0.0","ICD":{"library_path":"libEGL_nvidia.so.0"}}`). Without it EGL silently falls back to llvmpipe. Chromium's own Vulkan init fails (`vkCreateInstance -7`) and is harmless. The software fallback stays plain software compositing (SwiftShader costs 6.9 cores).
- **S2 BeginFrame:** one BeginFrame gives exactly one `OnPaint` when the page changed and none when it did not. `Invalidate(PET_VIEW)` produces an immediate paint of the current frame. Deviation from SPEC §2.4: no `Invalidate` per frame and no "one outstanding BeginFrame" rule (a static page never answers, which would stall it). Each tick commits the newest paint or repeats the last frame and sends one BeginFrame; `Invalidate` only after load, resize and source changes. A paint that arrives more than one period after its BeginFrame counts as late.
- **S3 no X server:** works with headless Ozone; no GTK, no Xvfb.
- **Alpha:** BGRA is premultiplied in every mode (rgba(255,0,0,0.5) arrives as R=128, A=128).
- **rAF:** `requestAnimationFrame` runs once per BeginFrame (1225 callbacks for ~1250 BeginFrames): animations are frame-locked.
- **S4 audio:** 48 kHz stereo in 480-frame packets; `pts` (ms) follows `CLOCK_MONOTONIC` exactly (20 010 ms over 20.010 s). Drift against TAI is the drift of the monotonic clock; the resampler controller stays as a safety net.
- **Build behind Zscaler:** the proxy cuts long downloads after 20–72 MiB. `docker/fetch.sh` resumes with `curl -C -` until the size matches, then checks the SHA-1 (33 attempts / 104 s for the 412 MB archive on the host).

## 3. Progress (2026-10-05)

Done: spikes; `docs/api.md` (wire format); `src/config` (settings table, document); `src/convert` (scalar pass 1, shared pass 2, preview); copied from mxl-decklink (MIT): `src/util/logging`, `src/util/uuid`, `src/mxlio/{domain,flowdef,videowriter}`, `cmake/EmbedFile.cmake`, `third_party/{doctest,picojson}`; web UI being written (`web/`).

Next, in order:
1. `src/convert/avx2.cpp` (pass 1 AVX2, unpremultiply via the table with a gather) and unit tests scalar == AVX2.
2. `src/mxlio/audiowriter` (planar float), domain creation with the tmpfs check (exit 78), RLIMIT_NOFILE.
3. `src/audio` (FIFO, PI-controlled libsamplerate resampler), `src/engine` (frame store with the OnPaint copy and converter thread; TAI tick: commit, audio, BeginFrame; metrics).
4. `src/cef` (app with switches per render mode, client handlers, browser facade, templates.local scheme, URL policy, dialogs/popups/downloads/permissions), `helper/main.cpp`.
5. `src/nmos` (ids, nmos-cpp node with video/key/audio senders, sr-ctrl, listener check), `src/ops` (HTTP/WebSocket server, REST API, interaction, events, metrics, DevTools proxy), `src/app`, `src/main.cpp`.
6. CMakeLists, `docker/Dockerfile` (fetch.sh, mallinfo shim, 10_nvidia.json, fonts), CI, Compose, k8s examples, Grafana dashboard, docs, tests (unit, integration with test pages), lab runs, release 1.0.0.
