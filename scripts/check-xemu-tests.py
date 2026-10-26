#!/usr/bin/env python3
"""Judges xemu_tests.exe's output from the windows-2022 runner: the shim and the vendored APU behind it, with no guest.

    check-xemu-tests.py <xemu-tests.txt>

Every test below must print `xemu test <name>: pass`; a test that did not run fails as surely as one that failed.
"""
import re
import sys

TESTS = [
    "flatten_and_dispatch", "read_masked_to_size", "hole_refuses", "unaligned_refuses", "ram_subregion_is_memory",
    "map_change_after_freeze_refuses", "bql_recursive_refuses", "timers_fire_in_order_with_bql", "assert_is_a_refusal",
    "irq_level_with_bql_and_isr_reenters", "muldiv64_wide", "apu_xgscnt_off_then_48k", "nv2a_boot0_and_ram_bar",
    "nv2a_push_buffer_context_switch_semaphore", "nv2a_vblank_rate", "nv2a_ptimer_alarm",
]


def main(path):
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as e:
        sys.exit(f"no xemu-tests.txt: {e}")
    for line in lines:
        print("  " + line)
    missing = [t for t in TESTS if not any(re.match(rf"^xemu test {t}: pass\b", l) for l in lines)]
    for t in missing:
        print(f"NOT PASSED {t}")
    if missing or not any(l == f"xemu tests: {len(TESTS)} pass, 0 fail" for l in lines):
        return 1
    print(f"xemu tests: pass ({len(TESTS)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
