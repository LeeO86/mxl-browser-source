# Changelog

## 1.1.0

- Interaction window: back, forward and reload buttons next to the Interact toggle (`POST /api/v1/source/back` and `/forward`; `can_go_back` and `can_go_forward` in the page state, from CEF's `OnLoadingStateChange`).
- Text can be selected with the mouse. Moves while a button was down did not carry the button flag (`EVENTFLAG_LEFT_MOUSE_BUTTON`), and Chromium ends a drag at the first such move. The server now keeps the buttons that are down and puts them on every down, move and up; a click focuses the page first; a leave or the end of control releases a button still down. Double and triple click select a word and a line.
- WebAuthn is hidden from pages by default (`BROWSER_WEBAUTHN=false`, new): windowless CEF cannot show Chromium's passkey or QR code dialog, so a sign-in page that offered a passkey (Microsoft Entra ID behind Zscaler) hung. Without `PublicKeyCredential` sign-in pages offer push, a code or a password (Entra's sign-in options no longer list the passkey).

## 1.0.1 - 2026-10-06

- A new `domain_def.json` carries `description` and `tags`, as BCP-007-03 requires (`id`, `label`, `description`, `tags`). The browser source wrote only `id` and `label`, and mxl-st2110-gateway 1.0.2 skipped such domains. An existing file is still not rewritten.

## 1.0.0 - 2026-10-06

First release: SPECIFICATION is `SPEC.md`, lab results and decisions are in `IMPLEMENTATION_PLAN.md`.

- A web page rendered offscreen with CEF 144 (GPU through ANGLE on NVIDIA, or software), one external BeginFrame per MXL grain index: video as v210, optionally with a key (`v210a` or a separate key flow), and float32 audio at 48 kHz, A/V-aligned (automatic video delay).
- NMOS node (IS-04, IS-05 v1.2, BCP-007-03) with nmos-cpp: senders report their real domain and flow in `/active`, `master_enable` stops a flow and survives a restart; static registry by default, DNS-SD (`local.`) on request.
- Web UI and API: preview, interaction with the page (mouse, keys, text, IME, wheel), CasparCG-style templates, DevTools through `/devtools/` (off by default), config export and import.
- Robustness: renderer crash and page hang recovery with backoff, dialogs, popups, downloads and permission requests never block, busy ports refused before anything is created, the own MXL domain on a tmpfs (root or domain directory).
- Metrics with the `mxl_browser_source_` prefix, process and CEF CPU/memory, Grafana dashboard; Compose demo, host and GPU files; Kubernetes examples; single-node RKE2 notes.
- Image about 900 MB, uid 1000; CI runs unit tests, the integration tests of SPEC §17 in software mode (including the start → SIGTERM lifecycle) and the AMWA suites.

Known limitation: the output is not yet free of single repeated frames. SPEC acceptance criterion 2 asks for 0 repeated grains in 1 h at 1080p50; 1 h soaks on the lab host (A16, GPU mode, `counter.html`) had 0 missed grains, but 11 repeat/skip pairs in 180 001 grains on a quiet host and 273 repeats with 35 skips in 180 002 grains while other lab work ran (0.15 %). Each is a paint that reaches the tick a little late. Released with the owner's agreement; the fix follows in 1.0.x.
