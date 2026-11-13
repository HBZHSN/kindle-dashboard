#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${KINDLE_BUILD_DIR:-/var/tmp/kindle-dashboard-arm}
STAGE="$ROOT/dist/stage"
APP="$STAGE/kindle-dashboard"
mkdir -p "$APP/bin" "$APP/certs" "$APP/tests" "$APP/licenses" "$STAGE/extensions/kindle-dashboard-native"
cp "$ROOT/.build/kindle-static/kindle-dashboard" "$APP/bin/"
cp -R "$ROOT/assets" "$APP/"
cp "$ROOT/config.yaml" "$APP/"
cp "$ROOT/tests/sample.json" "$ROOT/tests/demo.yaml" "$APP/tests/"
cp "$ROOT/kindle/control.sh" "$APP/"
cp "$ROOT/.build/cacert.pem" "$APP/certs/"
cp "$ROOT/kindle/menu.json" "$ROOT/kindle/config.xml" "$STAGE/extensions/kindle-dashboard-native/"
cp "$ROOT/THIRD_PARTY.md" "$APP/licenses/"
for spec in 'freetype/docs/FTL.TXT' 'freetype/docs/GPLv2.TXT' 'harfbuzz/COPYING' \
    'png/LICENSE' 'yaml/License' 'openssl/LICENSE.txt' 'curl/COPYING' 'zlib/LICENSE'; do
    if [ -f "$WORK/src/$spec" ]; then
        cp "$WORK/src/$spec" "$APP/licenses/${spec//\//-}"
    fi
done
printf '%s\n' '0.1.0' >"$APP/VERSION"
chmod +x "$APP/control.sh" "$APP/bin/kindle-dashboard"
tar -czf "$ROOT/dist/kindle-dashboard-0.1.0-kpw3.tar.gz" -C "$STAGE" kindle-dashboard extensions
sha256sum "$ROOT/dist/kindle-dashboard-0.1.0-kpw3.tar.gz"
