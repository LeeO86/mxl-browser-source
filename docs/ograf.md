# OGraf in mxl-browser-source (design, M0)

Status: design approved (M0 review, 2026-10-13); M1 implemented on `feat/ograf` (library, renderer,
Server API, mode, restore). Base: 1.2.0 (main `df529cf`). Target: 1.3.0. The review's answers are folded in
below; §14 lists them.

This adds EBU OGraf v1 to the browser source. Today the function shows one web page. After this change it can
also host up to 8 OGraf graphics on top of each other, with a graphics library, a controller tab, an `ograf`
widget and the OGraf Server API. The output is the same as today: one video flow (fill + key as `v210a` or
`fill_key`) and one audio flow.

References: OGraf Graphics spec v1 (`ebu/ograf` `v1/specification/docs/Specification.md`), Server API v1
(`v1/specification/open-api/server-api.yaml`), GDD (`v1/specification/json-schemas/gdd/README.md`),
SuperFlyTV `ograf-server` (its `renderer-layer` package does the same job in a normal browser).

## 1. Decisions taken by the user (input)

- At most 8 layers per output, z-ordered, one graphic instance per layer.
- Real-time only. Output fill + key as today.
- A graphics library: ZIP upload (one or more manifests per ZIP), validated against the v1 JSON schema,
  thumbnail list, delete.
- A built-in controller tab and an `ograf` widget. A manual form built from the graphic's JSON schema: Load,
  Play / next step, Update, Stop, custom actions, the on-air state per layer. No rundown.
- The OGraf Server API for later external controllers, but self-contained: nothing depends on an external
  controller.
- The CasparCG-style template API stays.
- Graphics work offline (no internet in production).
- A usable first version for the test broadcast in about 2–3 weeks comes first.

## 2. The two modes

The source document (SPEC §4.1) gets one new field:

| Field | Default | Meaning |
| --- | --- | --- |
| `mode` | `page` | `page`: today's behaviour, the browser shows `url`. `ograf`: the browser shows the built-in OGraf renderer page; `url` is kept but not shown. |

- `mode` changes at once, like every source field, and is saved in `config.json`, presets and the export.
- In `ograf` mode, `background`, `zoom`, `device_scale_factor`, `css`, `js`, `audio` and `reload_interval_s`
  still apply. `css` and `js` are injected into the renderer page, not into the graphics. `zoom` and
  `device_scale_factor` should stay at 1, and the UI says so.
- **Switching while on air:**
  - `page` → `ograf` replaces the page with the empty renderer at once.
  - `ograf` → `page` while a layer holds a graphic answers **409** unless the request has `?force=true`
    (`PUT`/`PATCH /api/v1/source`, a preset, a config import). With `force`, every layer is disposed first
    (`dispose()` on each graphic, up to 2 s), then the page is shown.
  - The UI asks for a confirmation and then sends `force`.
- The CasparCG template API (`/api/v1/template/*`) works in `page` mode only. In `ograf` mode it answers
  `409 {"error":"template API needs mode page"}`, and so do `navigate`, `back` and `forward`. The OGraf
  actions answer 409 the other way round (the library routes work in both modes). Nothing
  switches the mode on its own: an automation call must never replace what is on air.
- Interaction (Interact window, mouse and keys into the page) works in both modes. In `ograf` mode clicks go to
  the graphics.

Not chosen: an OGraf layer stack on top of a `page` URL (a background page plus graphics). That would need the
URL page in an iframe of the renderer, with cross-origin, interaction and URL-policy work. If a production
needs a page behind graphics, it uses two browser sources and a keyer, or a later version adds it.

## 3. Renderer: how the 8 layers map onto the one CEF page

One process stays one CEF browser and one page (SPEC §2.3). The renderer is a built-in page,
`https://ograf.local/renderer/` (plain HTML + JS, no framework), embedded in the binary like the UI.

**Layers are same-origin iframes, one per layer** (review Q1):

