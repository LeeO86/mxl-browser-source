# mxl-browser-source

Renders a web page offscreen with the Chromium Embedded Framework (CEF) and writes it as MXL flows: video (v210, optionally with a key as v210a or a separate key flow) and audio (float32, 48 kHz). One BeginFrame per MXL grain index keeps the page frame-locked to house time (TAI). Senders register with NMOS (IS-04/IS-05, BCP-007-03) and are routed like every other media function of the MXL PoC platform. An operator sees a preview and can interact with the page from the web UI, like the "Interact" window of the OBS Browser Source; HTML graphics templates are driven CasparCG-style (`play`, `stop`, `next`, `update`), and EBU OGraf v1 graphics play on up to 8 layers with a graphics library, a controller tab, an `ograf` widget and the OGraf Server API.

**Status: 1.2.0.** [`SPEC.md`](SPEC.md) is the contract, [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) records pins, spike results and deviations, [`docs/api.md`](docs/api.md) is the wire format of the HTTP and WebSocket API.

## Run it

The own MXL domain must be on a tmpfs: the MXL root, or a tmpfs mounted at the domain directory (anything else exits 78). On one host with Docker:

```sh
docker run -d --name browser --network host --shm-size 1g --ulimit rtprio=10 -u 1000:1000 \
  -v /Volumes/mxl:/Volumes/mxl -v $PWD/config:/config \
  -e NMOS_REGISTRY_ADDRESS=10.0.0.10 -e NMOS_REGISTRY_PORT=8010 \
  -e MXL_OUTPUT_DOMAIN_DIR=/Volumes/mxl/browser-1 \
  ghcr.io/leeo86/mxl-browser-source:<version>
```

Open `http://<host>:8160/` for the UI. With an NVIDIA GPU add `--gpus 1` (the image sets `NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics`); `BROWSER_RENDER=auto` uses it when it is visible and reports what Chromium really uses (`render.mode` in `/api/v1/status`, `render_mode` and `render_degraded` in `/metrics`).

| Port | Setting | Default |
| --- | --- | --- |
| Web UI, REST, WebSockets, `/livez`, `/readyz`, `/statusz`, `/metrics`, `/devtools/`, widgets | `WEB_PORT` | 8160 |
| NMOS Node and Connection APIs | `NMOS_PORT` | 3312 (+1 reserved) |

The most used settings (all of them, with defaults and "restart required" markers, are in SPEC §11 and `GET /api/v1/config`):

| Setting | Default | Meaning |
| --- | --- | --- |
| `BROWSER_FORMAT` | `1080p50` | `720p50`, `1080p25`, `1080p29.97`, `1080p50`, `1080p59.94` |
| `BROWSER_KEY_MODE` | `off` | `off`, `v210a` (alpha plane after the fill), `fill_key` (a second v210 flow with the key as luma) |
| `BROWSER_AUDIO_CHANNELS` | `2` | `0` (no audio flow), `2`, `8`, `16` |
| `BROWSER_RENDER` | `auto` | `auto`, `gpu` (ANGLE on the NVIDIA EGL device), `software` |
| `BROWSER_ON_PAGE_ERROR` | `hold` | output while a page loads, failed or crashed: `hold`, `transparent`, `black`, `slate` |
| `BROWSER_LABEL` | `Browser` | sender labels `<label> Video`, `<label> Key`, `<label> Audio` |
| `NMOS_REGISTRY_ADDRESS` / `_PORT` | – / 3210 | static registry; `/readyz` waits until the Query API lists the node |
| `MXL_OUTPUT_DOMAIN_DIR` | `<root>/browser-source-<seed>` | the own domain (created once, never rewritten) |
| `BROWSER_API_TOKEN` | – | optional bearer token for `/api/v1/…` and `/devtools/…` |
| `WIDGET_FRAME_ANCESTORS` | `'self'` | CSP `frame-ancestors` of the `/widget` pages; `/widgets` answers these origins with CORS. `;`, `,` or control characters exit 78 |
| `BROWSER_DEVTOOLS` | `false` | Chrome DevTools through `/devtools/` (never the internal port) |
| `BROWSER_PROFILE` | `ephemeral` | `persistent` keeps cookies and logins in `/config/profile` across restarts |
| `BROWSER_WEBAUTHN` | `false` | `true` offers WebAuthn (passkeys) to pages; see sign-in pages below |
| `BROWSER_WEBGPU` | `false` | `true` offers WebGPU (Dawn) to pages; off, `navigator.gpu.requestAdapter()` answers null without starting Dawn |
| `BROWSER_FRAME_PIPELINE` | `1` | a paint that misses its tick is shown at the next one instead of held (one period more latency); `0` as in 1.2.0 |
| `BROWSER_FRAME_CLOCK` | `external` | `internal`: Chromium paces the page with its own frame clock at the house rate, phase-locked to TAI, instead of one external BeginFrame per grain (smoother `<video>`, SPEC §2.4); 25 and 50 Hz formats only |
| `BROWSER_FRAME_CLOCK_PHASE_MS` | `0` | internal frame clock: its BeginFrames this many ms after each grain start |
| `BROWSER_REALTIME` | `true` | tick and converter threads `SCHED_FIFO` when the container allows it: give it `--ulimit rtprio=10` (Compose `ulimits: {rtprio: 10}`); `--cap-add SYS_NICE` alone does not help a non-root process (SPEC §2.2) |
| `BROWSER_CONVERT_THREADS` | `4` | threads that convert one frame (1–16) |
| `BROWSER_OGRAF_DIR` | `<state>/ograf` | the OGraf graphics library (on the `/config` volume) |
| `BROWSER_OGRAF_UPLOAD_MAX_MB` | `256` | largest graphics ZIP (unpacked at most 4×) |
| `BROWSER_OGRAF_ACTION_TIMEOUT_MS` | `10000` | how long an OGraf action waits for the graphic; then 504 |
| `BROWSER_OGRAF_NETWORK` | `local` | in mode `ograf`: `local` (the library only), `any` (the URL policy), or a comma list of extra hosts |
| `BROWSER_OGRAF_CORS_ORIGINS` | – | origins (space or comma list, never `*`) that may call `/ograf/v1` from a browser |

