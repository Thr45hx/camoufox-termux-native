#!/usr/bin/env python3
"""Patch Debian bookworm aarch64 libc.so.6 to redirect DNS lookups from
/etc/resolv.conf (which does not exist on Android) to /sdcard/res.conf
(which does, and is writable without root).

The two paths are both exactly 16 bytes (including trailing NUL), so this is
a byte-for-byte in-place replacement — no ELF offset recalculation, no symbol
version table manipulation, no relocation chaos.

Prior art: thr45hx/grok-cli-termux-native uses the same 16-byte trick on
Grok's static musl binary, writing to /sdcard/.grokdns. This script generalizes
the pattern to the dynamic glibc world used by Camoufox and other glibc-tool
ports in Termux.

Usage:
    python3 patch_libc_resolv.py debian-lib/libc.so.6

Then populate /sdcard/res.conf (one-time, outside this script):
    cat > /sdcard/res.conf <<EOF
    nameserver 1.1.1.1
    nameserver 8.8.8.8
    nameserver 8.8.4.4
    EOF

The patched libc.so.6 is idempotent — re-running the script on an already-
patched file reports "no change needed" and makes no edits.
"""
import sys
from pathlib import Path

OLD = b"/etc/resolv.conf\0"
NEW = b"/sdcard/res.conf\0"
assert len(OLD) == len(NEW) == 16, "both paths must be exactly 16 bytes"


def patch(path: Path) -> None:
    data = bytearray(path.read_bytes())
    n_old = data.count(OLD)
    n_new = data.count(NEW)

    if n_new > 0 and n_old == 0:
        print(f"{path}: already patched (found {n_new}× NEW, 0× OLD); no change needed")
        return
    if n_old == 0:
        print(f"{path}: no '/etc/resolv.conf' string found; nothing to patch")
        sys.exit(2)
    if n_old > 1:
        print(f"{path}: WARNING — found {n_old} occurrences of '/etc/resolv.conf'; "
              "patching all. If any instance is not a config path, the patch may be unsafe.")

    i = 0
    patched = 0
    while True:
        j = data.find(OLD, i)
        if j < 0:
            break
        data[j : j + len(NEW)] = NEW
        patched += 1
        i = j + len(NEW)

    path.write_bytes(bytes(data))
    print(f"{path}: patched {patched} occurrence(s) of '/etc/resolv.conf' → '/sdcard/res.conf'")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path-to-libc.so.6>", file=sys.stderr)
        sys.exit(1)
    patch(Path(sys.argv[1]))
