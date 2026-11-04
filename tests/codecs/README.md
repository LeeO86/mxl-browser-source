# Codec check page

`codecs.html` checks what the browser source can play: `canPlayType`, MSE,
the WebRTC receive and send codecs, playback of H.264 and H.265 files (direct
and MSE) and a WebRTC H.264 loopback inside the page. It prints OK/NO per line
and puts the result in the page title (`GET /api/v1/status` → `page.title`).

Make the media next to it and serve the folder over HTTP from an address the
URL policy allows (not loopback):

```sh
F="-f lavfi -i testsrc2=size=1280x720:rate=25 -t 4 -pix_fmt yuv420p"
ffmpeg $F -c:v libx264 -profile:v high -movflags +faststart h264.mp4
ffmpeg $F -c:v libx265 -tag:v hvc1 -movflags +faststart hevc.mp4
ffmpeg -i h264.mp4 -c copy -movflags frag_keyframe+empty_moov+default_base_moof h264-frag.mp4
ffmpeg -i hevc.mp4 -c copy -tag:v hvc1 -movflags frag_keyframe+empty_moov+default_base_moof hevc-frag.mp4
python3 -m http.server 8080
```

The normal image gives NO for every H.264 and H.265 line; the `-codecs` image
gives OK everywhere.
