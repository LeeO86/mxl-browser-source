#!/bin/bash
# run-modes.sh: the CEF spike in software and GPU modes on the lab host (no X server).
COMMON="ozone-platform=headless,autoplay-policy=no-user-gesture-required,disable-features=BackgroundTracing"
SW="$COMMON,disable-gpu,disable-gpu-compositing,use-gl=disabled"
EGL="$COMMON,use-gl=angle,use-angle=gl-egl,enable-gpu-rasterization,ignore-gpu-blocklist"
SS="$COMMON,use-gl=angle,use-angle=swiftshader,enable-unsafe-swiftshader"
VK="$COMMON,use-gl=angle,use-angle=vulkan,enable-features=Vulkan,enable-gpu-rasterization,ignore-gpu-blocklist"
run() { # <name> <flags> <invalidate> [gpu]
  local gpu=()
  [ -n "${4:-}" ] && gpu=(--gpus device=3 -e NVIDIA_DRIVER_CAPABILITIES=all)
  echo "== $1"
  sudo -n timeout 120 docker run --rm --shm-size 1g "${gpu[@]}" -e SPIKE_FLAGS="$2" -e SPIKE_INVALIDATE="$3" -e SPIKE_SECONDS=20 cef-spike:144 2>&1 \
    | grep -E "RESULT|audio started|load (end|error)|CefInitialize|FATAL|GPU process|gpu_init|Exiting GPU|shutdown ok|ERROR:.*(gpu|egl|vulkan|angle)" | cut -c1-260 | head -16
}
run software "$SW" 1
run software-no-invalidate "$SW" 0
run software-swiftshader "$SS" 1
run gpu-angle-egl "$EGL" 1 gpu
run gpu-angle-vulkan "$VK" 1 gpu
