# HTTP and WebSocket API

Everything is on `WEB_PORT` (default 8160). JSON bodies are UTF-8. Errors are `{"error": "<message>"}` with a 4xx/5xx status. With `BROWSER_API_TOKEN` set, every `/api/v1/…` and `/devtools/…` request needs `Authorization: Bearer <token>`; WebSockets pass it as `?token=<token>` (browsers cannot set headers on a WebSocket). SPEC §10 is the normative description; this file is the exact wire format.

## Ops

| Method | Path | Response |
| --- | --- | --- |
| GET | `/livez` | `200 {"live":true}` |
| GET | `/readyz` | `200 {"ready":true}` or `503 {"ready":false,"reason":"…"}` |
| GET | `/statusz` | plain text summary |
| GET | `/metrics` | Prometheus text, prefix `mxl_browser_source_` |

## Information and status

`GET /api/v1/info`

```json
{"version":"1.0.0","cef":"144.0.21+g4f5b28c+chromium-144.0.7559.248","chromium":"144.0.7559.248",
 "mxl_revision":"218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7","nmos_cpp":"fe303849527394b03bdedc8f161f377fe458bb62",
 "format":{"name":"1080p50","width":1920,"height":1080,"rate":"50/1"},
 "key_mode":"v210a","audio_channels":2,"render":{"requested":"auto","mode":"gpu"},
 "label":"Browser","devtools":false,"token":false}
```

`GET /api/v1/status` (also pushed as `{"type":"status",…}` on `/api/v1/events` four times a second)

```json
{"type":"status","ready":true,"not_ready":"",
 "page":{"url":"https://templates.local/bars.html","title":"Bars","state":"loaded","loading":false,"error":"","loads":3,
         "can_go_back":true,"can_go_forward":false},
 "render":{"mode":"gpu","renderer":"ANGLE (NVIDIA Corporation, NVIDIA A16/PCIe/SSE2, OpenGL ES 3.2)","requested":"gpu","degraded":false,
           "format":"1080p50","width":1920,"height":1080},
 "grains":{"video":180000,"key":180000,"repeated":12,"missed":0,"late_paints":0,"begin_frames":180000},
 "audio":{"channels":2,"stream":true,"drift_ppm":3.1,"buffer_ms":60.2,"peaks_dbfs":[-20.1,-20.1],"underruns":0,"overruns":0},
 "interact":{"sessions":2,"controlled":true},
 "nmos":{"registered":true,"node_id":"…","device_id":"…","domain_id":"…","senders":{"video":{"id":"…","flow_id":"…","enabled":true},
         "key":{"id":"…","flow_id":"…","enabled":true},"audio":{"id":"…","flow_id":"…","enabled":true}}},
 "devtools":{"enabled":false,"sessions":0}}
```

`page.state` is `loading`, `loaded`, `error`, `crashed` or `hung`. `not_ready` gives the reason while `ready` is false. `render.mode` is what the page reports (`unknown` before the first load). `grains.repeated` counts repeats for every reason (an unchanged page, a late paint, a page that loads or crashed); `late_paints` counts paints that were never committed because a newer one arrived before the tick. Paint latency and conversion time are histograms in `/metrics`. Senders that do not exist in the current key mode or audio setting are absent.

## Source

`GET /api/v1/source` → the source document:

```json
{"url":"https://templates.local/blank.html","background":"transparent","zoom":1.0,"device_scale_factor":1.0,
 "css":"","js":"","user_agent_suffix":"","reload_interval_s":0,"audio":true}
```

`PUT /api/v1/source` replaces it, `PATCH /api/v1/source` merges the given fields; both apply at once and answer with the new document. `background` is `transparent` or `#RRGGBB`.

| Method | Path | Body | Effect |
| --- | --- | --- | --- |
| POST | `/api/v1/source/navigate` | `{"url":"…"}` | navigate (URL policy applies; 403 when refused) |
| POST | `/api/v1/source/reload` | `{"ignore_cache":false}` | reload |
| POST | `/api/v1/source/stop` | – | stop loading |
| POST | `/api/v1/source/back` | – | back in the page's history (202) |
| POST | `/api/v1/source/forward` | – | forward in the page's history (202) |
| POST | `/api/v1/source/clear-cache` | `{"cookies":false}` | clear the HTTP cache (and cookies) |
| POST | `/api/v1/source/execute` | `{"js":"…"}` | run JavaScript in the main frame (202) |

## Presets

`GET /api/v1/presets` → `[{"name":"Bars","url":"…", …source fields…}]`
`POST /api/v1/presets` with a preset object creates or replaces it by `name`.
`DELETE /api/v1/presets/{name}` deletes. `POST /api/v1/presets/{name}/apply` applies it to the source (answers with the source document).

## Template control

`POST /api/v1/template/{verb}`, answers `202 {"queued":true}`:

