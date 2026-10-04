# Agent notes

- Read `SPEC.md` before changing behaviour. Create `IMPLEMENTATION_PLAN.md` with the first code: pins, spike results (SPEC §19), source layout, every deviation from the spec (numbered), and the platform guideline G1–G14 table (Item | Requirement | Status | Evidence | Change), as in mxl-multiviewer.
- The platform guideline (`mmz-srf/mxl-poc-platform` `docs/requests/leeo86-v1-readiness.md`, G1–G14) and its follow-ups win over this repo's habits. Record any conflict in SPEC §20.
- C++20, CMake ≥ 3.24, Ninja. No GStreamer in the media path. `-Wall -Wextra`; a clean build is the lint signal.
- Pins, each in exactly two places (`docker/Dockerfile` build args and `.github/workflows/ci.yaml`, with a "keep in sync" comment):
  - CEF `144.0.21+g4f5b28c+chromium-144.0.7559.248` (Linux x64 minimal distribution, hash-checked);
  - MXL `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` with `-DMXL_ENABLE_FABRICS_OFI=OFF`, fetched by the full SHA;
  - nmos-cpp `fe303849527394b03bdedc8f161f377fe458bb62`.
- All media-path time is TAI from MXL's index functions. Never add or subtract the UTC offset. One BeginFrame and one grain per index; a late page repeats the last frame and is counted, it never stalls the writer.
- Use nmos-cpp, not hand-built IS-04 JSON. The device registers `urn:x-nmos:control:sr-ctrl/v1.1`; every sender has its own label. nmos-cpp swallows listener errors: check the NMOS listener after start and exit 75.
- Never overwrite an existing `domain_def.json` or `options.json`; keep a different existing id and log it. Never write into another domain or a mirror (`x-mxl-fabrics-agent.mirror`). Raise `RLIMIT_NOFILE` at start.
- The AVX2 conversion must produce the same bytes as the scalar reference; test both on many widths and edge values.
- Config precedence: environment > `BROWSER_CONFIG_FILE` > defaults. Unknown environment variables are ignored. Invalid config exits 78; a port, CEF or MXL that cannot start exits 75; SIGTERM exits 143.
- Container: uid 1000 (Ubuntu 24.04 already has it), `ARG`s used in `FROM` declared before the first `FROM`, the `mallinfo` shim preloaded, CA imported into the NSS database (Chromium ignores `SSL_CERT_FILE`). Behind the corporate proxy, build with the `HTTP(S)_PROXY`/`NO_PROXY` and `EXTRA_CA_CERT_B64` build args.
- Image tags: `git-<sha7>` and `nightly-dev` from `main`, `X.Y.Z`/`X.Y`/`X` from a `vX.Y.Z` tag. No `latest`; tags are never moved. `io.dmf.mxl.revision` is the full MXL commit. Example manifests reference released tags only.
- MXL tests need a real tmpfs (`--tmpfs /mnt/mxl:size=2g`); the container's 64 MiB `/dev/shm` is too small for Chromium and video flows.
- Commits: one imperative sentence ending with a period, a body with the reason and any lab numbers. Pull requests are squash-merged. Releases: version in `CMakeLists.txt` and `src/version.hpp`, `CHANGELOG.md` (version headings without dates), tag `vX.Y.Z` on `main`, GitHub release with notes and the image digest.
- Line endings are LF (`.gitattributes`). On Windows clones set `core.autocrlf=false`.
