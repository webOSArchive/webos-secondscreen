#!/bin/bash
# Phase 0a: stream a synthetic test pattern to the TouchPad's VLC.
# Validates network path + libVLC software decode before involving screen capture.
# VLC on the TouchPad should open: http://<mac-ip>:8080/tp.ts
# The TouchPad VLC app probes the URL with ffprobe before playing, and
# -listen 1 serves only one connection — so restart after every disconnect.
while true; do
  ffmpeg -hide_banner -re \
    -f lavfi -i "testsrc2=size=1024x768:rate=20" \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -profile:v baseline -level 3.1 -pix_fmt yuv420p \
    -b:v 3M -maxrate 3M -bufsize 1M -g 48 \
    -f mpegts -listen 1 "http://0.0.0.0:8080/tp.ts"
  echo "--- client disconnected, restarting listener ---"
  sleep 0.5
done
