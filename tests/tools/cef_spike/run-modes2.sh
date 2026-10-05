#!/bin/bash
# run-modes2.sh: GPU modes with the host's NVIDIA EGL vendor and Vulkan ICD files mounted, and a
# static page (no damage) with and without Invalidate.
COMMON="ozone-platform=headless,autoplay-policy=no-user-gesture-required,disable-features=BackgroundTracing"
SW="$COMMON,disable-gpu,disable-gpu-compositing,use-gl=disabled"
EGL="$COMMON,use-gl=angle,use-angle=gl-egl,enable-gpu-rasterization,ignore-gpu-blocklist"
VK="$COMMON,use-gl=angle,use-angle=vulkan,enable-features=Vulkan,enable-gpu-rasterization,ignore-gpu-blocklist"
STATIC='data:text/html,<body style="margin:0;background:transparent"><div style="position:absolute;left:100px;top:100px;width:200px;height:200px;background:rgba(255,0,0,0.5)"></div><script>document.title="r:static"</script></body>'
GPU=(--gpus device=3 -e NVIDIA_DRIVER_CAPABILITIES=all
     -v /usr/share/vulkan/icd.d/nvidia_icd.json:/usr/share/vulkan/icd.d/nvidia_icd.json:ro
     -v /usr/share/glvnd/egl_vendor.d/10_nvidia.json:/usr/share/glvnd/egl_vendor.d/10_nvidia.json:ro)
run() { # <name> <flags> <invalidate> <url> [gpu]
  local extra=()
  [ -n "${5:-}" ] && extra=("${GPU[@]}")
  echo "== $1"
  sudo -n timeout 120 docker run --rm --name spike-run --shm-size 1g "${extra[@]}" -e SPIKE_FLAGS="$2" -e SPIKE_INVALIDATE="$3" \
    -e SPIKE_URL="$4" -e SPIKE_SECONDS=20 cef-spike:144 2>&1 \
    | grep -E "RESULT|load error|CefInitialize|FATAL|Exiting GPU|shutdown ok|ERROR:.*(vkCreate|eglInit|Display::init|gpu_init)" | cut -c1-230 | head -12
  sudo -n docker rm -f spike-run >/dev/null 2>&1
}
PAGE=file:///opt/spike/spike.html
run static-no-invalidate "$SW" 0 "$STATIC"
run static-invalidate "$SW" 1 "$STATIC"
run gpu-angle-egl-nvidia "$EGL" 0 "$PAGE" gpu
run gpu-angle-vulkan-nvidia "$VK" 0 "$PAGE" gpu
