#!/bin/zsh
# Rebuild Dashdance after a source edit and install it to /Applications.
# The port's input manifest pins every source file, so an edit needs the generate stage first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
B="$ROOT/build/mac"
cd "$ROOT"
if [[ "${1:-}" == "--install-only" ]]; then
  pgrep -x Dashdance > /dev/null && { echo "Quit Dashdance first."; exit 1; }
  rm -rf /Applications/Dashdance.app && ditto dist/Dashdance.app /Applications/Dashdance.app && echo "Installed /Applications/Dashdance.app"; exit 0
fi
python3 tools/bootstrap_port.py --decomp-root deps/melee --dol deps/disc/main.dol --build-dir "$B" \
  --gct-base 0x8065CC80 --macos-arch arm64 --stage generate > "$ROOT/regen.log" 2>&1 || { tail -20 "$ROOT/regen.log"; exit 1; }
cmake -S . -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DMELEE_DECOMP_ROOT="$ROOT/deps/melee" -DMELEE_DOL_PATH="$ROOT/deps/disc/main.dol" -DMELEE_PORT_GENERATED_DIR="$B/generated/guest" \
  -DMELEE_BUILD_PORT_TESTS=OFF -DMELEE_BUILD_PORT_HEADLESS=OFF -DMELEE_BUILD_PORT_METAL=ON > /dev/null
cmake --build "$B" --target melee_port_mac
tools/package_macos_app.sh "$B" dist
if pgrep -x Dashdance > /dev/null; then
  echo "Built dist/Dashdance.app. Dashdance is running, so /Applications was not updated; quit it and run: $0 --install-only"
  exit 0
fi
rm -rf /Applications/Dashdance.app && ditto dist/Dashdance.app /Applications/Dashdance.app
echo "Installed /Applications/Dashdance.app ($(git rev-parse --short HEAD)$(git diff --quiet || echo '+dirty'))"
