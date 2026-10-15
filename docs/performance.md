# Performance

Lab measurements of mxl-browser-source. The 1.0.0 numbers (one page, `counter.html`, latency) are in the README; this file starts with the OGraf soak of 1.3.0.

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
