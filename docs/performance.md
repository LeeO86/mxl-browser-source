# Performance

Lab measurements of mxl-browser-source. The 1.0.0 numbers (one page, `counter.html`, latency) are in the README; this file starts with 1.3.0: the frame loop (M5) and the OGraf soaks.

## Frame loop, 2026-10-14 (M5: held frames, missed grains)

**Setup.** Same host and GPU as the soak below (2× Xeon Gold 6136, NVIDIA A16 GPU 0, driver 595.84), 1080p50, `BROWSER_KEY_MODE=v210a`, `BROWSER_RENDER=gpu`, `--cpuset-cpus 2,4,…,16`, `--shm-size 1g`, and `--ulimit rtprio=99` so the tick (and in 1.3.0 the converter) run `SCHED_FIFO` (checked with `ps -eLo cls,rtprio`; `--cap-add SYS_NICE` alone does nothing for the uid-1000 process, `CapEff` 0). The host was "quiet" in the sense of the lab: no other agent's work, but the permanent containers of other users (mxl-lab-player, -writers, -replay, -browser, -strom) used about 4 cores throughout (sum of their `docker stats`), load average 3–12 (median about 6), GPU 0 used only by this container. Every run records `uptime`, `docker stats` of every container and `nvidia-smi` before, during and after (`runs.jsonl` below).

**Method.** `tests/integration/frameloop_bench.py`: the same continuously animated elements (GSAP timeline, CSS keyframes, GSAP tweens on an SVG, a GSAP-driven counter with a CSS ticker; GSAP 3.12.5 vendored) as one page (`templates/frameloop/index.html?n=N`) or as N OGraf layers of the lab graphic `tests/integration/ograf/anim` at the same places, and (`ebu`) the first N EBU examples of the M4 layer test. Every element changes on every frame, so every repeated grain is a held frame. 150–180 s per run after 15 s warm-up; the 10-minute target runs 600 s.

**Baseline: is it OGraf?** No. Image `1a2466d` (1.3.0 candidate before M5), held frames (repeated grains) in % of grains:

| Animated elements / layers | 2 | 4 | 8 |
| --- | --- | --- | --- |
| Page mode, one page | 1.45 % | 0.95 % | 1.00 % |
| OGraf, one layer each (same elements) | 1.64 % | 0.91 % | 0.99 % |
| OGraf, EBU examples (M4 order) | 1.04 % | 1.01 % | **33.6 %** |

Page and OGraf behave the same with the same content: the late paints are the frame loop's, not the iframes'. With the heavier EBU set on 8 layers (lottie, weather images, renderer-test) the old loop collapses: once paints are late they stay late and each replaces the next (2 526 late paints in 7 607 grains).

**Where the time goes** (trace `GET /api/v1/debug/frame-trace`, `tests/tools/frame_trace.py`, page mode, 8 elements, 1 756 grains, no pipeline; OGraf with 8 layers gives the same within 0.5 ms):

| Stage | All frames, mean / p95 | Frames that missed their tick (132), mean / p95 |
| --- | --- | --- |
| BeginFrame posted by the tick → sent on the UI thread | 0.11 / 0.08 ms | 0.59 / 2.29 ms |
| sent → `OnPaint` (Chromium: main thread, raster, display, GPU readback) | 7.56 / 17.53 ms | **17.91 / 19.75 ms** |
| `OnPaint` copy (8.3 MB, UI thread) | 2.26 / 2.95 ms | 2.74 / 4.19 ms |
| waiting for the converter (the previous conversion) | 0.02 / 0.03 ms | 0.02 / 0.03 ms |
| conversion BGRA → v210 + v210a (4 threads) | 1.74 / 2.80 ms | 2.25 / 3.72 ms |
| BeginFrame posted → frame ready | 11.6 / 22.8 ms | **23.5 / 26.6 ms** |
| next tick − ready (slack) | +6.3 ms | −5.5 ms (p95 −2.7) |

A late frame is one Chromium paints at its display deadline, 17–20 ms after the BeginFrame (about 7.5 % of frames); with the copy and the conversion it is ready 3–8 ms after the tick that wanted it. The GPU readback is inside Chromium's part and not visible separately; the function's own share is the copy and the conversion (≈ 4–5 ms). The tick's commit is 2 ms after T(k) (`commit_lateness_seconds`), unchanged. Without the pipeline only the newest paint is kept, so a late frame repeats the previous one for a grain and is then replaced by the next paint (held + skipped). The old `paint_latency_seconds` hid this: it measured from the newest BeginFrame *posted*, so a paint arriving while the next BeginFrame waited in the UI queue counted as 0–2 ms.

