#!/bin/bash
# Phase 0b: stream the Mac's screen to the TouchPad's VLC.
# Requires Screen Recording permission for the terminal running this.
# "Capture screen 0" is avfoundation device index 2 on this Mac.
# VLC on the TouchPad should open: http://<mac-ip>:8080/tp.ts
# -listen 1 serves one connection; restart after each disconnect so the
# TouchPad can reconnect without relaunching this script.
while true; do
  ffmpeg -hide_banner \
    -f avfoundation -framerate 30 -capture_cursor 1 -i "2:none" \
    -vf "fps=20,scale=1024:-2" \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -profile:v baseline -level 3.1 -pix_fmt yuv420p \
    -b:v 4M -maxrate 4M -bufsize 1M -g 60 \
    -f mpegts -listen 1 "http://0.0.0.0:8080/tp.ts"
  echo "--- client disconnected, restarting listener ---"
  sleep 0.5
done