The page to show, its CSS and JavaScript, zoom, background and presets are the source document (`/api/v1/source`, saved in `/config/config.json`); changes apply at once. Pages in `/config/templates` are served at `https://templates.local/…` without network access; the image ships `blank.html`, `bars.html`, `lower-third.html` and the test pages (`counter.html`, `avsync.html`, `tone.html`, `transparency.html`, `slow.html`, `hang.html`, `interact.html`, `dialogs.html`, `popup.html`, `download.html`, `permissions.html`).

The preview on the Source page is the interaction window: back, forward and reload, and with "Interact" on, mouse (drag to select text, double and triple click), keys, text and IME go to the page.

Operator screens can frame the interaction window as a widget: `GET /widgets` lists it (`interaction`, no parameters, `min_size` 640×400) and `GET /widget/interaction[?theme=dark|light|transparent]` is the page. It talks only to this instance's own origin (REST and both WebSockets on `WEB_PORT`), asks for `BROWSER_API_TOKEN` when one is set, and posts `widget-ready` and `widget-size` to its parent. Only the pages that `WIDGET_FRAME_ANCESTORS` names may frame it; on the platform set it to `'self' https://designer.<fqdn>`.

**Sign-in pages (Zscaler, Microsoft Entra ID).** Windowless CEF cannot show Chromium's WebAuthn dialog (passkeys, security keys, the QR code for a phone), so a page that asks for a passkey would hang. WebAuthn is therefore hidden from pages (`BROWSER_WEBAUTHN=false`), and Entra offers Authenticator push, a code or a password instead. Sign in once in the interaction window; with `BROWSER_PROFILE=persistent` the session survives restarts until the identity provider ends it. A tenant that allows passkeys only cannot sign in here: ask for another method, or for the graphics hosts to bypass the proxy sign-in.

Exit codes: 0 `--help`/`--version`, 75 a port, the state directory, CEF or MXL cannot start, 78 invalid configuration or the own domain not on a tmpfs, 143 SIGTERM/SIGINT (also when the shutdown budget ran out).

### OGraf graphics

