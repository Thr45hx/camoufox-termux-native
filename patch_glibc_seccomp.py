#!/usr/bin/env python3
"""
Patch the private Debian glibc (ld-linux-aarch64.so.1 + libc.so.6 in debian-lib/)
so Camoufox runs NON-ROOT under Android's app seccomp filter.

glibc's dynamic loader + libc startup register a robust-futex list and an rseq
area during early TLS setup. Android's app-process seccomp policy SIGSYS-TRAPs
those two syscalls (they aren't in its allowlist), killing the process before any
constructor runs — so it can't be fixed from an LD_PRELOAD.

Both are best-effort in glibc, so we neutralise the two `svc #0` sites:
  * set_robust_list (x8=99)  -> NOP the svc  (glibc ignores its result)
  * rseq            (x8=293) -> replace svc with `movn w0,#0` (w0=-1), so glibc
                                sees an error return and takes its clean
                                rseq-unavailable fallback (no kernel rseq area).

Idempotent: matches `mov x8,#<nr>` (64-bit movz) then the next `svc #0` within a
few instructions. Safe to re-run. Only touches debian-lib/ (camoufox-private);
does not affect $PREFIX/glibc used by the CLI agents.

Usage: python3 patch_glibc_seccomp.py [debian-lib-dir]
"""
import struct, sys, os

SVC0        = 0xd4000001
NOP         = 0xd503201f
MOVN_W0_0   = 0x12800000          # movn w0,#0  => w0 = -1
MOVZ_X8_99  = 0xd2800c68          # mov x8,#99   (set_robust_list)
MOVZ_X8_293 = 0xd28024a8          # mov x8,#293  (rseq)

def patch_file(path, targets):
    b = bytearray(open(path, 'rb').read()); n = len(b)
    done = {}
    for i in range(0, n-4, 4):
        w = struct.unpack_from('<I', b, i)[0]
        if w in targets:
            repl = targets[w]
            for j in range(i+4, min(i+4+32, n-3), 4):
                if struct.unpack_from('<I', b, j)[0] == SVC0:
                    struct.pack_into('<I', b, j, repl)
                    done.setdefault(w, []).append(hex(j))
                    break
    if done:
        open(path, 'wb').write(b)
    return done

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(__file__)), 'debian-lib')
    ld  = os.path.join(d, 'ld-linux-aarch64.so.1')
    libc = os.path.join(d, 'libc.so.6')
    tgt = {MOVZ_X8_99: NOP, MOVZ_X8_293: MOVN_W0_0}
    for f in (ld, libc):
        # one-time pristine backup
        bak = f + '.pre_robust'
        if not os.path.exists(bak):
            import shutil; shutil.copy2(f, bak)
        res = patch_file(f, tgt)
        names = {MOVZ_X8_99: 'set_robust_list', MOVZ_X8_293: 'rseq'}
        pretty = {names[k]: v for k, v in res.items()}
        print(f"{os.path.basename(f)}: {pretty or 'no sites (already patched?)'}")

if __name__ == '__main__':
    main()
