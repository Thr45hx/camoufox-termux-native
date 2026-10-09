#!/data/data/com.termux/files/usr/bin/bash
# Camoufox — native Termux, NON-ROOT, no proot.
# linux-arm64 glibc build + private Debian bookworm aarch64 lib tree.
#
# WORKING as of 2026-07-09 after two ELF fixes:
#   1. fix_relr.py            → libxul.so RELR + GLIBC_ABI_DT_RELR
#   2. patch_glibc_seccomp.py → debian-lib ld.so/libc set_robust_list + rseq
#                               (Android app seccomp SIGSYS traps)
#
# Proven non-root: --version, --headless --screenshot; Juggler loads.
#
# Usage:
#   ./camoufox-native.sh --version
#   ./camoufox-native.sh --headless --screenshot ~/shot.png https://example.com
#   ./camoufox-native.sh --headless -juggler 0
set -euo pipefail

CF="/data/data/com.termux/files/home/browsers/camoufox"
RUN="$CF/run-bin"
LIB="$CF/debian-lib"
BIN="$RUN/camoufox-bin"
LD="$LIB/ld-linux-aarch64.so.1"

export HOME="${CAMOUFOX_PROFILE:-$CF/home}"
export TMPDIR="${TMPDIR:-$CF/tmp}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$CF/tmp}"
export FONTCONFIG_PATH="$CF/fontconfig/linux"
export MOZ_DISABLE_CONTENT_SANDBOX=1
export MOZ_DISABLE_GMP_SANDBOX=1
export MOZ_ENABLE_WAYLAND=0
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-$CF/g6_glibc_icd.json}"
# Optional: avoid accidental Debian libc poisoning of parent shells is N/A —
# we only set LD_LIBRARY_PATH for the exec'd process.
mkdir -p "$HOME" "$TMPDIR" "$XDG_RUNTIME_DIR"
if [ ! -s /sdcard/res.conf ]; then
  printf "nameserver 1.1.1.1\nnameserver 8.8.8.8\nnameserver 8.8.4.4\n" > /sdcard/res.conf 2>/dev/null || true
fi
cd "$RUN"

# Critical on Termux: drop Bionic LD_PRELOAD (libtermux-exec-*) before
# entering Debian glibc — it requires versioned Bionic libc.so "LIBC".
# Run binary directly so /proc/self/exe is camoufox-bin (XPCOM/dependentlibs).
exec env -u LD_PRELOAD -u LD_PRELOAD_64 -u LD_PRELOAD_32 \
  LD_LIBRARY_PATH="$RUN:$LIB" \
  "$BIN" "$@"