- The renderer page holds 8 full-raster `<iframe>`s, absolutely positioned, `z-index` = layer number. Layer 8
  is on top. Each iframe loads a tiny layer document (`/renderer/layer.html`), which imports the graphic's
  `main` module, defines its custom element, appends it to `<body>`, calls `load()`, and then forwards the
  actions.
- **Why iframes and not one shared DOM** (ograf-server uses `<div>`s):
  - every graphic gets its own globals, its own CSS and its own `@font-face` names;
  - two graphics that ship different versions of GSAP or Lottie (the EBU examples do) don't clash;
  - a graphic's global CSS (`body { … }`) can't move another layer;
  - `customElements.define` happens per document, so a re-uploaded graphic with the same id gets a clean
    registry when its layer reloads, and nothing leaks into the next graphic on that layer.
- **Cost:** same-origin iframes stay in the same renderer process. One BeginFrame still drives every frame's
  `requestAnimationFrame` and CSS animations, so frame accuracy (SPEC §2.4) is unchanged. It adds a little
  memory per non-empty layer (a document, roughly 1–5 MB). An empty layer is `about:blank`, hidden.
- **What iframes don't fix:**
  - a graphic in an endless loop blocks the whole renderer (same process); the existing hang detection
    (SPEC §13) covers it;
  - a graphic can reach `window.parent`; graphics are trusted content from the library (§9).
- **Transparency:** iframes are transparent when neither document paints a background. The layer document
  sets `background: transparent` and `color-scheme: normal`, because a dark color scheme can give an iframe an
  opaque canvas. M1 tests this with the alpha ramps (`transparency.html` values through a layer).
- Each layer's `<iframe>` gets `allow="autoplay"`, so graphic audio plays like page audio: Chromium mixes it
  into the one audio flow. Graphics usually make no sound; one that does is heard on the output.
- **Fonts:** graphics bundle their own fonts (they are offline). Without one, text falls back to the image's
  system fonts: DejaVu, Liberation 2 (metric-compatible Arial, Times, Courier), Noto, Noto CJK and Noto Color
  Emoji; extra fonts go into `BROWSER_FONTS_DIR`.

**Commands from the function into the page** use the DevTools method the function already uses for the
renderer probe: `Runtime.evaluate` with `awaitPromise: true` and `returnByValue: true`, on the expression
`window.ografRenderer.call(<layer>, "<method>", <params JSON>)`. The result comes back with the call's id
(`onDevToolsResult`). The OGraf return payload (`statusCode`, `statusMessage`, `result`, `currentStep`) arrives
as a value, so the Server API can answer synchronously, as it must. No new IPC, no `ExecuteJavaScript`
fire-and-forget.

- A thrown exception or a rejected promise comes back as an error, and the API answers 550 (OGraf's code for
  an error inside the graphic).
- **Timeout:** `BROWSER_OGRAF_ACTION_TIMEOUT_MS` (default 10000). Then the API answers 504. The layer stays
  in its last known state, with `pending` set, until the promise settles.
