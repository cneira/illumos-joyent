#!/usr/bin/env python3
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# CDDL HEADER END
# Copyright 2026 Edgecast Cloud LLC.

"""Regression guard for the KPM cache update that panicked on PT_GLOBAL."""

from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[4] / "uts/i86pc/vm/hat_i86.c"


class KpmCacheContract(unittest.TestCase):
    def test_updates_only_cache_bits_of_existing_pte(self):
        source = SOURCE.read_text()
        match = re.search(r"\nhat_kpm_set_cache\(.*?\n}\n", source, re.S)
        self.assertIsNotNone(match, "HAT KPM cache update is missing")
        body = match.group()
        self.assertIn("x86pte_update(", body)
        self.assertIn("PT_WRITETHRU", body)
        self.assertIn("PT_NOCACHE", body)
        self.assertIn("PT_PAT_4K", body)
        self.assertNotIn("hat_devload(", body)


if __name__ == "__main__":
    unittest.main()
