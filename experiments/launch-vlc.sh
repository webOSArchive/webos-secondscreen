#!/bin/bash
# Launch the TouchPad's VLC port with a stream URL.
# Usage: ./launch-vlc.sh [stream-url]
# PDK apps receive params via argv only at launch, so kill any running
# instance first. novacom arg passing is unreliable; pipe commands to sh.
set -e

APP_ID="org.webosarchive.vlcplayer"
URL="${1:-http://192.168.10.37:8080/tp.ts}"

echo "killall vlcplayer 2>/dev/null; sleep 2" | novacom run file://bin/sh
echo "luna-send -n 1 -f luna://com.palm.applicationManager/launch '{\"id\": \"$APP_ID\", \"params\": [\"$URL\"]}'" | novacom run file://bin/sh