- **Order:** the spec says a graphic must take an action at any time, even while an earlier promise is open.
  So the renderer does not serialise actions, with one exception: actions that arrive while `load()` is still
  running wait for it (the spec's MUST). `clear` during a load waits for the load, then calls `dispose()`.

**The function keeps each layer's state** (it is the source of truth, not the page):

| Field | Meaning |
| --- | --- |
| `layer` | 1…8 |
| `graphicId`, `graphicName`, `package` | what is loaded |
| `instanceId` | UUID, new on every load |
| `state` | `empty`, `loading`, `ready` (loaded, not played), `on` (played, at a step), `stopped`, `error` |
| `step` | `currentStep` from the last `playAction` (absent for `stepCount` 0 or after the end) |
| `data` | the `load()` data, with every `updateAction` merged in (shallow) |
| `pending` | the action whose promise is still open, if any |
| `error` | the last error text |

**Recovery:** when the renderer crashes, hangs or is reloaded, the existing recovery (SPEC §13) reloads the
page, and the output holds the last frame meanwhile. Then the function restores every non-empty layer:

1. `load()` with the merged `data`;
2. for layers that were `on`, `playAction({goto: step, skipAnimation: true})`.

A graphic with `stepCount` 0 (play once and out) is not replayed. Metric: `ograf_restores_total{result}`.

**After a process restart** (review Q2): each layer's graphic and merged data are saved
(`<state>/ograf-layers.json`, written atomically on load, update and clear). After a start in mode `ograf` the
layers come back **loaded but not played** (state `ready`, no step): nothing goes on air by itself, and the
operator gets the setup back with one Play.

**renderCharacteristics** passed to `load()`:

- `resolution`: the format's width and height;
- `frameRate`: 25, 29.97, 50 or 59.94;
- `accessToPublicInternet`: false, unless `BROWSER_OGRAF_NETWORK=any` (§5).

## 4. Library

**On disk:** `BROWSER_OGRAF_DIR` (default `<state>/ograf`, on the `/config` volume):

```
<state>/ograf/
  index.json                 # graphics: id → package, manifest path, createdAt, size, warnings
  packages/<package-uuid>/   # one unpacked ZIP, as uploaded
  tmp/                       # uploads in progress (cleared at start)
```

- **A package is one ZIP.** The ZIP may hold several manifests, which share its files (the spec allows this).
- Manifests are found anywhere in the tree (`**/*.ograf.json`). A ZIP that has everything inside one top
  folder works. `__MACOSX/` and dot-files are ignored.
- `index.json` is written atomically (tmp + rename), like `config.json`.
- **Served to the renderer** at `https://ograf.local/packages/<package-uuid>/<path>` by a CEF scheme handler
  (no network, no `file://`). It is the same code as `templates.local`, with the same `..` and symlink checks.
  M1 adds `.mjs` → `text/javascript`, because Chromium refuses a module script with another MIME type.
- **Graphic id:** the manifest's `id`, unique in the library.
  - An upload with an id that already exists **replaces** it: new package, and the old package stays while a
    layer still uses it.
  - A package is deleted when no graphic in the index and no layer refers to it.

**Upload:** `POST /api/v1/ograf/graphics`, body = the ZIP (`application/zip`).

- **The whole ZIP is accepted or refused** (atomic). The answer lists every manifest found, with errors and
  warnings:
  `{"graphics":[{"id","name","replaced":bool,"warnings":[…]}]}` or
  `422 {"error":"…","manifests":[{"path","errors":[…]}]}`.
- **Refused when:**
  - no manifest found;
  - a manifest that doesn't validate against the v1 schema (an unknown field is only a warning: the EBU
    `l3rd-name` example itself has thumbnail fields the schema does not list, and newer manifests add fields);
  - `supportsRealTime` is false;
  - `main` missing, or outside the package;
  - two manifests in the ZIP with the same `id`;
  - unsafe ZIP content (below).
- **Warnings only:**
  - `renderRequirements` that this output does not meet (wrong resolution or frame rate: the EBU weather
    example asks for exactly 25 fps);
  - `accessToPublicInternet` required while the network is `local`;
  - a missing thumbnail file.
- **ZIP safety:**
  - only regular files and directories (no symlinks);
  - no absolute paths and no `..` (zip-slip);
  - at most `BROWSER_OGRAF_UPLOAD_MAX_MB` (default 256) compressed and 4× that unpacked;
  - at most 10 000 entries.
  - The ZIP is unpacked into `tmp/`, checked, then renamed into `packages/`.
- **Size:** today the HTTP server refuses bodies over 4 MiB, and the EBU weather example alone is 9.7 MB. So
  this one route streams the body to `tmp/` with its own limit, instead of reading it into memory.
- **Libraries:**
  - **miniz** (MIT, one C file in `third_party/`, like stb) to unpack.
  - **jsoncons** (Boost licence, header-only, vcpkg) for JSON Schema **draft 2020-12**, which the OGraf
    schemas use. The validator in nmos-cpp (valijson) only knows older drafts.
  - The 9 OGraf schema files are embedded in the binary and resolved by their `$id`. Validation needs no
    network.

