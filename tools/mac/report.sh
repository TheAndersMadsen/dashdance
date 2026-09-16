#!/bin/zsh
# Capture evidence for a crash, desync or disconnect into reports/<stamp>/ (gitignored).
# Usage: tools/mac/report.sh "what happened, in your words"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SUP="$HOME/Library/Application Support/Dashdance"
OUT="$ROOT/reports/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
{
  echo "# Report $(basename "$OUT")"
  echo
  echo "What happened: ${1:-(not given)}"
  echo
  echo "- build: $(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo '+dirty') on $(git -C "$ROOT" branch --show-current)"
  echo "- macOS: $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string)"
} > "$OUT/notes.md"
# The two newest session logs: the one that crashed may not be the latest if the app was reopened.
ls -t "$SUP/Logs"/session-*.log 2>/dev/null | head -2 | while read -r f; do cp "$f" "$OUT/"; done
# Crash reports from the last 24 hours.
find "$HOME/Library/Logs/DiagnosticReports" -name 'Dashdance*.ips' -mtime -1 -exec cp {} "$OUT/" \; 2>/dev/null || true
# The three newest replays: a desync or disconnect is usually in one of them.
ls -t "$SUP/Replays"/*.slp 2>/dev/null | head -3 | while read -r f; do cp "$f" "$OUT/"; done
echo "$OUT"
ls -1 "$OUT"
