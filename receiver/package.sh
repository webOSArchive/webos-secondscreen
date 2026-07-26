#!/bin/bash
# Stage and build the IPK. Run after build.sh.
set -e
cd "$(dirname "$0")"

STAGE=build/pkg
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp appinfo.json icon.png icon-256.png "$STAGE/"
cp build/secondscreen "$STAGE/"
chmod +x "$STAGE/secondscreen"

palm-package "$STAGE" -o build/
ls -l build/*.ipk
