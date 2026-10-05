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

Done: spikes; `docs/api.md` (wire format); `src/config` (settings table, document); `src/convert` (scalar pass 1, AVX2 pass 1, shared pass 2, preview); `src/mxlio/{audiowriter,setup}` (planar float writer; tmpfs check, domain files, RLIMIT_NOFILE); `src/audio` (SPSC FIFO, PI-controlled libsamplerate resampler); `src/engine/{tick,framestore}` (tick logic, triple-buffered paints); `CMakeLists.txt` for the core library and unit tests (no CEF, no libmxl; 13 cases pass in Ubuntu 24.04 on the lab); copied from mxl-decklink (MIT): `src/util/logging`, `src/util/uuid`, `src/mxlio/{domain,flowdef,videowriter}`, `cmake/EmbedFile.cmake`, `third_party/{doctest,picojson}`; web UI (`web/`, builds to one 117 kB page).

Decisions made while implementing:
- **Resampler fill estimate.** The FIFO fill moves in packet steps (CEF: 480 frames = 10 ms). Sampled raw at the 20 ms tick it is a sawtooth that beats with the tick (period 10 ms ÷ drift, 50 s at 200 ppm) and kept the controller swinging ±500 ppm. The fill now counts what the page produced since its last packet (the push records its TAI time; capped at one packet), smoothed over 1 s. PI gains Kp 0.04 /s, Ki = Kp²/4 (critically damped, 50 s). Unit test: ±200 ppm over 300 s, no underruns, fill within 5 ms of the target, mean correction within 30 ppm of the drift.
- **Tick.** As spike S2 found: a BeginFrame on every tick (none while crashed), no "outstanding" state; the tick commits the newest converted paint or repeats the last frame (`repeated` for a ready page, `repeatedHold` while loading/crashed/hung) and fills indexes it woke too late for (`missed`). Late paints are counted on the paint side (BeginFrame → OnPaint time).

Done since (2026-10-05, evening): `src/engine/engine` (converter thread, grain buffer sets, video delay ring, page-error substitutes, audio per grain through the resampler, preview on request); `src/util/metrics` (Prometheus text with millisecond buckets); `src/ops/{httpserver,urlpolicy,keymap,interact}` (the siblings' server with per-connection WebSocket sessions and a raw takeover for the DevTools tunnel; SPEC §14.3 policy; DOM code → virtual key and RAWKEYDOWN/CHAR/KEYUP; the interaction protocol); `src/nmos/{ids,node}`; `src/util/net` (announce address, listener check, HTTP GET); `src/cef/runtime` (switches per render mode, client with every handler of §4.5, templates.local scheme, message router for `window.mxlBrowserSource.post`, facade on the UI thread); `src/app/{application,api,mxladapters}` and `src/main.cpp`, `helper/main.cpp`; `docker/Dockerfile` (webui, cef, build, runtime stages; mallinfo shim; `10_nvidia.json`); built-in pages in `templates/`. Unit tests: 39 cases (engine, tick, audio, conversion, frame store, server, URL policy, key map, interaction, ids, listener check).

More decisions:
- **Hang detection.** SPEC §13 said "no paint for the timeout with BeginFrames outstanding"; with spike S2's model a static page paints nothing, so that would flag every static page. The supervisor sends a DevTools `Runtime.evaluate("1")` once a second while the page is loaded; no answer within `BROWSER_HANG_TIMEOUT_MS` is a hang (the renderer's main thread is blocked). Chromium's own `OnRenderProcessUnresponsive` also counts. Recovery: the unresponsive callback's `Terminate()`, else the browser is closed and created again on the current URL.
- **Real render mode.** `SystemInfo.getInfo` is a browser-target method that `ExecuteDevToolsMethod` does not reach. After the first load the page reports WebGL's unmasked renderer string (`Runtime.evaluate`); no WebGL, SwiftShader or llvmpipe means software.
- **`BROWSER_FRAME_LEAD`** is not a scheduling lead: a paint is committed at the first tick after it arrives, whatever the lead. It only enters the `BROWSER_VIDEO_DELAY_GRAINS=auto` estimate: delay = round((audio buffer + 10 ms − lead·P) / P), 3 grains at 1080p50 with the 60 ms default; to be checked with `avsync.html` on the lab.
- **Background.** CEF's `background_color` is fixed when the browser is created; a source change applies the colour as an injected `html,body{background}` style (replaced, never stacked), like the CSS.
- **`user_agent_suffix`** is fixed at `CefInitialize`; a change through the API is saved and applies at the next start (logged as `restart_required`).
- **Preview.** One preview size for all sessions (`BROWSER_PREVIEW_WIDTH`); a session's `width` in the `preview` message is accepted and ignored (one conversion serves everyone). Rates are per session: watchers up to `BROWSER_PREVIEW_FPS`, the controller up to 25.
- **DevTools behind a token.** A link with `?token=` is answered with a redirect that sets an `HttpOnly` cookie for `/devtools`, so the frontend's own requests and its WebSocket authenticate; the tunnel rewrites `Host` and `Origin` to `127.0.0.1:<port>` (Chromium refuses others) and drops the cookie and `Authorization` headers.

Next, in order:
1. First full image build on the lab; fix compile errors in the CEF/nmos-cpp parts; start it with `bars.html` and a registry.
2. Lab runs: frame accuracy (`counter.html` with a grain reader), A/V offset (`avsync.html`), colour and key values, interaction, crash and hang recovery, GPU and software CPU, 1 h soak.
3. CI (`.github/workflows/ci.yaml`, `container.yaml`), Compose files, Kubernetes examples, Grafana dashboard, README and docs, integration tests, release 1.0.0.
