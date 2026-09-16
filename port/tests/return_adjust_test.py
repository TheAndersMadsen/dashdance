#!/usr/bin/env python3
"""Callees that return past their call site (Gecko `lwz; addi rX, rX, 8; mtlr; blr`) resume the caller there."""
# SPDX-License-Identifier: GPL-2.0-or-later
import unittest
from pathlib import Path
import sys

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / "recomp"))

from analyze import _return_adjust  # noqa: E402
from gekko import decode  # noqa: E402

# Tail of UCF 0.84 "Shield Drop" (hooked into ftCo_80099894): unwind, return to the caller's return address + 8.
UCF_TAIL = [0x80E1001C, 0x38210018, 0x38E70008, 0x7CE803A6, 0x4E800020]
# An ordinary epilogue: lwz r0, 28(r1); addi r1, r1, 24; mtlr r0; blr.
EPILOGUE = [0x8001001C, 0x38210018, 0x7C0803A6, 0x4E800020]
# `mflr r5; addi r5, r5, 4; mtlr r5; blr`: return past an inline data word.
MFLR_SKIP = [0x7CA802A6, 0x38A50004, 0x7CA803A6, 0x4E800020]


def insns(words, base=0x80000000):
    return [decode(base + 4 * i, w) for i, w in enumerate(words)]


def mtlr_index(seq):
    return next(i for i, ins in enumerate(seq) if ins.op == "mtspr" and ins.f["spr"] == 8)


class ReturnAdjustTest(unittest.TestCase):
    def test_ucf_shield_drop_returns_eight_past_the_call(self):
        seq = insns(UCF_TAIL)
        self.assertEqual(_return_adjust(seq, mtlr_index(seq)), 8)

    def test_ordinary_epilogue_is_not_adjusted(self):
        seq = insns(EPILOGUE)
        self.assertIsNone(_return_adjust(seq, mtlr_index(seq)))

    def test_mflr_skip_is_adjusted(self):
        seq = insns(MFLR_SKIP)
        self.assertEqual(_return_adjust(seq, mtlr_index(seq)), 4)

    def test_mtlr_without_return_is_not_adjusted(self):
        seq = insns(UCF_TAIL[:-1] + [0x60000000])  # nop instead of blr
        self.assertIsNone(_return_adjust(seq, mtlr_index(seq)))


if __name__ == "__main__":
    unittest.main()
