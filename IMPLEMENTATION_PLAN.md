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

### First lab runs (2026-10-05, A16 lab host, 2× Xeon Gold 6136, 1080p50, v210a, lab registry)

The image builds (2.2 GB uncompressed) and starts; `/readyz` turns 200 when the lab registry's Query API lists the node. The grain reader (`tests/tools/grain_reader`, in the image as `mxl-bs-grain-reader`) decodes `counter.html` from every grain and finds `avsync.html`'s flashes and bursts.

| Step | Result |
| --- | --- |
| first run, software | 1002 grains in 20 s, 0 missed, but 121 repeats each followed by a skip of 2: the conversion took about 12 ms of CPU per frame on one thread and paint + conversion missed the 20 ms tick |
| pass 2 rewritten (chroma filtered once per row, tables for alpha and key, AVX2 alpha store), conversion in 4 bands on a worker pool | 997 of 1002 consecutive, 2 repeats; 2.2–3.3 ms per frame |
| AVX2 pass 1 skips the unpremultiply gather when 8 pixels are all opaque or all transparent, AVX2 chroma filter | 999 of 1002; 0.99 cores (was 1.23); conversion 2.2 ms |
| GPU mode | WebGL reports `ANGLE (NVIDIA Corporation, NVIDIA A16/PCIe/SSE2, OpenGL ES 3.2)`; 995 of 1002 consecutive, 3 repeats; 0.98 cores |
| `tone.html` | −20.01 dBFS on both channels; FIFO 61 ms, drift 13 ppm, no underruns at steady state |
| `avsync.html`, 3 grains video delay | first 87 ms: the FIFO held 121 ms after the stream start and the controller needed minutes at 500 ppm. Priming now ends at the target exactly (the excess is dropped): 16 ms after the start, 21.6 ms (17–25) at steady state |
| `avsync.html`, auto delay (GPU 4, software 5 grains) | GPU +1.1 ms (−4.0…+6.5), software +1.0 ms (−2.9…+5.2), 20 of 20 bursts each |
| `bars.html`, `transparency.html` (both modes) | all eight bars exactly the BT.709 10-bit values (e.g. yellow 877/64/553); alpha 0/25/50/75/100 % → 0/257/514/766/1023, fill straight (white stays 940 at any alpha) |
| `interact.html` via `/api/v1/interact` (both modes) | click, keys, text `ü你`, wheel reported by the page within 3.6 s; an observer's input is refused (`not_controller`). The wheel scrolled the wrong way (CEF's sign is the opposite of the DOM's); fixed |
| `tests/integration/ops_test.py` (GPU mode) | template `update`/`play`/`invoke`/`stop` (the page reports `play`); renderer `kill -9`: `crashed` after 0.1 s, loaded 1.7 s later, grains continue, 0 missed; `hang.html`: detected 3.8 s after the loop starts, renderer killed, `blank.html` loads 0.1 s after the navigation; a page that always hangs ends in `error` after 5 terminations (41 s); DevTools through the tunnel (`Runtime.evaluate`, bundled frontend). Fixed on the way: the hang recovery closed and recreated the browser without the crash backoff (a page that always hangs looped every 7 s), a crash reload pending from before a navigation reloaded the new page, the DevTools JSON answered 502 (Chromium does not answer HTTP/1.0) and pointed at the appspot frontend |
| SIGTERM (`MXL_CLEANUP_ON_EXIT=true`) | exit 143 after 1.0 s; node and senders 404 in the Query API; own domain removed. Before the fix: exit 0, nothing deregistered (CefInitialize replaces the SIGTERM handler with Chromium's own) |
| `tests/integration/pages_test.py` (GPU mode, clicks through `/api/v1/interact`) | `dialogs.html`: alert dismissed, confirm and prompt cancelled (the page gets `false` and `null`), file chooser cancelled, `beforeunload` accepted and the navigation goes on; `popup.html`: `window.open` and `target=_blank` blocked (2 counted), the page stays; `download.html`: denied and counted; `permissions.html`: camera/microphone, geolocation, notifications, clipboard read and MIDI all end as not granted. During every page 49.8–50.3 grains/s, 0 missed. Only camera and microphone are counted, and only with a capture device (`use-fake-device-for-media-stream`): the other four are `denied` in CEF's profile before any request (`navigator.permissions.query`), so Chromium refuses them without asking the handler |
| Startup refusals (`~/mxl-lab/bin/mbs-refusals`, next to a running instance) | MXL root not a tmpfs: exit 78 after 2.3 s with the fix in the message; busy `WEB_PORT`: 75 after 1.6 s; busy `NMOS_PORT`: 75 after 1.4 s; no domain left behind. Before the fix the ports were bound after the own domain and CEF: a busy web port left the domain behind, and a busy NMOS port took 15.8 s (shutting down nmos-cpp's half-started server hit the 10 s timeout). Both ports are now checked with a test bind before anything is created |
| IME (`interact_test.py`, GPU mode) | composition `にほ` shows in the field, commit `日本` replaces it, a composition `テ` that is cancelled disappears: the page reports `abü你にほ`, `abü你日本`, `abü你日本テ`, `abü你日本` |
| Click → picture (`tests/integration/latency_test.py`, 10 clicks, `grain_reader change` finds the first grain showing `interact.html`'s marker; latency = T(grain) − TAI of the click, on the lab host) | GPU mode (video delay 4 grains): median 126 ms, max 138 ms; software mode (5 grains): median 126 ms, max 146 ms; budget 150 ms. Every `ack` grain is no later than the first grain that shows the click. The video delay for A/V sync is most of it (80–100 ms). The interaction socket's preview JPEGs delayed the acks in the test; it asks for 1 fps at 160 px |
| `tests/integration/nmos_test.py` (IS-05 v1.2) | video and audio sender disabled and enabled: active document, `/api/v1/nmos` and `sender_enabled` agree; video flow read with `mxl-bs-grain-reader`: no new grain in 2 s while disabled, 102 in 2 s after enabling. Audio disabled, `docker restart`: still disabled after the restart, video enabled. Fixed on the way: `sender_enabled` was missing until the first IS-05 change (the document holds only changed states); every sender of the configuration is now reported |
| Renderer probe | One GPU start on the lab reported `render_degraded` (renderer `none`: no WebGL context) and kept it until the restart; the next start had ANGLE on the A16 again. A software answer is now asked again twice, 5 s apart. Checked with `BROWSER_CHROMIUM_FLAGS_APPEND=disable-webgl`: two `render_probe_retry`, then `render_degraded` 11 s after the load; a normal start still reports `gpu` on the first probe |
| Image size | 2.22 GB → 907 MB: `libcef.so` of the minimal CEF distribution carries its debug symbols (1.5 GB); `strip --strip-unneeded` in the CEF stage leaves 233 MB. The fonts SPEC §15 names stay (the apt layer is 442 MB). Afterwards in GPU mode on the lab: interaction and IME, §4.5 pages, template/crash/hang/DevTools, click → picture (max 148 ms), IS-05 tests all pass |
| AMWA (`tests/nmos/amwa.sh` in CI, first run) | IS-05-02 without failures. IS-05-01 test_11_01 and BCP-007-03-01 test_17: a Sender's active `mxl_flow_id` stayed `auto`; fixed (resolved to the sender's flow, checked on the lab: staged `auto`, active the real ids). IS-04-01: the node never registered with the tool's DNS-SD-advertised mock registry (nmos-cpp's DNS-SD calls through Avahi failed with -65537); open, the script now waits for avahi and logs diagnostics |
| Pod-like run (read-only root, all capabilities dropped, uid 1000, only `/Volumes/mxl/<instance>` mounted from the host tmpfs) | ready, grains without misses, exit 143 on SIGTERM, domain emptied. Before: refused with 78, because the tmpfs check looked at the MXL root only; it now accepts a tmpfs at the domain directory (SPEC §13) |
| CPU and memory metrics | `process_cpu_seconds_total`, `process_resident_memory_bytes`, `mxl_browser_source_cef_processes_cpu_seconds_total` and `_resident_bytes` (SPEC §12 named them, they were missing). Lab, static page: app 0.18 cores and 411 MB, CEF processes 721 MB |
| Compose demo (`docker/docker-compose.demo.yaml`, lab, without Prometheus because the lab runs one) | registry, browser source, monitor and MediaMTX up; the route service plays the lower third and routes the monitor: channel 1 video `running`, both nodes registered, Grafana loads the dashboard (12 panels). Fixed on the way: `rhastie/nmos-cpp:latest-amd64` no longer exists (now the platform's registry image), loopback `NMOS_HOST_ADDRESS` is refused, the monitor (uid 1000) could not write a root-owned volume |
| 1 h soak, GPU mode, `counter.html` (`~/mxl-lab/soak/mbs-soak.sh`) | 180 001 grains, 0 missed, 179 978 advances, 11 repeats each followed by a skip of 2 (a paint just after its tick: the tick repeats, the next one takes the newer paint), 0 crashes or hangs, 0.71 cores. SPEC §17 asks for 0 repeated; open |

All conversion changes produce the same bytes as before (unit tests against the previous packers at 19 widths, AVX2 against scalar on runs of opaque and transparent pixels, bands against one pass).

More decisions:
- **Late paints.** A paint carries no frame id and an unchanged page answers no BeginFrame, so pairing a paint with "its" BeginFrame drifts off by a period after one unanswered BeginFrame (the first run counted nearly every paint as late). `late_paints_total` now counts paints that were never committed because a newer one arrived before the tick; `paint_latency_seconds` is measured from the newest BeginFrame.
- **Auto video delay.** Measured with `avsync.html`: Chromium's own audio path adds about 60 ms, and GPU mode adds one period to the video (the compositor's extra frame). `BROWSER_VIDEO_DELAY_GRAINS=auto` = round((audio buffer + 60 ms − (lead + 1 in GPU mode)·P) / P): 4 grains at 1080p50 in GPU mode, 5 in software mode.
- **Hang recovery kills the renderer.** Chromium reports a renderer unresponsive only after input; for a hang without input the supervisor sends SIGKILL to the renderer processes of this browser process (`--type=renderer`, this process among the first four ancestors), so the crash path (backoff, 5 in 10 minutes) applies. Closing and recreating the browser stays as the fallback when none is found.
- **Signal handlers after CefInitialize.** Chromium installs its own SIGTERM/SIGINT handlers in `CefInitialize` (they exit 0 without our shutdown sequence); ours are installed again after it.
- **Logs.** Chromium logs to stderr and to `log_file`; `log_file` is `/dev/null`, otherwise every line appears twice. Chromium's D-Bus errors at start (no bus in the container) are harmless.

Next, in order:
1. AMWA IS-04-01: registration through DNS-SD in CI.
2. Integration tests in CI, the G1–G14 table, 1 h soak (repeat/skip pairs), release 1.0.0.
