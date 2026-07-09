# camoufox-termux-native

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

---

## The two real bugs (it is **not** "Firefox is unsupported on Android")

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

---

## Setup

1. **Loader + libs.** Unzip a Camoufox `lin.arm64` release, and assemble a Debian `bookworm`
   `aarch64` lib tree (`debian-lib/`) containing `ld-linux-aarch64.so.1`, `libc.so.6`, GTK3, X11,
   etc. (extract the `.deb`s; keep **one** libc/loader — don't mix termux-glibc with Debian). The
   `camoufox-bin` interpreter should point at your `debian-lib/ld-linux-aarch64.so.1`.
2. `python3 fix_relr.py libxul.so`
3. `python3 patch_glibc_seccomp.py debian-lib`
4. Launch with `camoufox-native.sh` (edit the paths at the top). Key env: unset `LD_PRELOAD`
   (Termux's `libtermux-exec` demands versioned Bionic `libc.so`), set
   `LD_LIBRARY_PATH=run-bin:debian-lib`, run the binary directly so `/proc/self/exe` is
   `camoufox-bin` (Firefox finds `libxul`/`omni.ja`/`dependentlibs.list` next to it).

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
