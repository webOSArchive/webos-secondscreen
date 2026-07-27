#!/bin/bash
# Stream Mac screen + system audio to the TouchPad over UDP push.
# UDP avoids the HTTP -listen pathologies (input backlog burst on connect,
# one-connection listener, restart races). mpegts self-syncs on the
# receiver, so start/stop order doesn't matter.
# Requires: BlackHole 2ch installed and selected as the Mac's sound output.
# Screen and audio MUST be separate avfoundation inputs: combining them in
# one input ("2:BlackHole 2ch") silently drops all audio frames — the
# screen input takes over the capture session clock.
# Usage: ./stream-screen-av.sh [touchpad-ip]
set -e

TP_IP="${1:-192.168.10.90}"

exec ffmpeg -hide_banner \
  -f avfoundation -framerate 30 -capture_cursor 1 \
  -thread_queue_size 1024 -i "2:none" \
  -f avfoundation -thread_queue_size 1024 -i ":BlackHole 2ch" \
  -map 0:v -map 1:a \
  -vf "fps=15,scale=1024:-2" \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -profile:v baseline -level 3.1 -pix_fmt yuv420p \
  -b:v 4M -maxrate 4M -bufsize 1M -g 60 \
  -c:a mp2 -b:a 192k -ar 44100 -ac 2 \
  -f mpegts "udp://$TP_IP:5000?pkt_size=1316"
