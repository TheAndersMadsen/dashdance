#!/usr/bin/env python3
"""Tests for canonical duplicate hooks and fail-closed HLE absorption."""
# SPDX-License-Identifier: GPL-2.0-or-later
import unittest
from pathlib import Path
import sys

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / "recomp"))

import gecko
import recomp


class _Owner:
    name = "PADControlMotor"


class _Symbols:
    def containing(self, address):
        return _Owner() if address == 0x8034DED8 else None


class AbsorbedHooksTest(unittest.TestCase):
    def setUp(self):
        self.gecko_set = recomp.GeckoSet(0x8065CC80)
        self.rumble = next(
            hook for hook in self.gecko_set.main.hooks
            if hook.hook == 0x8034DED8
        )

    def test_identical_duplicate_is_canonicalized_to_later_winner(self):
        duplicates = [
            hook for hook in self.gecko_set.main.hooks
            if hook.hook == 0x8016EA30
        ]
        self.assertEqual(len(duplicates), 2)
        winners, reports = gecko.canonicalize_hooks(duplicates)
        self.assertEqual(len(winners), 1)
        self.assertIs(winners[0], duplicates[-1])
        self.assertEqual(reports, [(0x8016EA30, True)])
        self.assertNotEqual(duplicates[0].words, duplicates[1].words)
        self.assertEqual(
            gecko.canonical_hook_body(duplicates[0]),
            gecko.canonical_hook_body(duplicates[1]),
        )

    def test_optional_codes_are_independent(self):
        flags = {"widescreen", "flash_failed_lcancel"}
        for selected in (set(), {"widescreen"}, {"flash_failed_lcancel"}, flags):
            with self.subTest(selected=selected):
                table, offset = gecko.generate_gct(self.gecko_set.codes, selected)
                patches = gecko.parse_gct(table, 0x8065CC80)
                hooks = {hook.hook for hook in patches.hooks}
                self.assertEqual(0x800C0148 in hooks, "flash_failed_lcancel" in selected)
                self.assertEqual(0x8008D690 in hooks, "flash_failed_lcancel" in selected)
                self.assertEqual(0x8036A4A8 in hooks, "widescreen" in selected)
                self.assertEqual(table[offset:offset + 8] == b"\xff\x00\x00\x00\x00\x00\x00\x00", not selected)

    def test_optional_hooks_keep_their_own_flag(self):
        flags = {hook.hook: hook.optional for hook in self.gecko_set.main.hooks}
        self.assertEqual(flags[0x800C0148], "flash_failed_lcancel")
        self.assertEqual(flags[0x8008D690], "flash_failed_lcancel")
        self.assertEqual(flags[0x8036A4A8], "widescreen")

    def test_generated_tables_fit_the_same_guest_allocation(self):
        self.assertEqual(self.gecko_set.gct_options[3], self.gecko_set.gct)
        full_hooks = {hook.hook: hook for hook in self.gecko_set.main.hooks}
        for mask, selected in enumerate((set(), {"widescreen"},
                                         {"flash_failed_lcancel"},
                                         {"widescreen", "flash_failed_lcancel"})):
            with self.subTest(mask=mask):
                table = self.gecko_set.gct_options[mask]
                self.assertLessEqual(len(table), len(self.gecko_set.gct))
                padded = table.ljust(len(self.gecko_set.gct), b"\0")
                hooks = {hook.hook: hook for hook in gecko.parse_gct(padded, 0x8065CC80).hooks}
                self.assertEqual(0x800C0148 in hooks, "flash_failed_lcancel" in selected)
                self.assertEqual(0x8008D690 in hooks, "flash_failed_lcancel" in selected)
                self.assertEqual(0x8036A4A8 in hooks, "widescreen" in selected)
                for address in (0x800C0148, 0x8008D690, 0x8036A4A8):
                    if address in hooks:
                        self.assertEqual(gecko.canonical_hook_body(hooks[address]),
                                         gecko.canonical_hook_body(full_hooks[address]))

    def test_pinned_handle_rumble_hook_is_absorbed(self):
        hooks = [self.rumble]
        recomp.validate_absorbed_hooks(hooks, _Symbols(), {"PADControlMotor"})
        self.assertEqual(self.rumble.absorbed_by, "PADControlMotor")

    def test_handle_rumble_fingerprint_drift_fails_closed(self):
        changed = gecko.Hook(
            self.rumble.hook, self.rumble.cave_addr, list(self.rumble.words)
        )
        changed.words[0] ^= 1
        with self.assertRaisesRegex(ValueError, "fingerprint"):
            recomp.validate_absorbed_hooks(
                [changed], _Symbols(), {"PADControlMotor"}
            )

    def test_unregistered_hle_overlap_fails_closed(self):
        unregistered = gecko.Hook(0x8034DED8, self.rumble.cave_addr,
                                  list(self.rumble.words))
        with self.assertRaisesRegex(ValueError, "not registered"):
            recomp.validate_absorbed_hooks(
                [unregistered], _Symbols(), {"PADControlMotor"}, {}
            )


if __name__ == "__main__":
    unittest.main()
