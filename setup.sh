#!/bin/zsh
# Dashdance one-command setup. Installs the build tools, fetches the doldecomp/melee checkout the
# port needs, pulls main.dol out of your own disc image, builds, packages and opens the app.
#
#   ./setup.sh /path/to/melee.iso            macOS app (default)
#   ./setup.sh /path/to/melee.iso --ios      iPad/iPhone Simulator app (needs Xcode)
#   ./setup.sh /path/to/melee.iso --visionos Vision Pro Simulator app (needs Xcode)
#   ./setup.sh /path/to/melee.iso --device   your own iPhone/iPad: dist/Dashdance.ipa for AltStore / SideStore /
#                                            Sideloadly (no developer account needed), or add --team <TEAMID>
#                                            (or --team auto) to sign with your Apple ID and install over USB
#
# Environment overrides: DASHDANCE_ISO (disc path), DASHDANCE_DECOMP (existing doldecomp/melee checkout),
# DASHDANCE_JOBS (parallel jobs). Re-running is safe: every step skips work that is already done.
set -euo pipefail
ROOT="${0:A:h}"
step() { printf '\n\033[1;33m==> %s\033[0m\n' "$1"; }
fail() { printf '\033[1;31merror:\033[0m %s\n' "$1" >&2; exit 1; }
ISO="${DASHDANCE_ISO:-}"
TARGET="mac"; TEAM="${DASHDANCE_TEAM:-}"; UDID="${DASHDANCE_UDID:-}"
expect=""
for arg in "$@"; do
  if [[ -n "$expect" ]]; then eval "$expect=\$arg"; expect=""; continue; fi
  case "$arg" in
    --ios) TARGET=ios;; --visionos) TARGET=visionos;; --mac) TARGET=mac;; --device) TARGET=device;;
    --team) expect=TEAM;; --team=*) TEAM="${arg#--team=}";; --udid) expect=UDID;; --udid=*) UDID="${arg#--udid=}";;
    --*) fail "unknown option $arg";; *) ISO="$arg";;
  esac
done

[[ "$(uname)" == "Darwin" ]] || fail "Dashdance builds on macOS (Apple silicon). See README.md for other platforms."
[[ "$(uname -m)" == "arm64" ]] || fail "an Apple silicon Mac is required."
[[ -n "$ISO" ]] || fail "give the path to your Super Smash Bros. Melee NTSC 1.02 disc image: ./setup.sh /path/to/melee.iso"
[[ -f "$ISO" ]] || fail "disc image not found: $ISO"

step "Xcode tools"
if [[ "$TARGET" == "mac" ]]; then
  xcode-select -p >/dev/null 2>&1 || { echo "installing the Command Line Tools (a dialog will open)…"; xcode-select --install || true; fail "re-run ./setup.sh once the Command Line Tools have finished installing."; }
else
  [[ -d /Applications/Xcode.app ]] || fail "the iPad and Vision Pro builds need the full Xcode from the App Store."
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  if [[ "$TARGET" == "ios" || "$TARGET" == "device" ]]; then
    SDK="$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo 0)"
    if [[ "$(printf '%s\n27.1\n' "$SDK" | sort -V | head -1)" != "27.1" ]]; then
      printf '\033[1;33mnote:\033[0m this Xcode has the iOS %s SDK. On iPhone Duo, apps built before the iOS 27.1 SDK run at a smaller,\n' "$SDK"
      printf '      compatible size instead of filling both displays. Install Xcode 27.1 or later and run setup again for the full fit.\n'
    fi
  fi
fi

step "Homebrew packages (cmake ninja python)"
if ! command -v brew >/dev/null; then
  echo "Homebrew is not installed; installing it (https://brew.sh)…"
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  eval "$(/opt/homebrew/bin/brew shellenv)"
fi
for pkg in cmake ninja python; do brew list --versions "$pkg" >/dev/null 2>&1 || brew install "$pkg"; done
python3 -c "import PIL" 2>/dev/null || python3 -m pip install --quiet --user pillow 2>/dev/null || true

step "doldecomp/melee (function names and animation helpers the port reads at build time)"
DECOMP="${DASHDANCE_DECOMP:-$ROOT/deps/melee}"
if [[ ! -d "$DECOMP/src" ]]; then
  mkdir -p "$ROOT/deps"
  git clone --depth 1 https://github.com/doldecomp/melee.git "$DECOMP"