**Each candidate fix, measured on its own** (held frames in % of grains):

| Change (image) | Page 2 / 4 / 8 | OGraf 2 / 4 / 8 | EBU 2 / 4 / 8 | CPU (8) |
| --- | --- | --- | --- | --- |
| Baseline (1a2466d, tick `SCHED_FIFO`) | 1.45 / 0.95 / 1.00 | 1.64 / 0.91 / 0.99 | 1.04 / 1.01 / 33.6 | 0.97 / 1.05 cores |
| Converter `SCHED_FIFO` 9 + WebGPU off (m5b) | 1.67 / 0.92 / 0.93 | 1.46 / 0.89 / 0.93 | – | 0.97 / 1.02 |
| 8 conversion threads instead of 4 (m5d, no pipeline) | – / – / 0.99 | – / – / 0.97 | – | 1.05 / 1.08 |
| Chromium flags `--disable-gpu-vsync --disable-frame-rate-limit --num-raster-threads=4 --disable-features=PaintHolding,ThrottleDisplayNoneAndVisibilityHiddenCrossOriginIframes` (m5d, no pipeline) | – / – / 0.05 | – / – / 1.41 | – | **3.25 / 2.29** |
| Pipeline v1: a paint waits for the second tick after its BeginFrame (tags) | 3.00 / 4.69 / 5.50 | – | – | 1.01 |
| **Pipeline (jitter buffer, `BROWSER_FRAME_PIPELINE=1`, m5d)** | **0.10 / 0 / 0.07** | **0 / 0 / 0** | **0 / 0.07 / 0** | 1.04 / 1.04 / 1.44 |

- Real-time converter: no change (the converter never waited; it is kept because it costs nothing and protects against host load).
- More conversion threads: −0.2 ms conversion, no change in held frames (the conversion is not the long part).
- Flags: `--disable-frame-rate-limit` lets Chromium render free-running between BeginFrames; page mode looked good, but at 3.3 cores instead of 1, and OGraf got worse. Not adopted. GPU rasterisation and threaded compositing were already on.
- Pipeline v1 made it worse: Chromium does not answer every BeginFrame with exactly one paint (some paints come before the next BeginFrame is dispatched, a second paint for the same BeginFrame, and the next BeginFrame then gets none), so a schedule by BeginFrame waits for paints that never come.
- The adopted pipeline keeps up to two converted frames and commits the oldest, so a late frame is shown one tick later instead of being replaced. Replayed offline on the recorded traces first (held 15–29 → 0–1 per 1 750 ticks), then measured. Latency: one period more while a frame waits; the buffer held one frame 72–82 % of the time in the replays, never more than two.
- WebGPU off (`BROWSER_WEBGPU=false`): `navigator.gpu.requestAdapter()` answers `null` without starting Dawn. The GPU process still logs one `vkCreateInstance() failed: -9` at start (Chromium enables WebGPU-on-Vulkan via GL interop before features are read; only `--ignore-gpu-blocklist`, which GPU mode needs, keeps it on). No "stack smashing" in the container logs kept from the M5 runs; the 8 h soak below checks the whole run.

**Targets** (1080p50, rtprio, pipeline on, image m5d, 600 s each = 30 107 grains):

| Target | Result |
| --- | --- |
| 0 held frames in 10 min with up to 4 layers animating | **Not quite: 40 (0.13 %)** with 4 OGraf layers. Every one of them is a BeginFrame Chromium answered with no paint at all (30 067 paints for 30 107 BeginFrames, 0 late paints); there is no frame to show, so no buffering on the function's side can fill it. Polling `nvidia-smi` every second during a 120 s run raised them from 14 to 24, so GPU-driver queries (the host's Prometheus exporter scrapes every 15 s) explain part, not all. What is achievable on the A16: 0 late paints; 0.0–0.2 % of BeginFrames unanswered by Chromium. |
| ≤ 1 % with 8 | **Met:** OGraf 8 layers 0.07 % (22), EBU examples on 8 layers 0.01 % (4); 150 s runs 0–0.07 %. |
| 0 missed grains | **Met in every M5 run** (352 567 measured grains with the tick `SCHED_FIFO`, 0 missed); 8 h soak below. |

