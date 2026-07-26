#!/bin/bash
# Build, bundle, sign, and (optionally) notarize webOS Second Screen.app.
#
#   ./package-app.sh                 build + sign
#   NOTARIZE=1 ./package-app.sh      also notarize + staple (needs stored
#                                    credentials: xcrun notarytool
#                                    store-credentials "$NOTARY_PROFILE")
#
# Env: SIGN_ID (default: the Developer ID Application cert in the keychain),
#      NOTARY_PROFILE (default "notary").
set -euo pipefail
cd "$(dirname "$0")"

APP_NAME="webOS Second Screen"
BUNDLE_ID="org.webosarchive.secondscreen.sender"
# single source of truth: the appVersion constant in UpdateCheck.swift
VERSION="$(sed -n 's/^let appVersion = "\(.*\)"$/\1/p' Sources/secondscreen-sender/UpdateCheck.swift)"
[[ -n "$VERSION" ]] || { echo "cannot read appVersion from UpdateCheck.swift" >&2; exit 1; }
ART="../artwork"
OUT="dist"
APP="$OUT/$APP_NAME.app"
SIGN_ID="${SIGN_ID:-$(security find-identity -v -p codesigning | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')}"
NOTARY_PROFILE="${NOTARY_PROFILE:-notary}"

echo "==> building (release)"
swift build -c release
BIN=".build/release/secondscreen-sender"

echo "==> icon"
rm -rf "$OUT"; mkdir -p "$OUT/AppIcon.iconset"
# artwork: 48 (unused — no slot), 64, 256, 512; downscale 512 for the rest
sips -z 16 16   "$ART/webOS-SecondScreen-512.png" --out "$OUT/AppIcon.iconset/icon_16x16.png"      >/dev/null
sips -z 32 32   "$ART/webOS-SecondScreen-512.png" --out "$OUT/AppIcon.iconset/icon_16x16@2x.png"   >/dev/null
sips -z 32 32   "$ART/webOS-SecondScreen-512.png" --out "$OUT/AppIcon.iconset/icon_32x32.png"      >/dev/null
cp              "$ART/webOS-SecondScreen.png"           "$OUT/AppIcon.iconset/icon_32x32@2x.png"
sips -z 128 128 "$ART/webOS-SecondScreen-512.png" --out "$OUT/AppIcon.iconset/icon_128x128.png"    >/dev/null
cp              "$ART/webOS-SecondScreen-256.png"       "$OUT/AppIcon.iconset/icon_128x128@2x.png"
cp              "$ART/webOS-SecondScreen-256.png"       "$OUT/AppIcon.iconset/icon_256x256.png"
cp              "$ART/webOS-SecondScreen-512.png"       "$OUT/AppIcon.iconset/icon_256x256@2x.png"
cp              "$ART/webOS-SecondScreen-512.png"       "$OUT/AppIcon.iconset/icon_512x512.png"
iconutil -c icns "$OUT/AppIcon.iconset" -o "$OUT/AppIcon.icns"

echo "==> bundle"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/secondscreen-sender"
cp "$OUT/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>       <string>secondscreen-sender</string>
    <key>CFBundleIdentifier</key>       <string>$BUNDLE_ID</string>
    <key>CFBundleName</key>             <string>$APP_NAME</string>
    <key>CFBundleDisplayName</key>      <string>$APP_NAME</string>
    <key>CFBundleIconFile</key>         <string>AppIcon</string>
    <key>CFBundlePackageType</key>      <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>$VERSION</string>
    <key>CFBundleVersion</key>          <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>   <string>13.0</string>
    <key>LSUIElement</key>              <true/>
    <key>NSHumanReadableCopyright</key> <string>© 2026 webOS Archive</string>
</dict>
</plist>
EOF

echo "==> sign ($SIGN_ID)"
codesign --force --options runtime --timestamp --sign "$SIGN_ID" "$APP"
codesign --verify --strict --verbose=2 "$APP"

echo "==> zip"
ditto -c -k --keepParent "$APP" "$OUT/$APP_NAME.zip"

if [[ "${NOTARIZE:-0}" == "1" ]]; then
    echo "==> notarize (profile: $NOTARY_PROFILE)"
    xcrun notarytool submit "$OUT/$APP_NAME.zip" \
        --keychain-profile "$NOTARY_PROFILE" --wait
    echo "==> staple"
    xcrun stapler staple "$APP"
    rm "$OUT/$APP_NAME.zip"
    ditto -c -k --keepParent "$APP" "$OUT/$APP_NAME.zip"
    spctl -a -vv "$APP"
else
    echo "==> skipping notarization (set NOTARIZE=1; store credentials once with:"
    echo "    xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <id> --team-id Z97JEYX9UJ)"
fi

echo "==> done: $APP"
