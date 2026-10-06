# Changelog

## 1.0.0 - 2026-10-06

First release: SPECIFICATION is `SPEC.md`, lab results and decisions are in `IMPLEMENTATION_PLAN.md`.

- A web page rendered offscreen with CEF 144 (GPU through ANGLE on NVIDIA, or software), one external BeginFrame per MXL grain index: video as v210, optionally with a key (`v210a` or a separate key flow), and float32 audio at 48 kHz, A/V-aligned (automatic video delay).
- NMOS node (IS-04, IS-05 v1.2, BCP-007-03) with nmos-cpp: senders report their real domain and flow in `/active`, `master_enable` stops a flow and survives a restart; static registry by default, DNS-SD (`local.`) on request.
- Web UI and API: preview, interaction with the page (mouse, keys, text, IME, wheel), CasparCG-style templates, DevTools through `/devtools/` (off by default), config export and import.
- Robustness: renderer crash and page hang recovery with backoff, dialogs, popups, downloads and permission requests never block, busy ports refused before anything is created, the own MXL domain on a tmpfs (root or domain directory).
- Metrics with the `mxl_browser_source_` prefix, process and CEF CPU/memory, Grafana dashboard; Compose demo, host and GPU files; Kubernetes examples; single-node RKE2 notes.
- Image about 900 MB, uid 1000; CI runs unit tests, the integration tests of SPEC §17 in software mode (including the start → SIGTERM lifecycle) and the AMWA suites.

Known limitation: the output is not yet free of single repeated frames. SPEC acceptance criterion 2 asks for 0 repeated grains in 1 h at 1080p50; 1 h soaks on the lab host (A16, GPU mode, `counter.html`) had 0 missed grains, but 11 repeat/skip pairs in 180 001 grains on a quiet host and 273 repeats with 35 skips in 180 002 grains while other lab work ran (0.15 %). Each is a paint that reaches the tick a little late. Released with the owner's agreement; the fix follows in 1.0.x.