fi
# The port translates the pinned decomp revision; a fresh clone tracks upstream master, which has moved on.
DECOMP_PIN="$(python3 -c "import json; print(json.load(open('$ROOT/tools/port_source_pins.json'))['decomp']['commit'])")"
if [[ "$(git -C "$DECOMP" rev-parse HEAD)" != "$DECOMP_PIN" ]]; then
  step "doldecomp/melee at the pinned revision $DECOMP_PIN"
  git -C "$DECOMP" fetch --depth 1 origin "$DECOMP_PIN"
  git -C "$DECOMP" checkout --detach --quiet FETCH_HEAD
fi

step "main.dol from your disc"
mkdir -p "$ROOT/deps/disc"
DOL="$ROOT/deps/disc/main.dol"
[[ -f "$DOL" ]] || python3 "$ROOT/tools/extract_dol.py" "$ISO" "$DOL"

step "Aurora (pinned window/renderer dependency)"
[[ -d "$ROOT/build/deps/aurora" ]] || "$ROOT/tools/bootstrap_aurora.sh"

JOBS="${DASHDANCE_JOBS:-$(sysctl -n hw.ncpu)}"
case "$TARGET" in
  mac)
    BUILD="$ROOT/build/mac"
    step "Generating the port (this translates the game once; a few minutes)"
    python3 "$ROOT/tools/bootstrap_port.py" --decomp-root "$DECOMP" --dol "$DOL" --build-dir "$BUILD" --gct-base 0x8065CC80 --macos-arch arm64 --stage generate
    step "Building the macOS app"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DMELEE_DECOMP_ROOT="$DECOMP" -DMELEE_DOL_PATH="$DOL" -DMELEE_PORT_GENERATED_DIR="$BUILD/generated/guest" \
      -DMELEE_BUILD_PORT_TESTS=OFF -DMELEE_BUILD_PORT_HEADLESS=OFF -DMELEE_BUILD_PORT_METAL=ON >/dev/null
    cmake --build "$BUILD" --target melee_port_mac --parallel "$JOBS"
    step "Packaging Dashdance.app"
    "$ROOT/tools/package_macos_app.sh" "$BUILD" "$ROOT/dist"
    echo
    echo "Done. Opening dist/Dashdance.app — choose your disc in the dashboard the first time (it is remembered)."
    [[ -n "${DASHDANCE_NO_OPEN:-}" ]] || open "$ROOT/dist/Dashdance.app" --args --iso "$ISO" --choose-disc   # install.sh opens the Applications copy instead
    ;;
  device)
    # Your own iPhone or iPad. The app contains the translated game, so it is for your own device only;
    # see README "iPhone and iPad (your own device)".
    if [[ "$TEAM" == "auto" ]]; then
      TEAM="$(security find-identity -v -p codesigning 2>/dev/null | sed -n 's/.*"Apple Development: .* (\([A-Z0-9]*\))".*/\1/p' | head -1)"
      [[ -n "$TEAM" ]] || fail "no 'Apple Development' signing identity in your keychain; sign in to Xcode (Settings › Accounts) first, or run without --team for an IPA to sideload."
      echo "signing team: $TEAM (from your Apple Development certificate)"
    fi
    if [[ -z "$TEAM" ]]; then
      BUILD="$ROOT/build/ios-device"
      step "Generating the port"
      python3 "$ROOT/tools/bootstrap_port.py" --decomp-root "$DECOMP" --dol "$DOL" --build-dir "$BUILD" --gct-base 0x8065CC80 --macos-arch arm64 --stage generate
      step "Building the iPhone/iPad app (arm64, iOS 17+)"
      cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
        -DMELEE_DECOMP_ROOT="$DECOMP" -DMELEE_DOL_PATH="$DOL" -DMELEE_PORT_GENERATED_DIR="$BUILD/generated/guest" \
        -DMELEE_BUILD_PORT_TESTS=OFF -DMELEE_BUILD_PORT_HEADLESS=OFF -DMELEE_BUILD_PORT_METAL=ON >/dev/null
      cmake --build "$BUILD" --target melee_port_mac --parallel "$JOBS"
      APP="$BUILD/port/Dashdance.app"
      step "Packaging dist/Dashdance.ipa (ad-hoc signed; your sideloading app re-signs it with your Apple ID)"
      codesign --force --sign - --timestamp=none "$APP"
      rm -rf "$ROOT/dist/ipa" && mkdir -p "$ROOT/dist/ipa/Payload" && cp -R "$APP" "$ROOT/dist/ipa/Payload/"
      (cd "$ROOT/dist/ipa" && rm -f ../Dashdance.ipa && zip -qry ../Dashdance.ipa Payload) && rm -rf "$ROOT/dist/ipa"
      echo
      echo "Done: $ROOT/dist/Dashdance.ipa"
      echo "Install it with AltStore (altstore.io), SideStore (sidestore.io) or Sideloadly (sideloadly.io): open the IPA in the"
      echo "sideloading app, sign in with your Apple ID, and it installs on your device (a free Apple ID re-signs every 7 days;"
      echo "a paid developer account lasts a year). Then copy your disc image into the app's folder in the Files app,"
      echo "or drop it in from Finder (device › Files › Dashdance). Prefer USB with your own Apple ID? Re-run with --team auto."
    else
      BUILD="$ROOT/build/ios-xcode"
      BID="app.dashdance.ios.$(echo "$TEAM" | tr '[:upper:]' '[:lower:]')"   # unique per team, so automatic signing can register it
      step "Generating the port"
      python3 "$ROOT/tools/bootstrap_port.py" --decomp-root "$DECOMP" --dol "$DOL" --build-dir "$BUILD" --gct-base 0x8065CC80 --macos-arch arm64 --stage generate
      step "Building and signing with Xcode (team $TEAM, bundle id $BID)"
      cmake -S "$ROOT" -B "$BUILD" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
        -DMELEE_APPLE_TEAM="$TEAM" -DMELEE_IOS_BUNDLE_ID="$BID" -DMELEE_ARTIFACT_VERIFY=OFF \
        -DMELEE_DECOMP_ROOT="$DECOMP" -DMELEE_DOL_PATH="$DOL" -DMELEE_PORT_GENERATED_DIR="$BUILD/generated/guest" \
        -DMELEE_BUILD_PORT_TESTS=OFF -DMELEE_BUILD_PORT_HEADLESS=OFF -DMELEE_BUILD_PORT_METAL=ON >/dev/null
      XLOG="$BUILD/xcodebuild.log"
      if ! xcodebuild -project "$BUILD"/*.xcodeproj -target melee_port_mac -configuration Release -sdk iphoneos -allowProvisioningUpdates -quiet build 2>&1 | tee "$XLOG"; [[ ${pipestatus[1]} -eq 0 ]] || false; then
        if grep -q "No Account for Team\|No profiles for" "$XLOG"; then
          fail "Xcode has your signing certificate but no Apple ID signed in, so it cannot make a provisioning profile. Open Xcode › Settings › Accounts, sign in with your Apple ID, connect and unlock your device, then run this again. Or run without --team to build dist/Dashdance.ipa for AltStore, SideStore or Sideloadly."
        fi
        fail "the signed device build failed; the full Xcode log is in $XLOG"
      fi
      APP="$(find "$BUILD" -maxdepth 4 -name Dashdance.app -path '*Release-iphoneos*' | head -1)"
      [[ -d "$APP" ]] || fail "the signed app did not appear under $BUILD"
      # The Xcode generator writes CMake's post-build resources (Slippi Sys, the mark, the compiled icon) next to a
      # literal 'Release${EFFECTIVE_PLATFORM_NAME}' folder; move them into the app and sign again with the same identity.
      STRAY="$(dirname "$APP")/../Release\${EFFECTIVE_PLATFORM_NAME}/Dashdance.app"
      if [[ -d "$STRAY" ]]; then cp -R "$STRAY"/. "$APP"/; else
        cp -R "$ROOT/port/slippi_sys" "$APP/slippi_sys"; cp "$ROOT/port/app/icons/AppIcon.icon/Assets/glyph.png" "$APP/AppMark.png"; cp -R "$ROOT/port/app/art/controller" "$APP/controller"; fi
      IDENTITY="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=\(Apple Development: [^,]*\)$/\1/p' | head -1)"
      ENT="$(mktemp).plist"; codesign -d --entitlements - --xml "$APP" > "$ENT" 2>/dev/null
      codesign --force --sign "$IDENTITY" --entitlements "$ENT" --timestamp=none "$APP"
      codesign --verify --deep --strict "$APP" || fail "re-signing the app failed"
      rm -rf "$ROOT/dist/ipa" && mkdir -p "$ROOT/dist/ipa/Payload" && cp -R "$APP" "$ROOT/dist/ipa/Payload/"
      (cd "$ROOT/dist/ipa" && rm -f ../Dashdance-signed.ipa && zip -qry ../Dashdance-signed.ipa Payload) && rm -rf "$ROOT/dist/ipa"
      step "Installing on your device"
      if [[ -z "$UDID" ]]; then
        UDID="$(xcrun devicectl list devices --json-output /dev/stdout 2>/dev/null | python3 -c "import sys,json
d=json.load(sys.stdin)['result']['devices']
ok=[x for x in d if x.get('connectionProperties',{}).get('tunnelState')=='connected' or x.get('deviceProperties',{}).get('bootState')=='booted']
print((ok or d or [{}])[0].get('identifier',''))" 2>/dev/null)"
      fi
      if [[ -n "$UDID" ]] && xcrun devicectl device install app --device "$UDID" "$APP" >/dev/null 2>&1; then
        echo "Installed on $UDID. First launch: Settings › General › VPN & Device Management › trust your developer certificate,"
        echo "and Settings › Privacy & Security › Developer Mode must be on. Then copy your disc into the app's folder in Files."
        xcrun devicectl device process launch --device "$UDID" "$BID" >/dev/null 2>&1 || true
      else
        echo "No connected device found (unlock it, plug it in over USB, and tap Trust). The signed app is at:"
        echo "  $APP   and   $ROOT/dist/Dashdance-signed.ipa"
        echo "Install it with: xcrun devicectl device install app --device <identifier> \"$APP\"   (identifiers: xcrun devicectl list devices)"
        echo "or drag the IPA onto your device in Finder / Apple Configurator."
      fi
    fi
    ;;
  ios|visionos)
    if [[ "$TARGET" == "ios" ]]; then BUILD="$ROOT/build/ios-sim"; SYS=iOS; SDK=iphonesimulator; MIN=17.0; else BUILD="$ROOT/build/visionos-sim"; SYS=visionOS; SDK=xrsimulator; MIN=1.0; fi
    step "Generating the port"
    python3 "$ROOT/tools/bootstrap_port.py" --decomp-root "$DECOMP" --dol "$DOL" --build-dir "$BUILD" --gct-base 0x8065CC80 --macos-arch arm64 --stage generate
    step "Building the $SYS Simulator app"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_SYSTEM_NAME=$SYS -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_DEPLOYMENT_TARGET=$MIN \
      -DMELEE_DECOMP_ROOT="$DECOMP" -DMELEE_DOL_PATH="$DOL" -DMELEE_PORT_GENERATED_DIR="$BUILD/generated/guest" \
      -DMELEE_BUILD_PORT_TESTS=OFF -DMELEE_BUILD_PORT_HEADLESS=OFF -DMELEE_BUILD_PORT_METAL=ON >/dev/null
    cmake --build "$BUILD" --target melee_port_mac --parallel "$JOBS"
    APP="$BUILD/port/Dashdance.app"
    step "Installing on a booted Simulator (boot one in Xcode › Open Developer Tool › Simulator first)"
    if xcrun simctl list devices booted | grep -q Booted; then
      xcrun simctl install booted "$APP"
      BID=app.dashdance.ios
      C="$(xcrun simctl get_app_container booted $BID data)"; mkdir -p "$C/Documents"; cp "$ISO" "$C/Documents/melee.iso"
      xcrun simctl launch booted $BID >/dev/null && echo "Launched. Touch controls are on by default; pair a controller in the Simulator's I/O menu."
    else
      echo "No Simulator is booted. Boot one, then: xcrun simctl install booted \"$APP\" and copy your disc into the app's Documents folder (Files app)."
    fi
    ;;
esac