Raw data (lab host): `~/mxl-lab/ograf/m5/` (`baseline-1401`, `matrix-1430` (incl. pipeline v1), `matrix2-1509`, `smi-test`, `trace-1427`; `runs.jsonl` per directory, traces in `traces/`).

## OGraf soak, 2026-10-14 (2 h, 8 layers)

**Setup.** Lab host with 2× Xeon Gold 6136 and an NVIDIA A16 (GPU 0 only), driver 595.84. Image built from `feat/ograf-m4` at `1a2466d` (1.3.0 candidate, version string still 1.2.0). 1080p50, `BROWSER_KEY_MODE=v210a`, `BROWSER_RENDER=gpu` (ANGLE on the A16), 2 audio channels, `--cpuset-cpus 2,4,…,16` (8 cores), `--shm-size 1g`, no `CAP_SYS_NICE` (the tick thread runs at normal priority, `tick_priority_normal`). Other lab work ran on the host at the same time, some of it unpinned (load average 7–33 on 48 CPUs).

**Load.** `tests/integration/ograf_soak.py 127.0.0.1:8460 ograf-mbs 120`: 8 threads, one per layer, each in a loop over the library (the 9 EBU examples plus `tests/integration/ograf/form`, layer n starting at graphic n): load (data from the schema's defaults plus text), play, update, next (graphics with `stepCount` ≠ 1), every custom action, stop, clear, 0.6 s apart. Every minute one CSV row: `/api/v1/status` grains, the `ograf_*` metrics, `cef_processes_resident_bytes`, GPU 0 memory (`nvidia-smi`), the renderer's JS heap (`performance.memory`, posted through `window.mxlBrowserSource.post` from `POST /api/v1/source/execute`), the container's CPU and memory (`docker stats`) and new errors in `/api/v1/console`.

**Result.**

| | Value |
| --- | --- |
| Duration, grains | 120 min, 360 179 video grains (50.0/s) |
| Actions | 52 172 (7.2/s): load, play, update, stop, clear 8 847 each, next 1 767, custom 6 189 |
| Failed actions | **0** (HTTP and the graphic's `statusCode`); `ograf_actions_total{result="error"|"timeout"}` 0 |
| Action time (mean, until the graphic's promise settles) | load 0.12 s, play 1.03 s, update 0.41 s, custom 0.57 s, stop 0.75 s, clear 0.005 s (play, stop and update wait for the graphic's animation) |
| Console errors | 0 |
| Renderer crashes, hangs, restores | 0, 0, 0 |
| Missed grains | **2** (0.0006 %) |
| Repeated grains | 31 823 (8.8 %), all `reason="late"` but 26 `hold` from the start |
| Late paints | 17 484 (4.9 %) |
| Paint latency (BeginFrame → `OnPaint`) | mean 9.4 ms; 91.5 % ≤ 15 ms, 99.98 % ≤ 20 ms, all ≤ 30 ms |
| Conversion BGRA → v210 + v210a | mean 1.7 ms |
| CPU (container) | 1.8 cores mean (1.1–2.6 per minute); host CPUs 2–16: 23 % user each. Of the CPU time: the function's own process about one third (`process_cpu_seconds_total`), the CEF processes two thirds |
| Memory (container, `docker stats`) | 793 MiB mean in the first 20 min, 879 MiB in the last 20 min (peak 900 MiB) |
| Memory of the processes at the end | function 449 MB, GPU process 423 MB, renderer 256 MB, network service 98 MB, zygotes and storage 182 MB (RSS; shared pages counted in each) |
| GPU 0 memory | median 680 MiB, 518–1 314 MiB (peaks while the weather graphic loads its images); 421 MiB with all layers empty after the soak |
| GPU 0 utilisation | 33 % mean (7–100 % per sample) |
| JS heap (renderer, used) | 8.5–63 MiB, mean 23.8 MiB first 20 min, 22.0 MiB last 20 min |

**Trends (first 20 min against the last 20 min, mean of the per-minute samples):**

| Series | Min 1–20 | 21–40 | 41–60 | 61–80 | 81–100 | 101–120 | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| CEF RSS (`cef_processes_resident_bytes`, MiB) | 1 150 | 1 183 | 1 198 | 1 197 | 1 216 | 1 221 | +71 MiB (+6 %); about +25 MiB/h after the first 40 min |
| Container memory (MiB) | 793 | 839 | 849 | 848 | 869 | 879 | +86 MiB; about +32 MiB/h after the first 40 min |
| GPU 0 memory (median, MiB) | 699 | 701 | 680 | 670 | 685 | 679 | flat |
| JS heap used (MiB) | 23.8 | 23.6 | 25.3 | 22.1 | 27.9 | 22.0 | flat |
| CPU (% of one core) | 189 | 183 | 183 | 177 | 190 | 178 | flat |

After the soak, with all 8 layers cleared, the CEF RSS stayed at 1 023 MiB (700 MiB after the start, before any graphic), and the renderer process held 289 MB with no graphic loaded.

**Pass criteria.**

| Criterion | Result |
| --- | --- |
| 0 missed grains | **Fail: 2 in 2 h.** The first (min 3, 11:03:28) came with the only crash of a helper process in the run: a graphic's page probed WebGPU, Dawn found no Vulkan driver, and a short-lived helper died with "stack smashing detected" (no renderer or GPU process was lost, nothing in the metrics). The second (min 11) had no log line. In the quiet runs after the soak, 3 more misses came exactly when the host load average jumped from about 7 to 22–33 (other lab work starting), logged by a 1 s poll. A miss is the tick thread waking more than one period late; the writer fills the gap with repeats, so the ring has no hole. Without `CAP_SYS_NICE` the tick thread cannot get `SCHED_FIFO` (the 1.0.0 soak on a quiet host had 0 in 1 h). Not caused by an OGraf action as far as the logs show; a rerun on a quiet host, and one with `--cap-add SYS_NICE`, would settle it. |
| Late paints and repeats explained | **Yes.** Repeats are grains without a fresh paint: a layer that does not change paints nothing (1 static layer: 6 000 of 6 000 grains repeated, 0 late), and a paint that misses its tick repeats the previous frame. Late paints grow with the number of layers animating at once (below): the 8 iframes share one renderer main thread and one compositor, so more paints land in the last few milliseconds before the tick, and with the conversion (≈ 2 ms) they miss it, although every paint arrives within 30 ms. On the output this is a held frame (a 20 ms judder in an animation), never a black, torn or missing frame. |
| No growth in RSS, GPU memory, heap | **GPU memory and JS heap: pass** (flat). **RSS: not flat**: +6 % between the first and last 20 min, still rising slowly (≈ 25 MiB/h, about 0.5 KB per action), and not released when the layers are cleared. It looks like Chromium caches settling in the renderer and GPU process rather than a per-action leak (the JS heap, which would hold leaked graphics, is flat), but 2 h is not enough to tell; a longer soak (8 h) with `BROWSER_MAX_RESIDENT_MB` as the guard is the next step. |
| No failed actions except expected ones | **Pass**: 0 of 52 172. |

**Late paints against the number of layers on air** (after the soak, the first n graphics of the list played, no actions, 120 s = 6 000 grains each):

| Layers on air | Paints | Paint latency mean | Late paints | Repeated | Container CPU |
| --- | --- | --- | --- | --- | --- |
| 1 (form graphic, static once played) | 0 | – | 0 | 6 000 | 0.17 cores |
| 2 (+ scoreboard) | 5 642 | 8.0 ms | 75 (1.3 %) | 113 | 0.83 cores |
| 4 (+ l3rd-name, minimal) | 5 408 | 7.5 ms | 274 (4.6 %) | 314 | 1.05 cores |
| 8 (+ logo, bar chart, headline, renderer-test) | 5 904 | 9.3 ms | 701 (11.7 %) | 790 | 1.53 cores |
| 8, 300 s right after the soak | – | – | 2 677 of 15 000 (17.8 %) | 2 711 | 2.0 cores |

**Other observations.**

- `network=local` held: no request outside the library (`navigation_blocked_total{reason="ograf_local"}` stayed 0; bar-chart and headline fetch their Google Sheet only when one is configured). Chromium's own background connections through the lab proxy failed a TLS handshake 18 times in 2.5 h (`net_error -202`, not from a page or graphic, no effect on the output).
- `ograf_graphics` reports 11 (the 10 soak graphics plus the lab's edges graphic).

Raw data (lab host): `~/mxl-lab/ograf/soak-20261014-1100/` (`soak.csv`, `soak.log`, `metrics-end.txt`, `mpstat.log`, `misswatch.log`, `quiet.log`, `layers.log`).
