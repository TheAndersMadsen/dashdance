---
name: fix-logs
description: Review Dashdance session logs and crash reports that nobody has looked at yet, turn what they show into issues, fix them one commit at a time, rebuild, and keep docs/MAC_FIXES.md and docs/PR.md current. Use when Christopher says "/fix-logs", "check the logs", "look at my crashes", "I had a desync/disconnect/crash", or "fix what broke".
---

# fix-logs

Christopher plays Dashdance and hits crashes, desyncs and disconnects. This skill is the loop that turns
those sessions into fixes on his fork.

## 1. Find what is unreviewed

```bash
tools/mac/unreviewed.sh
```

It lists session logs (`~/Library/Application Support/Dashdance/Logs/`) and macOS crash reports
(`~/Library/Logs/DiagnosticReports/Dashdance*.ips`) not yet in `.reviewed-logs`, with the lines that matter:
`FATAL`, `DESYNC`, disconnects, `[aot] missing`, frontend exceptions, adapter read failures. It skips the
session still being written if Dashdance is running. If it prints "Nothing unreviewed.", say so and stop.

## 2. Triage

For each file, open the full log around the flagged lines, not just the grep. Match a crash report to its
session log by time (the `.ips` timestamp falls inside the session). Classify:

- **Crash**: `FATAL` reason, the "recent function entries" list, the `.ips` stack.
- **Desync**: `DESYNC: checksum mismatch at frame N`. Note the frame and which player.
- **Disconnect**: who dropped. `got disconnect from peer` or `force-disconnecting player N after 7 s stall`
  means the other side went away; `online connection failed` or our own stall means it may be us.
  Christopher suspects some disconnects were his side, so say which it was and why.
- **Noise**: a network blip or a clean exit with nothing actionable.

Check `docs/MAC_FIXES.md` Open issues first. A repeat of a known issue gets its evidence added to that
row, not a new row. Group files that show the same failure.

## 3. Fix, one issue at a time

Work the issue with the most evidence first. Read the code before changing it. Make the smallest change
that could go upstream, following the repo's CLAUDE.md rules (the pinned-input manifest, strict AOT, no
game data in the tree). If the root cause is not provable from the evidence, don't guess a fix: add the
logging that would prove it next time, commit that, and say so.

For each fix:

1. Edit.
2. `tools/mac/rebuild.sh`. It regenerates the input manifest; a plain cmake build fails after any edit.
   If Dashdance is running it only builds, and `tools/mac/rebuild.sh --install-only` installs later.
3. Commit that fix alone. The message says what broke, the evidence, and why the change fixes it.
4. Move or add the row in `docs/MAC_FIXES.md` (Fixed, with the short commit hash), and add one bullet to
   `docs/PR.md` under "What this changes". Commit those docs changes.
5. `git push fork christopher/mac-fixes`.

A fix is "confirmed" only after Christopher has played through the case again. Until then say
"fixed, not yet confirmed in play".

## 4. Mark reviewed

```bash
tools/mac/unreviewed.sh --mark <every file you triaged>
```

Mark a file even when it was noise, so it never comes back. Don't mark a file whose issue you ran out of
time on; leave it for the next run and say which.

## 5. Report

Open with one line: how many sessions and crash reports were reviewed, and how many problems they held.

Then one short block per problem, in plain words (no function addresses unless they are the only name it has):

```
**<what he saw, e.g. "Crash in the middle of a match">** (<when>, <how many times>)
- What went wrong: <the cause, in one or two sentences a player can follow>
- Whose side: <for desyncs and disconnects: us, them, or unknown, and the log line that says so>
- Fix: <what changed and why that stops it>, or "Not fixed yet: <why>, and what now gets logged to catch it"
- Status: fixed and confirmed / fixed, not yet confirmed in play / logged only / noise
```

Files that were clean or noise get one line total, not a block each. End with what, if anything, he should
try in play to confirm a fix.
