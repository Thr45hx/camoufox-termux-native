# camoufox-termux-native

[![platform: Termux/Android](https://img.shields.io/badge/platform-Termux%2FAndroid-3DDC84?logo=android&logoColor=white)](https://termux.dev)
[![arch: aarch64](https://img.shields.io/badge/arch-aarch64-blue)](#)
[![no proot / no root](https://img.shields.io/badge/no%20proot-no%20root-success)](#)
[![license: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

> **Part of a two-repo set:**
> **camoufox-termux-native** (this repo — the browser engine) ·
> [**hermes-camofox-termux**](https://github.com/Thr45hx/hermes-camofox-termux) (the Hermes agent integration)
>
> [![companion: hermes-camofox-termux →](https://img.shields.io/badge/companion-hermes--camofox--termux%20%E2%86%92-8A2BE2)](https://github.com/Thr45hx/hermes-camofox-termux)

Run **[Camoufox](https://github.com/daijro/camoufox)** (a Firefox-class, anti-detect browser)
**natively in Termux on Android — no proot, no root** — and drive it with Playwright's Juggler
protocol.

Camoufox ships a `linux-arm64` **glibc** build. Termux is a **Bionic** environment, so the trick
is the same one the native-Termux CLI agents use: run the glibc binary under a private glibc
loader + a Debian `aarch64` lib tree. That part is "just" library plumbing — but two deeper
issues make an untouched Camoufox **crash on Android** regardless. This repo fixes both.

> Status (2026-07): Camoufox `150.0.2-beta.25` runs headless, renders real pages, and speaks the
> full Juggler automation protocol (Playwright-compatible) on a Pixel 9 Pro XL (Android 14+,
> aarch64), **non-root**. `--version`, `--headless --screenshot`, and a live
> `Browser.enable` + navigate round-trip all pass.
>
> **Status (2026-10)** — ported to **Pixel 11 Pro XL / Tensor G6 / Android 17**. Two more fixes
> on top of the original pair:
> - **DNS silently timing out** → 16-byte in-place patch of `libc.so.6` redirects
>   `/etc/resolv.conf` → `/sdcard/res.conf`. Live `reddit.com` navigation returns the full
>   homepage in **~0.01s** with no Cloudflare challenge. See *Bug 3* below.
> - **GPU-accelerated Vulkan via PowerVR C-Series** → a Khronos-loader-compatible ICD shim
>   (`vulkan_g6/`) bridges the glibc process to Android's `/vendor/lib64/hw/vulkan.powervr.so`
>   through a mini Android linker loaded into memory. **G6-specific** (vendor HAL id, TPIDR
>   init, `hwvulkan_device_t` layout are chip-specific — see the GPU section below). Other
>   SoCs/Android versions will likely need adjustments.

---

## The real bugs (it is **not** "Firefox is unsupported on Android")

### 1. `libxul.so` RELR relocations → SIGSEGV in static init

Upstream `libxul.so` ships its **RELR** (packed relative relocation) dynamic tags with bit 27
set — `0x08000024 / 0x08000023 / 0x08000025` instead of the standard
`DT_RELR / DT_RELRSZ / DT_RELRENT = 0x24 / 0x23 / 0x25`. A mainline glibc loader doesn't
recognise the tag, so it **silently skips ~12,700 relative relocations**. Every global
pointer/vtable keeps its link-time (base-0) value, and the process dies the instant a static
constructor calls through one — right at `calling init: libxul.so` (confirmed with `LD_DEBUG`).

**`fix_relr.py`** (patches `libxul.so` in place):
1. Normalises the three RELR tags to `0x24 / 0x23 / 0x25`.
2. glibc then enforces its guard — *"DT_RELR without GLIBC_ABI_DT_RELR dependency"* — which we
   satisfy by injecting a `GLIBC_ABI_DT_RELR` version dependency (the string already exists in
   `.dynstr`; we repurpose the duplicate `GLIBC_2.28` vernaux in libc's verneed group, so there's
   no structural resize).

```bash
python3 fix_relr.py /path/to/camoufox/libxul.so
```

### 2. Android app seccomp → SIGSYS on `set_robust_list` + `rseq`

Even after RELR, a **non-root** run dies with SIGSYS. glibc's dynamic loader/libc registers a
robust-futex list and an rseq area during **early TLS setup — before any `init_array`**, so an
`LD_PRELOAD` constructor can't observe it. Android's app-process seccomp filter SIGSYS-**traps**
two syscalls that aren't in its allowlist:

| syscall | nr (aarch64) | site |
|---------|--------------|------|
| `set_robust_list` | 99 | `ld.so` TLS init |
| `rseq` | 293 | `ld.so` + `libc.so.6` |

(The `glibc.pthread.rseq=0` tunable does **not** help — `ld.so` self-registers rseq before
tunables are parsed.)

Both are **best-effort** in glibc, so **`patch_glibc_seccomp.py`** neutralises their `svc #0`
sites in your **private** Debian glibc copies: `set_robust_list` → `nop`; `rseq` → `movn w0,#0`
(returns −1 so glibc takes its clean rseq-unavailable fallback).

```bash
# point it at the debian-lib dir that holds ld-linux-aarch64.so.1 + libc.so.6
python3 patch_glibc_seccomp.py /path/to/debian-lib
```

`tools/catch_syscall.c` is the tiny custom **ptracer** used to find those syscalls (Termux
`strace` can't trace this glibc process). It's a reusable primitive for *any*
glibc-binary-on-Android SIGSYS hunt: exec the child, catch the SIGSYS signal-delivery-stop, read
`x8` via `PTRACE_GETREGSET`, map the PC to a module+offset via `/proc/<pid>/maps`.

```bash
clang -O2 -o catch_syscall tools/catch_syscall.c
./catch_syscall ./your-glibc-binary --args
```

### 3. DNS silently times out — `/etc/resolv.conf` doesn't exist on Android

glibc's `getaddrinfo()` hardcodes a read of `/etc/resolv.conf` to find nameservers. On Android,
`/etc` is a read-only symlink to `/system/etc` which has no `resolv.conf` (Android resolves via
Bionic's netd sockets, not a config file). With the file missing, glibc defaults to querying
`127.0.0.1:53`, no local resolver answers, every DNS query hangs with `EAI_AGAIN (-3)`. In
Camoufox you see: **"Problem loading page"** on every URL after a ~30-second timeout, even though
`curl` and `ping` from Termux work fine (they use Bionic, not glibc).

**The fix — a 16-byte in-place binary patch of `libc.so.6`:**

- `"/etc/resolv.conf\0"` is exactly 16 bytes
- `"/sdcard/res.conf\0"` is also exactly 16 bytes
- One-byte-for-one-byte replacement — no ELF offset recalculation, no symbol-version chaos

```bash
python3 patch_libc_resolv.py debian-lib/libc.so.6
# then populate /sdcard/res.conf once:
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\nnameserver 8.8.4.4\n' > /sdcard/res.conf
```

The launcher re-creates `/sdcard/res.conf` on each run if it's missing, so a fresh user doesn't
need to remember the second step.

**Prior art:** [thr45hx/grok-cli-termux-native](https://github.com/thr45hx/grok-cli-termux-native)
uses the same 16-byte trick on Grok's static musl binary, writing to `/sdcard/.grokdns`. This
repo generalizes the pattern to dynamic glibc via `libc.so.6`. See `patch_libc_resolv.py` for
the idempotent, re-runnable version.

**Verification** — `tools/juggler_probe.py` navigating to `https://www.reddit.com/` after the
patch returns the full homepage title ("Reddit - The heart of the internet") in **~10ms**, with
no Cloudflare challenge, no block, 6.7KB body text. The anti-detect fingerprint walks through
reddit's bot detection cleanly.

---

## GPU acceleration via PowerVR Vulkan (G6 / A17 only)

**Scope note:** this section targets **Pixel 11 Pro XL / Tensor G6 / Android 17** specifically.
The vendor HAL id, `hwvulkan_device_t` struct layout, TPIDR_EL0 init path, and `WRAPPED_MAGIC`
dispatch chain are chip-and-firmware specific. Other SoCs/Android versions will likely need
adjustments to `mini_linker.c`'s HAL open path and the vendor HAL struct offsets. The
Khronos-loader side (`vk_powervr_shim.c`) is architecturally reusable but currently coded for
the Vulkan 1.4 / `apiVersion=1.4.317` surface exposed by PowerVR C-Series on this firmware.

Without this shim, Camoufox falls back to software rendering (CPU rasterisation) because:
- Android ships `libvulkan.so` as a Bionic-linked ICD under `/vendor/lib64/hw/`, which the
  Khronos loader in our glibc process can't dlopen directly
- Mesa's upstream PowerVR driver only supports older Rogue chips (AXE/AXM), not G6's C-Series
  Volcanic microarch — no open-source Vulkan path exists for this GPU

**What `vulkan_g6/` provides:**

- **`mini_linker.c`** — loads `/apex/com.android.runtime/bin/linker64` into memory, bootstraps
  Android's bionic namespace machinery (sphal namespace via `android_dlopen_ext` with
  `ANDROID_DLEXT_USE_NAMESPACE`), spawns a bionic pthread to capture a real TCB so TPIDR_EL0
  swaps have a valid errno slot, then dlopens `/vendor/lib64/hw/vulkan.powervr.so` and resolves
  the vendor HAL (`hwvulkan_device_t`, 144 bytes, `GetInstanceProcAddr` at offset `0x88`, HAL
  open id `"vk0"` — all empirically mapped).
- **`vk_powervr_shim.c`** — a Vulkan ICD exposing `vk_icdNegotiateLoaderICDInterfaceVersion` (v5)
  and `vk_icdGetInstanceProcAddr`. Wraps `VkInstance` / `VkPhysicalDevice` / `VkDevice` with
  `{loader_dispatch@0, real_handle@8, WRAPPED_MAGIC@16}`. Dynamic ARM64 trampolines generated
  per entry point save caller TPIDR, load bionic TPIDR, check `[x0, #16] == 0x57524150` before
  unwrapping (so vendor-created dispatchable handles like `VkQueue`/`VkCommandBuffer` pass
  through verbatim), call the real vendor function, restore TPIDR, return. Compiled with
  `-Wl,-z,nodelete` so dlclose can't unmap the shim while the bionic TCB keepalive thread is
  still spinning.
- **`g6_glibc_icd.json.template`** — Khronos ICD manifest template. Point
  `VK_ICD_FILENAMES` at the materialised copy (see `camoufox-native.sh`).
- **`build.sh`** — builds `libvulkan_g6_glibc.so` from the two C sources.

**Build + install (one-time):**

```bash
cd vulkan_g6
# easiest: via proot-distro Debian (has native glibc gcc)
proot-distro install debian                     # first time only
proot-distro login debian --termux-home -- bash /root/<path>/vulkan_g6/build.sh
# or with a staged native glibc gcc:
GCC=/path/to/glibc-gcc ./build.sh

# materialise ICD JSON pointing at the built .so
sed 's|@CAMOUFOX_HOME@|'"$HOME/browsers/camoufox"'|' \
    g6_glibc_icd.json.template > g6_glibc_icd.json
```

The `camoufox-native.sh` launcher auto-exports `VK_ICD_FILENAMES` to the shipped ICD JSON
location, so once `libvulkan_g6_glibc.so` + `g6_glibc_icd.json` exist, Camoufox picks the shim
up on next launch with no further config.

**Verified** — `tools/juggler_probe.py` + `vulkaninfo --summary` through this shim prints
`PowerVR C-Series CXTP-48-1536 MC1, apiVersion=1.4.317` and full device enumeration passes.
Command buffer recording, queue submit, and `vkQueueWaitIdle` all succeed end-to-end.

---

## Setup

1. **Loader + libs.** Unzip a Camoufox `lin.arm64` release, and assemble a Debian `bookworm`
   `aarch64` lib tree (`debian-lib/`) containing `ld-linux-aarch64.so.1`, `libc.so.6`, GTK3, X11,
   etc. (extract the `.deb`s; keep **one** libc/loader — don't mix termux-glibc with Debian). The
   `camoufox-bin` interpreter should point at your `debian-lib/ld-linux-aarch64.so.1`.
2. `python3 fix_relr.py libxul.so`
3. `python3 patch_glibc_seccomp.py debian-lib`
4. `python3 patch_libc_resolv.py debian-lib/libc.so.6` *(DNS fix)*
5. **G6 / A17 only, optional for GPU-accelerated WebGL/WebGPU:**
   ```bash
   cd vulkan_g6 && bash build.sh
   sed 's|@CAMOUFOX_HOME@|'"$HOME/browsers/camoufox"'|' \
       g6_glibc_icd.json.template > ../g6_glibc_icd.json
   ```
6. Launch with `camoufox-native.sh` (edit the paths at the top). Key env: unset `LD_PRELOAD`
   (Termux's `libtermux-exec` demands versioned Bionic `libc.so`), set
   `LD_LIBRARY_PATH=run-bin:debian-lib`, run the binary directly so `/proc/self/exe` is
   `camoufox-bin` (Firefox finds `libxul`/`omni.ja`/`dependentlibs.list` next to it). The
   launcher also auto-populates `/sdcard/res.conf` and exports `VK_ICD_FILENAMES` for the
   shipped ICD JSON if present.

```bash
./camoufox-native.sh --version
./camoufox-native.sh --headless --screenshot ~/shot.png https://example.com
```

## Automation (Playwright / Juggler)

Use **`--juggler-pipe`** (this build has no TCP `-juggler <port>`). Transport = Playwright pipe:
fd 3 = browser-in, fd 4 = browser-out, JSON `{id, method, params}` messages **`\0`-delimited**;
first command is `Browser.enable {attachToDefaultContext:true}`. `tools/juggler_probe.py` is a
minimal, dependency-free client that proves a full round-trip.

To drive it from Playwright / the `camoufox` package, point `executablePath` at your patched
binary and launch with the env above. On Termux, `playwright-core` rejects `process.platform ==
"android"` — force it to `"linux"` via a Node preload (see the companion repo
**hermes-camofox-termux** for the full Node-side wiring).

---

## What this repo does **not** include

The Camoufox binary, `libxul.so`, `omni.ja`, and the Debian lib tree are **not** committed (they're
large and belong to upstream). Grab a [Camoufox release](https://github.com/daijro/camoufox/releases)
and run the scripts on it.

## Credits

Camoufox by [daijro](https://github.com/daijro/camoufox). The RELR + seccomp diagnosis and the
Termux native-glibc approach were worked out on-device. Contributed as part of the `*-termux-native`
family of ports.

## License

MIT
