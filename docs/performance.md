# Performance

Lab measurements of mxl-browser-source. The 1.0.0 numbers (one page, `counter.html`, latency) are in the README; this file starts with 1.3.0: the frame clock and audio (M6), the frame loop (M5) and the OGraf soaks.

## Frame clock and audio, 2026-10-15 (M6)

**Setup.** Same host as M5 (NVIDIA A16 GPU 0, `--cpuset-cpus 2,4,…,16`, `--ulimit rtprio=99`, frame pipeline on), 1080p50 `v210a`, GPU rendering. Image `1bb08f6`, from before the default changed, so every run sets `BROWSER_FRAME_CLOCK` explicitly. "Before hardening" is image `2f7c726` (M5 head). Lab run directory: `~/mxl-lab/ograf/m6/matrix-1bb08f6/`.

**Method.**
- `tests/integration/cadence_bench.py` measures 120 s after 15 s of warm-up (30 s for YouTube).
- The clips come from `tests/media/make_clips.sh` (150 s, VP9 1080p with the frame number as bar code, Opus with a 1 kHz tone at −12 dBFS) and play through `templates/video.html`.
- Per grain, `grain_reader counter` reads which clip frame reached MXL. That gives content repeats and skips against an ideal sampler of the clip's rate on 50 Hz, counted on fresh grains (Chromium's choice of video frame) and on held grains (a missing paint).
- `getVideoPlaybackQuality()` through DevTools gives the frames Chromium itself dropped.
- `grain_reader audio` finds gaps below −40 dBFS (1 ms windows) and tone phase jumps on the MXL output. The function's counters give underruns.
- A Chromium trace (audio, media, gpu.capture) counts "GenerateMoreAudio Is Behind".
- CPU per process type comes from `/proc` in the container.
- `tests/integration/frameloop_bench.py` covers the animated pages and OGraf layers (150 s).

**Video cadence** (25p and 50p: 3 runs each, mean; others 1 run). "Held" is the share of grains without a new frame; content at 25 fps is ideally 50 % held, 29.97 fps 40.06 %, 50 and 59.94 fps 0 %.

| Content | Clock | Held | Content repeats / skips on fresh grains | Repeats on held grains (per run) | Dropped by Chromium | rAF phase after T(k): centre / spread |
| --- | --- | --- | --- | --- | --- | --- |
| 25p clip | external | 50.75 % | 2.48 % / 1.23 % | 337 | 134 of 9 410 (1.4 %) | 4.1 / 3.2 ms |
| 25p clip | **internal** | **50.00 %** | **0.02 % / 0** | **0** | **0 of 9 408** | **0.0 / 0.1 ms** |
| 50p clip | external | 13.4 % | 15.1 % / 1.1 % | 18 | 2 517 of 18 818 (13.4 %) | 3.7 / 1.6 ms |
| 50p clip | **internal** | **0** | **0 / 0** | **0** | **0 of 18 814** | **0.0 / 0.1 ms** |
| 29.97p clip | external | 42.5 % | 1.6 % / 0.6 % | 148 | 150 of 3 759 (4.0 %) | 3.7 / 3.1 ms |
| 29.97p clip | **internal** | **40.07 %** | 0.44 % / 0 | 16 | **0 of 3 759** | 0.0 / 0.1 ms |
| 59.94p clip | external | 0 | 0 / 9.9 % (steps of 3 instead of 2) | 0 | 1 246 of 7 518 (16.6 %, the rate difference) | 3.4 / 1.2 ms |
| 59.94p clip | **internal** | 0.02 % | 0.02 % / 0.02 % | 0 | 1 247 of 7 517 (16.6 %, the rate difference) | 0.0 / 0.1 ms |
| YouTube 1080p25 (below) | external | 54.2 % | – (no bar code) | – | 128 and 376 of 3 138 | 3.7 / 1.3 ms |
| YouTube 1080p25 | **internal** | **50.03 %** | – | – | **0 and 0 of 3 137** | 0.0 / 0.1 ms |