**List, thumbnails, delete:** through the Server API (§6): `GET /ograf/v1/graphics`,
`GET /ograf/v1/graphics/{id}/thumbnail?file=…`, `DELETE /ograf/v1/graphics/{id}[?force=true]`.

- **Delete without `force`** unlists the graphic. Its files stay while a layer uses it (as the spec
  recommends), and the layer keeps running.
- The graphics list carries `v_inUse: [{layer, current}]`; `current: false` means the layer still runs the
  replaced version ("replaced, in use on layer n" in the UI).
- **Delete with `force`** first clears the layers that use the graphic.
- A graphic without thumbnails shows a placeholder. The function does not render thumbnails itself.

**Not in v1:**
- a library shared between productions (SPEC §20 item 12);
- export of the library;
- editing graphics.

The library lives on the instance's `/config` volume; the config export (`/api/v1/config/export`) does not
include it.

## 5. Offline and network

- Graphics must not need the internet in production. So that a lab test proves this, the default in `ograf`
  mode is `BROWSER_OGRAF_NETWORK=local` (`local`, `any`, or a comma list of extra hosts):
  - the renderer and the graphics may load only `https://ograf.local/…`;
  - every other request is refused and counted (`navigation_blocked_total{reason="ograf_local"}`), with a
    console message naming the graphic's layer.
- A host list (`data.example, *.scores.example`) allows those hosts too, with the normal URL policy on top;
  for graphics that fetch data from an internal server.
- `any`: the normal URL policy (SPEC §14.3) applies; `accessToPublicInternet` is reported as true.
- Several EBU examples call external services (bar-chart and headline read a Google Sheet). With `local` they
  show their built-in data. M1 documents which examples work offline.

## 6. OGraf Server API

Base: **`/ograf/v1`** on `WEB_PORT`. The spec allows any prefix; `/ograf/v1` is the one it uses in its
examples. With `BROWSER_API_TOKEN` set it needs the bearer token, like `/api/v1`. Errors are RFC 7807
`ErrorResponse` objects, as the spec defines.

**Mapping:**

- **Renderer:** one per process. id `main`, name `BROWSER_LABEL`.
  - `renderTargetSchema`: `{"type":"object","properties":{"layer":{"type":"integer","minimum":1,"maximum":8}},"required":["layer"]}`.
  - `status` is `OK`; `WARNING` while the mode is `page` or the page is loading or recovering; `ERROR` when the
    page is in `error`.
- **RenderTarget** = a layer, `{"layer": n}`, named `Layer n`.
- **GraphicInstance** = what a layer holds, id = the layer's `instanceId`.

| Endpoint | v1 | Notes |
| --- | --- | --- |
| `GET /` | yes | name, description, version |
| `GET /graphics` | yes | |
| `GET /graphics/{id}` | yes | `graphic` = the manifest; `metadata.createdAt`, `updatedAt` |
| `DELETE /graphics/{id}` | yes | `force` as in §4 |
| `GET /graphics/{id}/thumbnail` | yes | `file` must be one of the manifest's `thumbnails[].file` |
| `GET /renderers`, `GET /renderers/{id}` | yes | `renderTargets` = the 8 layers with their instances |
| `GET /renderers/{id}/target?renderTarget=…` | yes | |
| `PUT …/target/graphicInstance/clear` | yes | filters by `renderTarget`, `graphicId`, `graphicInstanceId`; calls `dispose()` |
| `POST …/target/graphicInstance/load` | yes | replaces what the layer holds (dispose first) |
| `POST …/playAction`, `…/stopAction`, `…/updateAction`, `…/customActions/{id}` | yes | `graphicInstanceId` must match the layer's instance, else 404 |
| `POST /renderers/{id}/customActions/{id}` | no custom actions | `customActions: []`; any id → 404 |
| upload | – | not in the spec; our own route, §4 |

