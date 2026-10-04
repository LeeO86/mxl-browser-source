# mxl-browser-source

Renders a web page offscreen with the Chromium Embedded Framework (CEF) and writes it as MXL flows: video (v210, optionally with a key as v210a or a separate key flow) and audio (float32, 48 kHz). One BeginFrame per MXL grain index keeps the page frame-locked to house time (TAI). Senders register with NMOS (IS-04/IS-05, BCP-007-03) and are routed like every other media function of the MXL PoC platform. An operator sees a preview and can interact with the page from the web UI, like the "Interact" window of the OBS Browser Source; HTML graphics templates are driven CasparCG-style (`play`, `stop`, `next`, `update`).

**Status: specification only.** The behaviour to build is in [`SPEC.md`](SPEC.md). Nothing is implemented yet.

| Port | Setting | Default |
| --- | --- | --- |
| Web UI, REST, WebSockets, `/livez`, `/readyz`, `/metrics` | `WEB_PORT` | 8160 |
| NMOS Node and Connection APIs | `NMOS_PORT` | 3312 (+1 reserved) |

It is planned to run with Docker Compose on a single host and on Kubernetes (the platform's Helm chart lives in `mmz-srf/mxl-poc-platform`); see SPEC §15.

## License

MIT, see [`LICENSE`](LICENSE). The container image will contain CEF and Chromium, which have their own licences (SPEC §3).
