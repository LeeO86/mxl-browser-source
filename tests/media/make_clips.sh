#!/bin/bash
# SPDX-License-Identifier: MIT
# Test clips for the video and audio continuity tests (docs/performance.md, M6): VP9 + Opus WebM,
# 1920x1080, with the frame number as counter.html's bar code (top 16 lines, bit i = a 60 px
# block, white = 1; tests/tools/grain_reader counter decodes it) over testsrc2, and a continuous
# 1 kHz tone at -12 dBFS on both channels (grain_reader audio finds gaps and phase jumps in it).
#
#   tests/media/make_clips.sh OUTDIR [SECONDS] [RATES]
#
# RATES defaults to "25 50 30000/1001 60000/1001"; files are clip-25.webm, clip-50.webm,
# clip-29.97.webm and clip-59.94.webm. Needs ffmpeg with libvpx-vp9, libopus, drawbox and testsrc2
# (Ubuntu 24.04's /usr/bin/ffmpeg has them). Served as a template, for example
# https://templates.local/m6/video.html?src=clip-50.webm (templates/video.html).
set -euo pipefail
out=${1:?outdir}
seconds=${2:-180}
rates=${3:-"25 50 30000/1001 60000/1001"}
FFMPEG=${FFMPEG:-ffmpeg}
mkdir -p "$out"

boxes="drawbox=x=0:y=0:w=1920:h=16:color=black:t=fill"
for i in $(seq 0 31); do
    boxes+=",drawbox=x=$((60 * i)):y=0:w=60:h=16:color=white:t=fill:enable='eq(mod(floor(n/pow(2\\,$i))\\,2)\\,1)'"
done

for rate in $rates; do
    case $rate in
    30000/1001) name=29.97 ;;
    60000/1001) name=59.94 ;;
    *) name=$rate ;;
    esac
    gop=$(python3 -c "from fractions import Fraction as F; print(round(2 * F('$rate')))")
    "$FFMPEG" -hide_banner -loglevel error -y \
        -f lavfi -i "testsrc2=size=1920x1080:rate=$rate:duration=$seconds" \
        -f lavfi -i "aevalsrc=0.2512*sin(2*PI*1000*t):s=48000:c=stereo:d=$seconds" \
        -filter_complex "[0:v]$boxes,format=yuv420p[v]" -map "[v]" -map 1:a \
        -c:v libvpx-vp9 -crf 30 -b:v 0 -row-mt 1 -deadline good -cpu-used 4 -g "$gop" \
        -c:a libopus -b:a 128k -ar 48000 \
        "$out/clip-$name.webm"
    echo "$out/clip-$name.webm"
done