- **CORS:** off by default. `BROWSER_OGRAF_CORS_ORIGINS` (a space or comma list of origins, never `*`) adds the
  headers the spec suggests, for those origins only, as `WIDGET_FRAME_ANCESTORS` does for the widgets.
- **Interop:** M3 runs SuperFlyTV's `ograf-server` default controller (its UI) against `/ograf/v1`, plus the
  EBU `renderer-test` graphic.

## 7. Own API and events (for the UI and the widget)

The UI and the widget use the Server API above for everything it covers. Our own routes:

| Method | Path | Purpose |
| --- | --- | --- |
| POST | `/api/v1/ograf/graphics` | upload a ZIP (§4) |
| GET | `/api/v1/ograf/layers` | the 8 layer states (table in §3), plus the mode |

On `/api/v1/events`, a message `{"type":"ograf","layers":[…]}` is sent on every layer change (and once on
connect), so the UI shows on-air state without polling.

## 8. Controller tab and `ograf` widget

**A new UI tab "Graphics"** (Vue, like the others), shown in both modes; with mode `page` it offers the
switch. It has:

- **Library:**
  - the upload button (drag and drop too), with the warnings and errors per manifest;
  - a list with thumbnail, name, id, version, step count and warnings;
  - delete, with a confirmation.
- **Layers:**
  - 8 rows, top layer first: state with colour (empty / ready / ON AIR / stopped / error), graphic, step `n of
    stepCount`, pending action;
  - buttons: Load (the selected graphic), Play, Next (`delta: 1`, shown when `stepCount` > 1 or −1),
    Stop, Clear, and "skip animation".
