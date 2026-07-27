#!/bin/bash
# Mac screen + system audio over HTTP for the TouchPad VLC app.
# The VLC port probes with ffprobe first (consumes one connection), then
# plays — the restart loop serves each connection a fresh session, so the
# real playback never sees a stale backlog.
# Requires BlackHole 2ch as the Mac's sound output.

while true; do
  ffmpeg -hide_banner \
    -f avfoundation -framerate 30 -capture_cursor 1 \
    -thread_queue_size 1024 -i "2:none" \
    -f avfoundation -thread_queue_size 1024 -i ":BlackHole 2ch" \
    -map 0:v -map 1:a \
    -vf "fps=15,scale=1024:-2" \
    -c:v libx264 -preset ultrafast -tune zerolatency \
    -profile:v baseline -level 3.1 -pix_fmt yuv420p \
    -b:v 3M -maxrate 3M -bufsize 1M -g 45 \
    -c:a mp2 -b:a 192k -ar 44100 -ac 2 \
    -f mpegts -listen 1 "http://0.0.0.0:8080/tp.ts"
  echo "--- client disconnected, restarting listener ---"
  sleep 0.5
done
