#!/bin/zsh
# Packages the macOS build as "Dashdance.app" (ad-hoc signed). Nothing from the
# game enters the bundle: only the executable and the vendored Slippi Sys folder.
# Usage: tools/package_macos_app.sh <build-dir> <output-dir>
set -euo pipefail
ROOT="${0:A:h:h}"
BUILD="${1:?build dir}"
OUT="${2:?output dir}"
VERSION="$(head -n1 "$ROOT/VERSION")"
APP="$OUT/Dashdance.app"
EXE="$BUILD/port/melee_port_mac"
[[ -x "$EXE" ]] || { echo "missing $EXE; build target melee_port_mac first" >&2; exit 1; }
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
sed "s/@VERSION@/$VERSION/g" "$ROOT/port/app/macos/Info.plist" > "$APP/Contents/Info.plist"
cp "$EXE" "$APP/Contents/MacOS/Dashdance"
cp -R "$ROOT/port/slippi_sys" "$APP/Contents/Resources/slippi_sys"
# The replay player (tools/mac/build_player.py): the game translated against the Slippi Playback code set.
PLAYER="${DASHDANCE_PLAYER:-$ROOT/build/mac-player/port/melee_port_mac}"
if [[ -x "$PLAYER" ]]; then
  cp "$PLAYER" "$APP/Contents/MacOS/DashdancePlayback"
  cp -R "$ROOT/port/slippi_sys_playback" "$APP/Contents/Resources/slippi_sys_playback"
  echo "replay player: included"
else
  echo "replay player: not built (tools/mac/build_player.py); Watch buttons will say so"
fi
cp "$ROOT/port/app/icons/AppIcon.icon/Assets/glyph.png" "$APP/Contents/Resources/AppMark.png"   # the hero mark in the dashboard
cp -R "$ROOT/port/app/art/controller" "$APP/Contents/Resources/controller"                        # controller editor artwork
# App icon: the Icon Composer bundle (the Dashdance mark as a glass layer over violet) compiled by
# actool into Assets.car + AppIcon.icns, so macOS 26 renders it as Liquid Glass. Older toolchains fall
# back to the flat 1024px master.
# actool lives in Xcode, not in the Command Line Tools; point at Xcode for this one step when it is installed.
ACTOOL_DEV="${DEVELOPER_DIR:-}"; [[ -z "$ACTOOL_DEV" && -d /Applications/Xcode.app/Contents/Developer ]] && ACTOOL_DEV=/Applications/Xcode.app/Contents/Developer
if [[ -n "$ACTOOL_DEV" ]] && DEVELOPER_DIR="$ACTOOL_DEV" xcrun --find actool >/dev/null 2>&1 && DEVELOPER_DIR="$ACTOOL_DEV" xcrun actool "$ROOT/port/app/icons/AppIcon.icon" --compile "$APP/Contents/Resources" \
     --output-format human-readable-text --warnings --errors --output-partial-info-plist "$(mktemp)" --app-icon AppIcon \
     --include-all-app-icons --enable-on-demand-resources NO --development-region en --target-device mac \
     --minimum-deployment-target 26.0 --platform macosx >/dev/null 2>&1 && [[ -f "$APP/Contents/Resources/AppIcon.icns" ]]; then
  echo "icon: Icon Composer bundle compiled with actool"
else
  ICONSET="$(mktemp -d)/AppIcon.iconset"
  mkdir -p "$ICONSET"
  for size in 16 32 128 256 512; do
    sips -z $size $size "$ROOT/port/app/icons/AppIcon-1024.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    sips -z $((size*2)) $((size*2)) "$ROOT/port/app/icons/AppIcon-1024.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
fi
printf 'APPL????' > "$APP/Contents/PkgInfo"
codesign --force --sign - --deep "$APP"
codesign --verify --deep --strict "$APP"
echo "packaged $APP ($VERSION)"
