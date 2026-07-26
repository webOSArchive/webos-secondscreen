#!/bin/bash
# Play an A/V stream on the TouchPad: video -> framebuffer, audio -> ALSA.
# Same on-device ffmpeg decodes both from one mpegts, so A/V stay in sync.
# Usage: ./touchpad-play-av.sh [url] [seconds]
set -e

URL="${1:-udp://0.0.0.0:5000?fifo_size=1000000&overrun_nonfatal=1}"
DURATION="${2:-}"
[ -n "$DURATION" ] && TFLAG="-t $DURATION"

echo "killall vlcplayer 2>/dev/null
APP=/media/cryptofs/apps/usr/palm/applications/org.webosarchive.vlcplayer
GLIBC=/media/cryptofs/apps/usr/palm/applications/com.nizovn.glibc/lib
\$GLIBC/ld.so --library-path \$APP/lib:\$GLIBC \$APP/bin/ffmpeg -hide_banner \
  -probesize 1000000 -analyzeduration 2000000 -threads 2 \
  -i '$URL' $TFLAG \
  -map 0:v -pix_fmt bgra -f fbdev /dev/fb0 \
  -map 0:a -f alsa default 2>&1 | tail -5" | novacom run file://bin/sh
