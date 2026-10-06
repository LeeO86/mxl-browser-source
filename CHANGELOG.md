# Changelog

## 1.0.0 (not released yet)

First release: SPECIFICATION is `SPEC.md`, lab results and decisions are in `IMPLEMENTATION_PLAN.md`.

- A web page rendered offscreen with CEF 144 (GPU through ANGLE on NVIDIA, or software), one external BeginFrame per MXL grain index: video as v210, optionally with a key (`v210a` or a separate key flow), and float32 audio at 48 kHz, A/V-aligned (automatic video delay).
- NMOS node (IS-04, IS-05 v1.2, BCP-007-03) with nmos-cpp: senders report their real domain and flow in `/active`, `master_enable` stops a flow and survives a restart; static registry by default, DNS-SD (`local.`) on request.
- Web UI and API: preview, interaction with the page (mouse, keys, text, IME, wheel), CasparCG-style templates, DevTools through `/devtools/` (off by default), config export and import.
- Robustness: renderer crash and page hang recovery with backoff, dialogs, popups, downloads and permission requests never block, busy ports refused before anything is created, the own MXL domain on a tmpfs (root or domain directory).
- Metrics with the `mxl_browser_source_` prefix, process and CEF CPU/memory, Grafana dashboard; Compose demo, host and GPU files; Kubernetes examples; single-node RKE2 notes.
- Image about 900 MB, uid 1000; CI runs unit tests, the integration tests of SPEC §17 in software mode (including the start → SIGTERM lifecycle) and the AMWA suites.
