#!/bin/zsh
# List Dashdance session logs and crash reports nobody has reviewed yet, with the lines that matter.
#   tools/mac/unreviewed.sh              list unreviewed files and their flagged lines
#   tools/mac/unreviewed.sh --mark F...  record files (full paths or basenames) as reviewed
# Review state is local to this Mac: .reviewed-logs in the repo root (git-excluded).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STATE="$ROOT/.reviewed-logs"
touch "$STATE"
if [[ "${1:-}" == "--mark" ]]; then
  shift
  for f in "$@"; do basename "$f"; done >> "$STATE"
  sort -u "$STATE" -o "$STATE"
  exit 0
fi
LOGS="$HOME/Library/Application Support/Dashdance/Logs"
CRASHES="$HOME/Library/Logs/DiagnosticReports"
# A session still being written is not ready for review.
current=""
pgrep -x Dashdance > /dev/null && current="$(readlink "$LOGS/latest.log" 2>/dev/null)"
PATTERN='FATAL|DESYNC|mismatched|force-disconnect|disconnect|connection failed|frontend exception|read failed|recent function entries|\[aot\] missing|no retrace|result: exit=[^0]'
found=0
for f in "$LOGS"/session-*.log(N) "$CRASHES"/Dashdance*.ips(N); do
  name="$(basename "$f")"
  grep -qxF "$name" "$STATE" && continue
  [[ "$name" == "$current" ]] && continue
  found=1
  echo "=== $f"
  if [[ "$f" == *.ips ]]; then
    python3 - "$f" <<'PY'
import json, sys
head, body = open(sys.argv[1]).read().split('\n', 1)
d = json.loads(body); ex = d.get('exception', {})
print(f"crash: {ex.get('type')} {ex.get('signal')} at {json.loads(head).get('timestamp')}")
imgs = d['usedImages']; t = d['threads'][d.get('faultingThread', 0)]
for fr in t['frames'][:16]:
    print('  ', imgs[fr['imageIndex']].get('name', '?'), fr.get('symbol', '?'))
PY
  else
    hits="$(grep -nE "$PATTERN" "$f" | grep -v 'checksums agree' | head -40)"
    if [[ -n "$hits" ]]; then echo "$hits"; else echo "clean: $(grep -m1 -E '^result:' "$f" || echo 'no result line (killed or crashed without FATAL)')"; fi
  fi
done
(( found )) || echo "Nothing unreviewed."
