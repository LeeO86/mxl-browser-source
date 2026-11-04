# Image with H.264 and H.265 (proprietary codecs)

> **Licensing warning.** H.264 (AVC) and H.265 (HEVC) are covered by patents and
> patent pools. This image variant contains decoders for both. Whoever deploys it is
> responsible for any licences that apply to their use. This project gives no
> licence and no advice on it. Use the normal image if in doubt.

The normal image uses the CEF build from the CEF project, which has only royalty-free
codecs (VP8, VP9, AV1). Pages that play H.264 or H.265 stay black there, for example
the WebRTC previews of most media functions.

The variant `ghcr.io/leeo86/mxl-browser-source:<version>-codecs` is the same function
with a CEF build of the same version (`CEF_VERSION` in `docker/Dockerfile`) that adds:

| What | H.264 | H.265 |
|---|---|---|
| `<video>` and MSE (FFmpeg, software) | yes | yes |
| WebRTC receive (FFmpeg, software) | yes | offered (Main, Main 10), not tested end to end |
| WebRTC send (encode) | OpenH264 | no |

A GPU decoder is not needed and not used. (Chromium on Linux decodes video in
hardware only through VA-API, Intel and AMD; NVIDIA's NVDEC is not supported.)

Checked with `tests/codecs/codecs.html` in the variant (software rendering): H.264
and H.265 1280x720 play in `<video>` and through MSE, a WebRTC H.264 loopback
decodes, and H.265 is offered for WebRTC receive. WebRTC H.265 was not tested end
to end: the build has no H.265 encoder, and no external H.265 WebRTC sender was
at hand.

## How the CEF build is made

CEF's `automate-git.py` at the CEF commit of `CEF_VERSION`, Linux x64, Release, with:

```
GN_DEFINES=is_official_build=true use_sysroot=true symbol_level=0 is_cfi=false
  chrome_pgo_phase=0 proprietary_codecs=true ffmpeg_branding="Chrome"
  enable_hevc_parser_and_hw_decoder=true rtc_use_h264=true rtc_use_h265=true
```

`proprietary_codecs` and `ffmpeg_branding="Chrome"` give H.264. Chromium decodes H.265
only with a hardware decoder, so three patches in `docker/cef-codecs/` add a software
path (apply after CEF's own patches):

- `ffmpeg-hevc.patch` (in `third_party/ffmpeg`): the FFmpeg configuration for
  Chrome/linux/x64 regenerated with the HEVC decoder and parser
  (`media/ffmpeg/scripts/build_ffmpeg.py linux x64 --branding=Chrome`, then
  `copy_config.sh`), and their sources added to `ffmpeg_generated.gni`. Four of
  them share a file name with an existing source, so they are built through
  `autorename_*` wrappers (as `generate_gn.py` does), and the target also
  searches `libavcodec/` for headers (the HEVC files include them by bare name).
- `chromium-media-hevc.patch` (in `src`): the FFmpeg video decoder accepts HEVC (and
  picks its thread count like H.264), HEVC counts as a built-in codec, FFmpeg's
  allowed decoders include it, and `build_ffmpeg.py` keeps the flags.
- `webrtc-h265-software.patch` (in `third_party/webrtc`): WebRTC's FFmpeg H.264
  decoder also decodes H.265, and the built-in decoder factory offers H.265.

Then `make_distrib` builds the minimal distribution. It is attached to a GitHub
release of this repository; `docker/cef-codecs/cef.env` names its URL and SHA-1.
A tag `codecs-<version>` builds the image (`.github/workflows/container-codecs.yaml`):
`docker/Dockerfile` with `CEF_URL` and `CEF_SHA1` set to that file.