| Verb | Body |
| --- | --- |
| `play`, `stop`, `next`, `remove` | – |
| `update` | `{"data": <string or object>}` |
| `invoke` | `{"function":"name","args":[…]}` |

`GET /api/v1/templates` → `{"files":["blank.html","bars.html", …]}` (served to the browser at `https://templates.local/<file>`).

## Console

`GET /api/v1/console` → `{"messages":[{"time":"2026-10-05T10:00:00Z","level":"error","message":"…","source":"…","line":12}]}` (last 200).

## Preview

`GET /api/v1/preview.jpg` → `image/jpeg`, the newest frame at `BROWSER_PREVIEW_WIDTH`, composited over a mid-grey checkerboard where transparent.

## NMOS

`GET /api/v1/nmos` → `{"registered":true,"node_id":"…","device_id":"…","domain_id":"…","senders":{…as in status…}}`

## Configuration

`GET /api/v1/config`

```json
{"settings":[{"key":"BROWSER_FORMAT","value":"1080p50","default":"1080p50","source":"env","restart":true,"secret":false,
              "description":"Video format"}, …],
 "file":"/config/config.json"}
```

`source` is `env`, `file` or `default`. A secret's `value` is `""` and `set` tells whether it has one.

`PUT /api/v1/config` with `{"KEY":"value", "OTHER":null}` writes the file layer (`null` removes the key) and answers `{"restart_required":["KEY"]}`. Keys set by the environment cannot be changed here (409).

`GET /api/v1/config/env` → `text/plain` `KEY=value` lines (secrets omitted).

`GET /api/v1/config/export` → the config document (SPEC §11): `{"version":1,"settings":{…},"source":{…},"presets":[…],"senders":{…}}`.
`POST /api/v1/config/import` with that document → `{"restart_required":[…]}`.

## WebSocket `/api/v1/events`

Server → client only: `{"type":"status",…}` (4 Hz), `{"type":"dialog","kind":"alert","message":"…","result":"dismissed"}`, `{"type":"console",…}`, `{"type":"page","state":"loaded","url":"…","title":"…"}`, `{"type":"event","data":<object from window.mxlBrowserSource.post>}`.

## WebSocket `/api/v1/interact`

The interaction protocol of SPEC §7.2 (schema there).

Client → server (JSON text frames):

```json
{"type":"hello","client":"ui","protocol":1}
{"type":"interact","enable":true,"take":false}
{"type":"pointer","action":"move","x":0.42,"y":0.31,"modifiers":[],"seq":17}
{"type":"pointer","action":"down","x":0.42,"y":0.31,"button":"left","clicks":1,"modifiers":["shift"],"seq":18}
{"type":"pointer","action":"up","x":0.42,"y":0.31,"button":"left","clicks":1,"seq":19}
{"type":"pointer","action":"leave","x":0,"y":0}
{"type":"wheel","x":0.5,"y":0.5,"dx":0,"dy":-120,"modifiers":[]}
{"type":"key","action":"down","code":"KeyA","key":"a","modifiers":[],"repeat":false,"seq":20}
{"type":"key","action":"up","code":"KeyA","key":"a","modifiers":[]}
{"type":"text","text":"Grüezi 你好"}
{"type":"ime","action":"composition","text":"にほ","selection":[2,2]}
{"type":"ime","action":"commit","text":"日本"}
{"type":"focus","focused":true}
{"type":"preview","fps":10,"width":960}
```

Server → client:

```json
{"type":"state","interact":{"enabled":true,"controller":"self","expires_in_s":118},
 "page":{"url":"…","title":"…","loading":false,"error":"","can_go_back":false,"can_go_forward":false},"render":{"mode":"gpu","format":"1080p50","width":1920,"height":1080}}
{"type":"ack","seq":18,"grain":123456789}
{"type":"cursor","cursor":"pointer"}
{"type":"dialog","kind":"confirm","message":"Sure?","result":"cancelled"}
{"type":"console","level":"error","message":"…","source":"…","line":3}
{"type":"error","code":"not_controller","message":"interaction is off for this session"}
```

Binary frames: 8-byte little-endian grain index, then a JPEG (the preview). `controller` is `self`, `other` or `none`. Input without control answers `error` `not_controller`. Coordinates are 0…1 of the picture. Wheel deltas are pixels with the DOM sign (positive `dy` scrolls down). A `down` focuses the page; the buttons that are down ride on every `move` until their `up` (a drag selects text), and `leave` or the end of control sends the `up` of a button still down.

## DevTools (`BROWSER_DEVTOOLS=true` only)

`GET /devtools/json/list`, `GET /devtools/json/version`, the frontend under `/devtools/…`, and the page WebSocket `/devtools/page/<id>` proxied to CEF on `127.0.0.1:BROWSER_DEVTOOLS_PORT`. The UI opens `/devtools/inspector.html?ws=<host>/devtools/page/<id>` (the `devtoolsFrontendUrl` in `/devtools/json/list` already points there). When DevTools is off, every `/devtools/…` path answers 404.
