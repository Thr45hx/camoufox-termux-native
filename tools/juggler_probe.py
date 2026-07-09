#!/usr/bin/env python3
"""Minimal Juggler pipe-transport client: launch camoufox --juggler-pipe with
fd3=browser-in / fd4=browser-out, send one protocol command (\\0-framed), read
the reply. Proves the Playwright/Camoufox automation transport works end-to-end
without the full Playwright python stack."""
import os, sys, json, select, subprocess, time

CF = "/data/data/com.termux/files/home/browsers/camoufox"
RUN = f"{CF}/run-bin"

cmd_r, cmd_w = os.pipe()      # parent writes cmd_w -> child reads as fd 3
resp_r, resp_w = os.pipe()    # child writes fd 4 -> parent reads resp_r

def child_setup():
    os.dup2(cmd_r, 3)
    os.dup2(resp_w, 4)

env = {
    "HOME": f"{CF}/home", "TMPDIR": f"{CF}/tmp", "XDG_RUNTIME_DIR": f"{CF}/tmp",
    "FONTCONFIG_PATH": f"{CF}/fontconfig/linux",
    "MOZ_DISABLE_CONTENT_SANDBOX": "1", "MOZ_DISABLE_GMP_SANDBOX": "1",
    "MOZ_ENABLE_WAYLAND": "0", "LD_LIBRARY_PATH": f"{RUN}:{CF}/debian-lib",
    "PATH": "/data/data/com.termux/files/usr/bin",
}
p = subprocess.Popen(
    [f"{RUN}/camoufox-bin", "--headless", "--juggler-pipe", "about:blank"],
    cwd=RUN, env=env, pass_fds=(3, 4), preexec_fn=child_setup,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
)
os.close(cmd_r); os.close(resp_w)

def send(obj):
    os.write(cmd_w, (json.dumps(obj) + "\0").encode())

def read_msg(timeout=20):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([resp_r], [], [], end - time.time())
        if resp_r in r:
            chunk = os.read(resp_r, 65536)
            if not chunk:
                break
            buf += chunk
            while b"\0" in buf:
                msg, buf = buf.split(b"\0", 1)
                if msg.strip():
                    yield json.loads(msg.decode())
    return

got = []
try:
    # Browser.enable is the handshake Playwright sends first.
    send({"id": 1, "method": "Browser.enable", "params": {"attachToDefaultContext": True}})
    for m in read_msg(20):
        got.append(m)
        # stop once we see the reply to id 1 (result or error both prove round-trip)
        if m.get("id") == 1:
            break
finally:
    for m in got:
        print("RECV:", json.dumps(m)[:300])
    # success = our command got its matching id reply with no error, AND the
    # browser streamed page lifecycle events (real attach, not just an echo).
    reply_ok = any(m.get("id") == 1 and "error" not in m for m in got)
    events_ok = any("method" in m and m["method"].startswith(("Page.", "Runtime.")) for m in got)
    ok = reply_ok and events_ok
    print(f"ROUND-TRIP OK: {ok}  (reply={reply_ok}, page/runtime events={events_ok})")
    try: p.terminate()
    except Exception: pass
    sys.exit(0 if ok else 1)
