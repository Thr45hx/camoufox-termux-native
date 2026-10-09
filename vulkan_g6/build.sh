#!/data/data/com.termux/files/usr/bin/bash
# Build libvulkan_g6_glibc.so — the Vulkan ICD shim that bridges Camoufox's
# glibc process to Android's bionic-linked /vendor/lib64/hw/vulkan.powervr.so
# via a mini Android linker loaded into memory.
#
# WHAT THIS IS: a Khronos-loader-compatible ICD that forwards Vulkan calls
# from a glibc process to the Android vendor PowerVR Vulkan HAL, with full
# TPIDR_EL0 swapping and bionic pthread TCB keepalive. Enables GPU-accelerated
# WebGL / WebGPU in a glibc Firefox running natively on Android.
#
# TARGET: Pixel 11 Pro XL / Tensor G6 / A17 (PowerVR C-Series CXTP-48-1536).
# Other SoCs/Android versions probably need adjustments to the vendor HAL id,
# the hwvulkan_device_t layout, and the TPIDR init path. See README for notes.
#
# BUILD DEPS: a glibc gcc (not Termux's bionic clang). Easiest route is
# proot-distro Debian:
#   proot-distro install debian  (first time only)
#   proot-distro login debian --termux-home -- bash /root/<path>/build.sh
#
# OR if you already have a native glibc gcc staged (e.g. via Termux's
# glibc package set), set GCC=/path/to/glibc-gcc and run directly.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/libvulkan_g6_glibc.so"
GCC="${GCC:-gcc}"

echo "[build] $GCC -O2 -shared -fPIC -Wl,-soname,libvulkan_g6_glibc.so -Wl,-z,nodelete"
echo "[build]   vk_powervr_shim.c mini_linker.c -o $OUT -ldl"

"$GCC" -O2 -shared -fPIC \
  -Wl,-soname,libvulkan_g6_glibc.so \
  -Wl,-z,nodelete \
  "$HERE/vk_powervr_shim.c" \
  "$HERE/mini_linker.c" \
  -o "$OUT" \
  -ldl

echo "[build] OK → $OUT ($(wc -c < "$OUT") bytes)"
echo
echo "Next: materialise the ICD JSON pointing at this .so, then set"
echo "  VK_ICD_FILENAMES=<path to g6_glibc_icd.json>"
echo "before launching Camoufox. The camoufox-native.sh launcher does this"
echo "automatically if g6_glibc_icd.json lives next to the camoufox install."
