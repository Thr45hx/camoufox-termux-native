#!/usr/bin/env python3
"""
Fix Camoufox (Firefox-class) libxul.so so the Debian/termux glibc loader will
run it natively on Termux/Android.

Root cause of the old SIGSEGV-in-libxul-init:
  Upstream libxul.so ships its RELR (packed relative) relocation tags with
  bit 27 (0x08000000) OR'd into the DT tag number:
       0x08000024  (should be DT_RELR    = 0x24)
       0x08000023  (should be DT_RELRSZ  = 0x23)
       0x08000025  (should be DT_RELRENT = 0x25)
  A mainline glibc loader doesn't recognise 0x08000024, so it SILENTLY SKIPS
  ~12,700 relative relocations. libxul's global pointers keep their link-time
  (base-0) values and the process dies the instant a static constructor calls
  through one ("calling init: libxul.so").

Two edits make glibc apply RELR correctly:
  1. Normalise the three DT tags 0x0800002{3,4,5} -> 0x2{3,4,5}.
  2. glibc then refuses RELR unless the object declares a versioned dependency
     "GLIBC_ABI_DT_RELR" (its guard against the exact silent-skip above).
     The string already exists in .dynstr, and libc.so.6's verneed group has a
     duplicate GLIBC_2.28 entry we repurpose in place (no structural resize).

Idempotent-ish: run once on a pristine libxul.so (extract from the release zip).
Usage: python3 fix_relr.py /path/to/libxul.so
"""
import struct, sys

def elfhash(name):
    h = 0
    for c in name.encode():
        h = (h << 4) + c
        g = h & 0xf0000000
        if g:
            h ^= g >> 24
        h &= ~g
    return h & 0xffffffff

def patch(path):
    f = open(path, 'r+b')
    hdr = f.read(64)
    e_shoff = struct.unpack_from('<Q', hdr, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', hdr, 0x3a)[0]
    e_shnum = struct.unpack_from('<H', hdr, 0x3c)[0]
    e_shstrndx = struct.unpack_from('<H', hdr, 0x3e)[0]
    e_phoff = struct.unpack_from('<Q', hdr, 0x20)[0]
    e_phentsize = struct.unpack_from('<H', hdr, 0x36)[0]
    e_phnum = struct.unpack_from('<H', hdr, 0x38)[0]

    # --- edit 1: normalise RELR dynamic tags in PT_DYNAMIC ---
    f.seek(e_phoff); ph = f.read(e_phentsize * e_phnum)
    dyn_off = dyn_sz = None
    for i in range(e_phnum):
        p = ph[i*e_phentsize:(i+1)*e_phentsize]
        if struct.unpack_from('<I', p, 0)[0] == 2:  # PT_DYNAMIC
            dyn_off = struct.unpack_from('<Q', p, 0x08)[0]
            dyn_sz = struct.unpack_from('<Q', p, 0x20)[0]
    fixmap = {0x08000024: 0x24, 0x08000023: 0x23, 0x08000025: 0x25}
    f.seek(dyn_off); d = bytearray(f.read(dyn_sz))
    n1 = 0
    for i in range(0, len(d), 16):
        tag = struct.unpack_from('<Q', d, i)[0]
        if tag in fixmap:
            struct.pack_into('<Q', d, i, fixmap[tag]); n1 += 1
        if tag == 0:
            break
    f.seek(dyn_off); f.write(d)

    # section helpers
    f.seek(e_shoff); sh = f.read(e_shentsize * e_shnum)
    def shent(i):
        b = sh[i*e_shentsize:(i+1)*e_shentsize]
        return dict(name=struct.unpack_from('<I', b, 0)[0],
                    off=struct.unpack_from('<Q', b, 24)[0],
                    size=struct.unpack_from('<Q', b, 32)[0])
    sst = shent(e_shstrndx); f.seek(sst['off']); shstr = f.read(sst['size'])
    def snm(o): return shstr[o:shstr.index(b'\0', o)].decode()
    secs = {snm(shent(i)['name']): shent(i) for i in range(e_shnum)}

    ds = secs['.dynstr']; f.seek(ds['off']); dstr = f.read(ds['size'])
    def cstr(o): return dstr[o:dstr.index(b'\0', o)].decode()
    sidx = dstr.find(b'GLIBC_ABI_DT_RELR\x00')
    if sidx < 0:
        raise SystemExit("GLIBC_ABI_DT_RELR string not in .dynstr")
    relr_hash = elfhash("GLIBC_ABI_DT_RELR")

    # --- edit 2: repurpose duplicate GLIBC_2.28 vernaux in libc.so.6 group ---
    vr = secs['.gnu.version_r']; base = vr['off']
    f.seek(base); vd = bytearray(f.read(vr['size']))
    off = 0; n2 = 0
    while True:
        vn_ver, vn_cnt, vn_file, vn_aux, vn_next = struct.unpack_from('<HHIII', vd, off)
        if cstr(vn_file) == 'libc.so.6':
            aoff = off + vn_aux; seen = 0
            for _ in range(vn_cnt):
                a_hash, a_flags, a_other, a_name, a_next = struct.unpack_from('<IHHII', vd, aoff)
                if cstr(a_name) == 'GLIBC_2.28':
                    seen += 1
                    if seen == 2:  # the redundant duplicate
                        struct.pack_into('<IHHII', vd, aoff, relr_hash, 0, 93, sidx, a_next)
                        n2 += 1
                if a_next == 0:
                    break
                aoff += a_next
        if vn_next == 0:
            break
        off += vn_next
    f.seek(base); f.write(vd); f.close()
    print(f"patched {n1} RELR tags, {n2} verneed entry ({path})")
    if n1 != 3 or n2 != 1:
        print("WARNING: expected 3 tags + 1 verneed; file may already be patched or differ")

if __name__ == '__main__':
    patch(sys.argv[1] if len(sys.argv) > 1 else 'libxul.so')