- **Form** for the selected layer, built from the manifest's `schema`:
  - Update sends `updateAction`, Load sends the form as `load()` data. Defaults come from the schema's
    `default`.
  - **Basic types:** string, number, integer, boolean, enum → select.
  - **GDD types:** single-line and multi-line text, select and select-multiple, colour `RRGGBB` and
    `RRGGBBAA` (colour picker), percentage, duration in ms; file and image paths as text.
  - **Nested:** object as a fieldset; array as a list with add, remove and move.
  - `required` fields are marked, and Update and Load stay off while one is empty. `hidden` and `order` are
    honoured as hints.
  - Anything the generator doesn't know falls back to a JSON editor for that property (GDD's graceful
    degradation).
  - The function passes `data` through unchanged. It does not validate it against the graphic's schema on the
    server side (ograf-server doesn't either). The form guarantees the types.
- **Custom actions:** one button per `customActions[]` entry, with a small form for its `schema` (same
  generator).
- **The preview** (the existing JPEG push) above the layers.

**Widget `ograf`:** the same component without the app around it, following the contract of SPEC §10.6
(`/widgets`, `/widget/ograf?theme=…&layers=1,2,3`, CSP `frame-ancestors`, `widget-ready` and `widget-size`).

- `min_size` 640×480.
- Optional parameter `layers` limits the rows shown (one operator screen per layer group).
- The upload and delete buttons are hidden in the widget; they stay in the tab.

## 9. Security

- Graphics are code. They run like any page, with the container as the boundary (SPEC §14). An upload is as
  powerful as `/api/v1/source/execute`, so it sits behind the same token, and every upload and delete is logged
  with the client address.
- `network=local` (§5) stops a graphic from calling home, and also from reaching the cluster.
- ZIP content is checked (§4). Files are served only from inside a package.

## 10. Settings

| Key | Default | Restart | Meaning |
| --- | --- | --- | --- |
| `BROWSER_OGRAF_DIR` | `<state>/ograf` | yes | library |
| `BROWSER_OGRAF_UPLOAD_MAX_MB` | 256 | yes | largest ZIP (unpacked: 4×) |
| `BROWSER_OGRAF_ACTION_TIMEOUT_MS` | 10000 | yes | wait for a graphic's promise; then 504 |
| `BROWSER_OGRAF_NETWORK` | `local` | yes | `local`, `any`, or a comma list of hosts (§5) |
| `BROWSER_OGRAF_CORS_ORIGINS` | empty | yes | origins allowed to call `/ograf/v1` from a browser (§6); `*` exits 78 |

Fixed, not settings: 8 layers, renderer id `main`, base path `/ograf/v1`. The mode is the source field `mode`.

## 11. Metrics

Prefix `mxl_browser_source_` (so `mxl_browser_source_ograf_actions_total`, …):

| Metric | Type | Labels |
| --- | --- | --- |
| `ograf_actions_total` | counter | `action` (`load`, `play`, `stop`, `update`, `custom`, `clear`), `result` (`ok`, `error`, `timeout`) |
| `ograf_action_seconds` | histogram | `action` (until the graphic's promise settles) |
| `ograf_layers` | gauge | `state` |
| `ograf_graphics` | gauge | – (library size) |
| `ograf_uploads_total` | counter | `result` (`ok`, `rejected`) |
| `ograf_restores_total` | counter | `result` |

The Grafana dashboard gets one row with these metrics.

## 12. Tests

- **Unit** (no CEF):
  - manifest validation on the 9 EBU examples (all valid) and broken manifests (each rule of §4);
  - ZIP safety (zip-slip, symlink, bomb, size, count);
  - the library index (replace, delete with and without force while in use);
  - Server API request parsing (`renderTarget` as a JSON query string, filters);
  - layer state transitions and the restore plan.
- **Integration** (CI, software mode):
  - upload the EBU examples;
  - `renderer-test` through all its steps;
  - l3rd-name load/play/update/stop;
  - scoreboard steps and custom actions;
  - 8 layers at once;
  - alpha through a layer equal to `transparency.html` (±1);
  - a renderer kill → restore to the same step;
  - `network=local` blocks an external fetch;
  - the template API answers 409 in `ograf` mode.
- **Lab (GPU 0):**
  - the EBU examples on 8 layers, fill+key checked in the multiviewer and by grain reads, including alpha
    edges;
  - a soak (M4): hours of load / play / update / stop on 8 layers, with no leaks (`cef_processes_resident_bytes`
    flat), no late or missed grains, and the paint latency recorded.

## 13. Milestones (from the brief)

- **M0:** this document and the questions.
- **M1:**
  - the library: upload, validate, list, thumbnail, delete;
  - the renderer with 8 layers, the `mode` field, recovery and restore;
  - the Server API routes the tab needs: graphics, load, play, stop, update, clear, target;
  - unit and CI tests;
  - lab check of fill+key.
- **M2:** the Graphics tab, the form generator, custom actions, the `ograf` widget.
- **M3:** the rest of the Server API (`/`, renderers, filters, thumbnails, CORS); interop with `ograf-server`'s
  controller.
- **M4:** soak, `docs/performance.md` numbers, README, SPEC (new §4.6 and the tables), CHANGELOG, a platform
  note (new settings, ports unchanged, routes, the library volume), release 1.3.0 after review.

## 14. Review answers (2026-10-13)

- **Q1:** one iframe per layer: yes.
- **Q2:** after a process restart, layers come back loaded, not played (§3); a renderer crash or reload restores
  the step too.
- **Q3:** `local` by default, plus `any` and a host list (§5).
- **Q4:** CORS off by default, per origin, never `*` (§6).
- **Q5:** layers 1–8, 8 on top.
- **Q6:** a ZIP is accepted or refused as a whole; the same id replaces; a layer keeps the old package until it
  is cleared or reloaded, and the list says so (§4).
- **Q7:** miniz and jsoncons, listed in `THIRD_PARTY_NOTICES.md` and in the image's `/usr/share/doc`.
- Notes: the upload route alone streams large bodies; actions on different layers never wait for each other
  (one HTTP thread per request, one DevTools call each); a mode change away from `ograf` needs `force` while
  layers are loaded; metric names carry the repository prefix.