- **External BeginFrames.** CEF 144 gives each BeginFrame the call time as frame time, a null deadline and a 60 Hz interval (CEF #4311). A `<video>` therefore picks the wrong frame often: Chromium drops 13 % of a 50p clip, and 15 % of fresh grains repeat content. The 25p clip shows about 340 repeats per 2 minutes on held grains (paints that never came).
- **Internal clock.** No content error in 18 814 grains of 50p, exact 2:1 for 25p, and the right 59.94 → 50 pattern. The function's phase error stays ≤ 0.0002 ms in every run, with 0 late anchors and 0 drift re-anchors. The rAF timestamps sit within ±0.07 ms of T(k) + φ (spread 0.1 ms, against 1.2–5.7 ms of dispatch jitter for external BeginFrames).
- **Unanswered BeginFrames** (external only; with the internal clock the function sends none): 50p 24 % (the pipeline turns them into 13.4 % held), 59.94p 9.6 %.
- **`nvidia-smi` polled every second:**
  - external 50p: held 12.2 % (unchanged), but unanswered BeginFrames rise from 24 % to 45 %;
  - internal 50p: held 0 and 0 dropped, unchanged.
- **`--run-all-compositor-stages-before-draw`:**
  - external 25p: becomes clean (0.1 % repeats, 2 dropped);
  - external 50p: breaks (46 % held, 70 % content repeats, 2 909 of 6 273 dropped);
  - internal: identical to without the flag (0 / 0 / 0).
  - Not adopted.
- **`BROWSER_FRAME_CLOCK_PHASE_MS=10`:** the rAF centre moves to 10.03 ms and 50p stays clean (1 held grain in 12 030, 0 dropped). No gain over φ = 0, so the default stays 0.
- **CPU, cores, function / GPU process / renderer / audio service:**
  - 50p: external 0.49 / 0.19 / 0.70 / 0.016 = 1.40; internal 0.55 / 0.23 / 0.86 / 0.016 = 1.65. The extra 0.25 cores come from Chromium decoding and presenting the 13 % of frames it used to drop.
  - 25p: 0.98 → 1.11 cores. YouTube: 1.00 → 1.11 cores.
- Late paints: 0 with external; with internal 0–3 per run (59.94p 1, trace run 3, YouTube 1–3).

**OGraf and animated pages under the internal clock** (`frameloop_bench.py`, every element changes every frame, 150 s = 7 607 grains, held in %, 0 missed grains in every run; the bench's `paints` field counts only paints paired with a BeginFrame, so the paint count here is `paint_copy_seconds`, one per grain):

| Elements / layers | 2 | 4 | 8 | CPU (8) |
| --- | --- | --- | --- | --- |
| Page, internal | 0.07 | 0 | 0 | 1.04 cores |
| Page, external (same image) | 0.04 | 0 | 0.12 | 0.98 |
| OGraf layers, internal | 0.24 | 0 | 0.01 | 1.06 |
| EBU examples, internal | 0 | 0 | 0.13 | 1.56 |
| M5 pipeline, external (m5d, for comparison) | page 0.10 / 0 / 0.07, OGraf 0 / 0 / 0, EBU 0 / 0.07 / 0 | | | 1.04 / 1.44 |

For animations, both clocks give what M5 reached with the pipeline (≤ 0.24 %). The internal clock does not cost frames on OGraf, and CPU is the same.

**YouTube** (Adrian's case: "Dimitri Vegas - Live at Tomorrowland 2026 Mainstage", `3o0T4z6oT4Y`; the player chose 1920×1080 at 25 fps, AV1 + Opus):
- Opening `youtube.com/embed/<id>` as the page fails: YouTube answers `embedder.identity.missing.referrer` (player error 153) when no Referer identifies the embedder. `templates/youtube.html` embeds the player in an iframe, and the bench reads its stats from the iframe's DevTools target (`--target youtube.com/embed`).
- Internal: held 50.05 % and 50.01 % (ideal 50 %), Chromium dropped 0 of 3 137 in both runs, stats for nerds "0 dropped".
- External: held 52.2 % and 56.2 %, dropped 128 and 376 of 3 138 (4 % and 12 %).
- The old image (`2f7c726`, external): held 50.2 %, 4 dropped.
- CPU 1.0–1.1 cores.

**Audio** (gaps on the MXL output / function underruns; tone runs 120 s, 1 kHz, −12 dBFS):

| Run | Before (`2f7c726`) | After, external | After, internal | Internal, `BROWSER_AUDIO_BUFFER_MS=250` |
| --- | --- | --- | --- | --- |
| Tone | 1 gap, 347 ms | 4 gaps, 186 ms | 3 gaps, 75 ms | 5 gaps, 364 ms |
| Tone 5 s on / 3 s off (15 off periods, 3 000 ms each) | 2 995–3 071 ms | 2 992–3 061 ms (mean 3 027) | 3 005–3 061 ms, 0 underruns | – |
| 50p clip (per run) | 9 gaps, 1 046 ms | 0–3 gaps, 0–265 ms (3 runs: 4, 399 ms) | **0** (3 runs) | 1–2 gaps, 4–107 ms (3 runs: 5, 178 ms) |
| 25p clip, 3 runs | – | 7 gaps, 1 270 ms | **0** | – |
| 50p with `nvidia-smi` / with tracing | – | 3, 415 ms / 0 | 3, 267 ms / 3, 257 ms | – |
| YouTube (per run) | 1 underrun (375 ms) | 10 underruns, 1.30 s; 10, 1.81 s | **13, 1.53 s; 12, 1.91 s** | 1, 0.03 s; 4, 0.40 s |
| "GenerateMoreAudio Is Behind" (30 s traces) | – | 0 (50p), 0 (YouTube) | 0 (50p), 0 (YouTube) | 0 (YouTube) |

- **What the hardening does.** Every gap on the output is an underrun the function counts (`audio_underrun_seconds_total` = `audio_realigned_seconds_total` = the measured gap). Gaps are exactly as long as the audio that came late, with no re-priming. The function's own gap and overrun counters stay 0. Stream start and stop cost at most the page's timer jitter (off periods 2 992–3 061 ms against 3 000 nominal). On clips the result is clearly better than before: 50p went from 9 gaps (1 s) to 0 with the internal clock.
- **Where the gaps come from.** Chromium delivers audio late, by up to 380 ms at a time, while its renderer is not behind (0 "Behind" events in every trace). The gaps come in clusters within 1–3 s and do not follow the bench's 20 s polls. The host's other containers are not pinned and share CPUs 2–16, and Chromium's audio path does not run at real-time priority (the tick and the converter do).
- **YouTube is worse than before.** 12–13 underruns per 2 minutes (1.5–1.9 s of silence), against 1 on `2f7c726`. They cluster at the same position of the video (about 1:50–2:25) in every run with both clocks, so they follow the stream, not the frame clock. The old resampler re-primed after an underrun and let the latency grow, which absorbed a burst of late packets. The new one holds the 60 ms target and turns every stall longer than that into a gap.
- **A 250 ms FIFO does not remove the gaps.** YouTube drops to 1–4 underruns (0.03–0.40 s), but the tone and the clips are no better (tone 5 gaps, 364 ms; 50p 5 gaps in 3 runs). It costs 190 ms of latency: `BROWSER_VIDEO_DELAY_GRAINS=auto` delays the video from 3 to 13 grains to stay in sync. In those runs the 50p clip also had 27–40 late paints (0.45–0.68 % held) instead of 0. The default stays 60 ms.
- **Open.** The cause of the YouTube bursts needs the packet arrival times from `OnAudioStreamPacket` around 1:50–2:25 of that video. Then decide between a short re-prime after a burst and a larger target for streamed media.

**Decision (2026-10-15, Adrian).** `BROWSER_FRAME_CLOCK=internal` is the default from 1.3.0, as in OBS and CasparCG. `external` stays an option, and 1080p59.94 and 29.97 fall back to it (`frame_clock_fallback`).

Raw data (lab host): `~/mxl-lab/ograf/m6/matrix-1bb08f6/`:
- `runs.jsonl`, one line per cadence run; `summary.md` is the table of `~/mxl-lab/ograf/m6/summary.py`;
- `frameloop.jsonl`;
- `raw/`: counter, audio and frame traces per run;
- `bench.log`;
- `runs-invalid-yt-referrer.jsonl`: the YouTube runs before `youtube.html`.

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
- WebGPU off (`BROWSER_WEBGPU=false`): `navigator.gpu.requestAdapter()` answers `null` without starting Dawn. The GPU process still logs one `vkCreateInstance() failed: -9` at start (Chromium enables WebGPU-on-Vulkan via GL interop before features are read; only `--ignore-gpu-blocklist`, which GPU mode needs, keeps it on). No "stack smashing" in the container logs kept from the M5 runs; the 8 h soak below had one, at the first load of `renderer-test`, and none in the 8 hours after it.

**Targets** (1080p50, rtprio, pipeline on, image m5d, 600 s each = 30 107 grains):

| Target | Result |
| --- | --- |
| 0 held frames in 10 min with up to 4 layers animating | **Not quite: 40 (0.13 %)** with 4 OGraf layers. Every one of them is a BeginFrame Chromium answered with no paint at all (30 067 paints for 30 107 BeginFrames, 0 late paints); there is no frame to show, so no buffering on the function's side can fill it. Polling `nvidia-smi` every second during a 120 s run raised them from 14 to 24, so GPU-driver queries (the host's Prometheus exporter scrapes every 15 s) explain part, not all. What is achievable on the A16: 0 late paints; 0.0–0.2 % of BeginFrames unanswered by Chromium. |
| ≤ 1 % with 8 | **Met:** OGraf 8 layers 0.07 % (22), EBU examples on 8 layers 0.01 % (4); 150 s runs 0–0.07 %. |
| 0 missed grains | **Met in every M5 run** (352 567 measured grains with the tick `SCHED_FIFO`, 0 missed), and in the 8 h soak below (1 442 229 grains, 0 missed). |

Raw data (lab host): `~/mxl-lab/ograf/m5/` (`baseline-1401`, `matrix-1430` (incl. pipeline v1), `matrix2-1509`, `smi-test`, `trace-1427`; `runs.jsonl` per directory, traces in `traces/`).

## Memory after clearing the layers, 2026-10-14 (M5)

The M4 soak ended with 1 023 MiB CEF RSS after all layers were cleared, against 700 MiB after the start. To tell a per-layer leak from a cache, the same 15-minute run of `ograf_soak.py` (8 layers, the EBU loop, about 7 actions/s, 0 failures) ran on the old image (layers cleared by navigating the iframe to `about:blank`) and on M5 (a clear replaces the iframe element), then all layers were cleared and the processes read after 1 and 5 minutes (`ps` RSS in the container, shared pages counted per process):

| | Idle after start | 1 min after clear, old / M5 | 5 min after clear, old / M5 |
| --- | --- | --- | --- |
| CEF RSS (`cef_processes_resident_bytes`) | 699 / 698 MiB | 976 / 968 MiB | 976 / 968 MiB |
| GPU process | 263 MiB | 419 / 419 MiB | 419 / 419 MiB |
| Renderer (the renderer page and its 8 iframes) | 110 MiB | 228 / 221 MiB | 228 / 221 MiB |
| Function process | 410 MiB | 424 / 422 MiB | 424 / 422 MiB |
| Container (`docker stats`) | 430 MiB | 634 / 627 MiB | 634 / 626 MiB |

- Replacing the iframes frees about 7 MiB in the renderer: the disposed documents, module maps and their GPU resources were already released when the frames were navigated away. The change stays (it costs nothing and makes the release explicit), but it is not the cause.
- What stays after a clear is a high-water level, not a leak: the GPU process keeps about +155 MiB (texture and shared-image pools grown while the weather and lottie graphics loaded) and the renderer +115 MiB (V8 and PartitionAlloc keep freed pages for reuse); nothing is released in the following 5 minutes, and the level after 15 minutes (≈ 970 MiB) is the same as after the 2 h soak (1 023 MiB). It is reached within minutes of playing the heavier graphics and does not grow with the number of actions.
- The 8 h soak below: the rise flattens after about 4 hours (≈ +6 MiB/h in hours 5–8), and the level after clearing is 1 127 MiB.

Raw data: `~/mxl-lab/ograf/m5/mem-old/`, `mem-new/` (`memory.log`, `soak.csv`).

## OGraf soak, 2026-10-14/15 (8 h, 8 layers, M5)

**Setup.** Image `2f7c726` (M5 head, frame pipeline on, tick and converter `SCHED_FIFO` through `--ulimit rtprio=99`). Otherwise the setup and load are those of the 2 h soak below (`ograf_soak.py`, 8 threads, one per layer, 0.6 s between actions, 480 minutes). It ran 18:34–02:35 CEST. All layers were then cleared, and memory was read again 5 minutes later. Host load average 3.4–10.5 (median 6.5), GPU 0 used only by this container.

**Result.**

| | Value |
| --- | --- |
| Duration, grains | 480 min, 1 442 229 video grains (50.0/s) |
| Actions | 208 370 (7.2/s, 404–459 per minute): load, play, update, stop, clear 35 318 each, next 7 063, custom 24 717 |
| Failed actions | **0**; `ograf_actions_total{result="error"\|"timeout"}` 0 |
| Console errors, layers in error, restores | 0, 0, 0; page `loaded` in every sample |
| Missed grains | **0** (2 h soak without real-time priority: 2) |
| Commit lateness (tick → grain committed) | mean 3.1 ms, all ≤ 10 ms |
| BeginFrame dispatch (posted → sent on the UI thread) | 99.6 % ≤ 5 ms; 384 > 60 ms, 25 > 250 ms (UI-thread stalls). None of them cost a grain. |
| Paints, late paints | 1 248 032 paints for 1 442 229 BeginFrames; **3 951 late (0.27 %)** (2 h soak without the pipeline: 4.9 %) |
| Repeated grains | 198 148 (13.7 %): 194 197 BeginFrames without a paint, plus the late paints, plus 28 `hold` at the start |
| Frame ready (BeginFrame → converted) | mean 13.5 ms |
| "stack smashing detected" | **1**, at 18:37 (minute 3), 3 s after `renderer-test` was loaded the first time: Dawn's Vulkan probe ("vkCreateInstance: Found no drivers") in a short-lived helper. No grain was missed and no process that matters was lost. There were none in the 7 h 57 min after it, although `renderer-test` was loaded about 3 500 more times. `BROWSER_WEBGPU=false` does not stop this one probe (see the M5 notes above). It is harmless here and noted as a known log line. |
| Other log lines | `dbus` and `vkCreateInstance() failed: -9` at start (known); an SSL handshake error every 10 min (a page's request to an outside host through the lab's TLS inspection, `net_error -202`) |

**Over time** (mean per hour of the per-minute samples; "distinct" is the number of different graphics on the 8 layers, sampled every second from the `ograf_load` log lines):

| Hour | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Repeated grains (%) | 3.1 | 5.6 | 10.1 | 10.1 | 15.3 | 22.4 | 22.8 | 19.6 |
| Late paints (%) | 0.41 | 0.29 | 0.34 | 0.32 | 0.23 | 0.19 | 0.19 | 0.22 |
| Distinct graphics on air | 5.8 | 4.3 | 3.2 | 3.1 | 2.3 | 2.1 | 2.1 | 2.2 |
| Container CPU (% of a core) | 185 | 173 | 170 | 173 | 165 | 151 | 145 | 146 |
| Actions per minute | 436 | 435 | 435 | 434 | 434 | 433 | 433 | 433 |
| Host load average (1 min) | 6.3 | 6.7 | 6.3 | 6.5 | 6.6 | 6.5 | 6.2 | 6.5 |
| CEF RSS (MiB), mean / max | 1 179 / 1 235 | 1 223 / 1 272 | 1 276 / 1 369 | 1 318 / 1 376 | 1 334 / 1 409 | 1 342 / 1 450 | 1 340 / 1 470 | 1 352 / 1 461 |
| Container memory (MiB) | 823 | 868 | 900 | 921 | 931 | 930 | 923 | 939 |
| GPU 0 memory, median (MiB) | 707 | 698 | 704 | 664 | 682 | 716 | 690 | 695 |
| JS heap used (MiB) | 25.3 | 27.7 | 30.0 | 30.4 | 29.5 | 33.3 | 31.3 | 32.6 |

**Why the repeated grains rose from 3 % to 23 % (4.2 % at minute 98).** The cause is the soak's load generator, not the frame loop:

- A repeated grain is almost always a BeginFrame that Chromium answered with no paint. Only 3 951 of the 198 148 come from late paints. Chromium paints nothing when nothing on the page changed, so these grains show the same picture again, which is correct output. They are not a held animation frame.
- Over the hours the 8 layer threads fell into step. All threads loop over the same 10 graphics, and every graphic's cycle takes the same time on every layer (65.9 s per loop; the share of layer time per graphic was the same in every hour within 0.2 %). By hour 5, layers 1–6 loaded the same graphic within 0.6 s of each other, and layers 7–8 did the same as a second group, 39 s behind. The number of different graphics on air fell from 5.8 to 2.1. Static stretches (a lower third after its in-animation, `mbs-form`, the minimal examples) then coincide on all layers, and Chromium has nothing to paint.
- Per minute, repeated % against distinct graphics on air: r = −0.85. Against the host load average: r = −0.03. Against the actions per minute (constant): r = −0.02. Late paints and CPU fell at the same time as the repeats rose (0.41 → 0.2 %, 185 → 146 % of a core): less was animating at once.
- Why the threads lock: `ograf_soak.py`'s lock covers only its counters. The threads meet in the function's action path, which handles one action at a time on the UI thread. When that path pauses (a heavy load, the once-a-minute JS heap probe, the UI-thread stalls above), all waiting threads continue together, and identical cycle lengths keep them together afterwards. Future soaks should add jitter to the pause between actions so the page mix stays mixed.
- In the 10-minute M5 runs every element changed on every frame, so a BeginFrame without a paint there was a real held frame (0.0–0.2 %). In this soak, "repeated grains" measures the soak's content. The figure for the frame loop is late paints: 0.27 % over 8 h, falling, and 0 missed grains.

**Memory.**

| | Start (no graphic) | End of the soak | 5 min after clearing all layers |
| --- | --- | --- | --- |
| CEF RSS (`cef_processes_resident_bytes`) | 702 MiB | 1 353 MiB | **1 127 MiB** |
| GPU process | 263 MiB | 504 MiB | 504 MiB |
| Renderer (renderer page and its 8 iframes) | 112 MiB | 485 MiB | 290 MiB |
| Function process (`process_resident_memory_bytes`) | 406 MiB | 433 MiB | 433 MiB |

- **The CEF RSS flattens:** +44, +53, +42 MiB in hours 2–4, then +16, +8, −2, +12 MiB in hours 5–8 (≈ +6 MiB/h). GPU memory and the JS heap stay flat; the heap rose by about 7 MiB in the first 3 hours.
- Caveat: the flattening falls in the same hours as the threads locking together, when fewer different graphics were on air at once. Part of the slowdown may come from the lighter mix. Every graphic was still loaded at the same rate.
- **After clearing,** the level depends on how long the run was: 968 MiB after 15 min, 1 023 MiB after 2 h, 1 127 MiB after 8 h. The GPU process keeps its high-water level (504 MiB, +241 MiB over the start; nothing is released when the layers are cleared). The renderer gives back about 195 MiB but keeps +178 MiB.
- This looks like pools that keep growing slowly (texture and shared-image pools in the GPU process, PartitionAlloc and V8 in the renderer), not a per-action leak. 208 370 actions would leak far more at even 1 KB each.
- For long runs, set `BROWSER_MAX_RESIDENT_MB` with headroom (e.g. 2048 for this mix). A restore of the page would then reset the level.

**Pass criteria.** 0 missed grains: **pass**. 0 failed actions: **pass**. Held frames and late paints explained: **pass** (0.27 % late paints; the repeats are static content). RSS flattens: **pass with the caveat above**. No "stack smashing": **1, harmless**, from Dawn's probe at the first `renderer-test` load.

Raw data (lab host): `~/mxl-lab/ograf/soak-m5-20261014-1834/` (`soak.csv`, `soak.log`, `load.log`, `metrics-*.txt`, `ps-*.txt`, `container.log`).

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