The source field `mode` chooses what the browser shows: `page` (the default, `url` as before) or `ograf`, a built-in renderer with 8 layers for [EBU OGraf v1](https://ograf.ebu.io/) graphics (layer 8 on top, one graphic per layer). The output stays one video flow (fill and key) and one audio flow. The design and every rule are in [`docs/ograf.md`](docs/ograf.md).

- **Library:** upload a ZIP with one or more `*.ograf.json` manifests in the Graphics tab (button or drag and drop) or with `POST /api/v1/ograf/graphics` (body: the ZIP). Each manifest is checked against the embedded v1 schemas; a ZIP is accepted or refused as a whole, the same `id` replaces a graphic (a layer keeps the old version until it is cleared). The library lives in `BROWSER_OGRAF_DIR` on the `/config` volume; the config export does not include it.
- **Graphics tab:** the output preview, the 8 layers with their state (ON AIR, ready, stopped, error) and Load, Play, Next, Stop, Clear; a form built from the graphic's schema (the GDD types, nested objects and lists, required fields) for Load and Update; one button per custom action. Leaving mode `ograf` with a graphic loaded asks first.
- **Widget `ograf`:** `GET /widget/ograf[?layers=1,2&theme=dark|light|transparent]` is the tab's preview, layers and form for the listed layers (`layers`: a comma list of 1…8, default all), without the library or the mode switch; `min_size` 640×480. `GET /widgets` lists it next to `interaction`.
- **Server API:** the OGraf Server API v1 at `/ograf/v1` (one renderer `main`, render targets `{"layer": 1…8}`), checked with SuperFlyTV's `ograf-server` controller. A browser controller on another origin needs `BROWSER_OGRAF_CORS_ORIGINS`. Besides the spec's codes it answers 409 when there is no renderer page and 504 when a graphic does not settle in time (`docs/ograf.md` §6). The layer states are also at `GET /api/v1/ograf/layers` and on `/api/v1/events`.
- **Offline:** in mode `ograf` the renderer and the graphics may load only from the library (`BROWSER_OGRAF_NETWORK=local`); everything else is refused and counted (`navigation_blocked_total{reason="ograf_local"}`). Graphics that fetch data from an internal server need its host in the list.
- **Recovery:** after a renderer crash the layers come back at their step without animation; after a restart they come back loaded, not played.
- **Metrics:** `ograf_actions_total{action,result}`, `ograf_action_seconds`, `ograf_layers{state}`, `ograf_graphics`, `ograf_uploads_total`, `ograf_restores_total`. A 2 h soak with 8 layers is in [`docs/performance.md`](docs/performance.md).

The CasparCG template API stays and works in mode `page` only (409 in mode `ograf`).

### Docker Compose

- `docker/docker-compose.demo.yaml`: one machine with the platform's nmos-cpp registry, the browser source on a tmpfs MXL root, mxl-webrtc-monitor with MediaMTX, Prometheus and Grafana. A one-shot service puts the lower third on air and routes the monitor to it over IS-05: `docker compose -f docker/docker-compose.demo.yaml up`, then open the monitor at `http://127.0.0.1:8100/`, the browser source at `:8160` and Grafana at `:3000`.
- `docker/docker-compose.host.yaml`: the browser source alone, host network, `/Volumes/mxl` and a facility registry (`NMOS_REGISTRY_ADDRESS`).
- `docker/docker-compose.gpu.yaml`: overlay for either file with an NVIDIA device reservation and `BROWSER_RENDER=gpu`.

### Kubernetes

`deploy/mxl-browser-source.yaml` (software rendering) and `deploy/mxl-browser-source-gpu.yaml` (one GPU time slice) are examples: ConfigMap, Deployment (pod network, uid 1000, read-only root filesystem, all capabilities dropped, only the own MXL domain mounted from the node's tmpfs), Service, NetworkPolicy and ServiceMonitor. The platform's chart lives in the platform repository. `docs/single-node-rke2.md` sets up a one-machine test cluster.

### Metrics and Grafana

`/metrics` has grains, repeats and misses, paint latency, conversion and commit lateness, audio, page state, crashes, interaction, sender and registration state, and CPU and memory: `process_cpu_seconds_total` and `process_resident_memory_bytes` of the process, `mxl_browser_source_cef_processes_cpu_seconds_total` and `_resident_bytes` of the CEF processes below it. The Grafana dashboard is `deploy/grafana/mxl-browser-source.json` (with an OGraf row: actions, action time, layers, library, uploads, restores and blocked requests).

Image size: about 900 MB uncompressed (CEF's `libcef.so` stripped of its debug symbols; the fonts take about 440 MB with the system libraries).

## Lab results so far (A16 lab host, 2× Xeon Gold 6136, 1080p50, v210a)

| Case | Result |
| --- | --- |
| `counter.html` (canvas redrawn every frame), software, 20 s | 1002 grains, 999 consecutive counters, 1 repeat, 0 missed; 0.99 cores |
| the same, GPU (ANGLE, NVIDIA A16 reported by WebGL) | 1002 grains, 995 consecutive, 3 repeats, 0 missed; 0.98 cores |
| `tone.html` | −20.01 dBFS on both channels, no underruns |
| `avsync.html` (flash and 1 kHz burst in the same frame) | audio 16 ms after video (12–22 ms) |
| conversion BGRA → v210 + v210a, 4 threads | 2.2 ms per frame |
| click → first grain showing it (`tests/integration/latency_test.py`) | GPU: median 126 ms, max 138 ms; software: max 146 ms (budget 150 ms) |

## Build

CMake ≥ 3.24 and Ninja. Without CEF and libmxl the core library and its unit tests build anywhere (`cmake -S . -B build -G Ninja && cmake --build build && build/unit-tests`; needs `libsamplerate0-dev`). The process itself builds in `docker/Dockerfile` (CEF minimal distribution, MXL via vcpkg, nmos-cpp). Behind a TLS-intercepting proxy pass `EXTRA_CA_CERT_B64` and the proxy build args.

## License

MIT, see [`LICENSE`](LICENSE). The image contains CEF (BSD-3-Clause), Chromium and other components with their own licences: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), in the image under `/usr/share/doc/mxl-browser-source/` (CEF's `LICENSE.txt` and Chromium's `CREDITS.html` in `cef/`).
