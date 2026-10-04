"""On-device glass harness: framebuffer burst plus touch injection.

Runs ON the printer (its own python3). Copy it over with
`ssh root@<printer> 'cat > /tmp/glassrun.py' < glassrun.py`.

usage: glassrun.py NAME SECONDS [ACTION] [tap:X,Y@T[~H] ...]
  ACTION  start     stop the UI service and start $GLASS_BIN (default /tmp/gs-new)
                    under the same pidfile
          krestart  POST /printer/restart
          estop     POST /printer/emergency_stop (only with the printer idle)
          none      capture only (the default)
  tap:X,Y@T[~H]     touch screen X,Y at T seconds after the action and hold it
                    H seconds (default 0.12). Each tap runs on its own thread,
                    so frames keep coming while it is held.
                    Taps must not overlap: they all use MT slot 0.

Frames land in /tmp/glass/NAME as zlib'd BGRA, 480x272, about every 0.2 s,
plus index.txt: one line per frame (file name, then Klipper's state or the
error text) and one line per tap. Decode them on the desktop with sheet.py.
"""
import json
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import zlib

BIN = os.environ.get("GLASS_BIN", "/tmp/gs-new")
PID = "/var/run/gui.pid"
MOONRAKER = "http://127.0.0.1"  # Moonraker on this box listens on port 80
FB_BYTES = 480 * 272 * 4

name, secs = sys.argv[1], float(sys.argv[2])
rest = [a for a in sys.argv[3:] if not a.startswith("tap:")]
action = rest[0] if rest else "none"
if len(rest) > 1 or action not in ("start", "krestart", "estop", "none"):
    sys.exit(
        "glassrun.py: bad ACTION %r. Use start, krestart, estop or none.\n%s" % (action, __doc__)
    )
taps = []
for a in (a for a in sys.argv[3:] if a.startswith("tap:")):
    xy, t = a[4:].split("@")
    t, _, hold = t.partition("~")
    x, y = xy.split(",")
    taps.append([float(t), int(x), int(y), False, float(hold or 0.12)])
# One contact only: every tap drives MT slot 0, so overlapping windows would
# stomp each other's down/up events and read as a single bogus touch. This
# refuses a schedule that asks for overlap. A tap the loop launches late can
# still reach the next one's start, and TAP_LOCK in tap() covers that case.
for a, b in zip(sorted(taps), sorted(taps)[1:]):
    if b[0] < a[0] + a[4]:
        sys.exit("taps %d,%d@%g~%g and %d,%d@%g overlap; they share MT slot 0. Space them out."
                 % (a[1], a[2], a[0], a[4], b[1], b[2], b[0]))

out = "/tmp/glass/" + name
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))


def post(path):
    req = urllib.request.Request(MOONRAKER + path, data=b"", method="POST")
    # The printer's own Moonraker on localhost, plain http by design.
    # nosemgrep: dynamic-urllib-use-detected, insecure-urlopen
    return urllib.request.urlopen(req, timeout=5).read()[:120]


def kstate():
    try:
        # nosemgrep: dynamic-urllib-use-detected, insecure-urlopen
        r = json.load(urllib.request.urlopen(MOONRAKER + "/printer/info", timeout=0.6))
        return r["result"]["state"]
    except Exception as e:  # noqa: BLE001 - the error text IS the reading
        return "ERR:" + str(e)[:40].replace(" ", "_")


EV = struct.Struct("IIHHi")  # armv7 input_event: 32-bit sec, usec, type, code, value


# Taps run on their own threads and share MT slot 0, so a tap waits for the
# one before it to release instead of writing over a held contact.
TAP_LOCK = threading.Lock()


def tap(x, y, hold):
    with TAP_LOCK:
        fd = os.open("/dev/input/event0", os.O_WRONLY)

        def ev(t, c, v):
            os.write(fd, EV.pack(0, 0, t, c, v))

        try:
            # MT_SLOT 0, TRACKING_ID 0, MT_X, MT_Y, BTN_TOUCH down, ABS_X/Y, SYN
            ev(3, 0x2F, 0)
            ev(3, 0x39, 0)
            ev(3, 0x35, x)
            ev(3, 0x36, y)
            ev(1, 0x14A, 1)
            ev(3, 0, x)
            ev(3, 1, y)
            ev(0, 0, 0)
            time.sleep(hold)
        finally:
            # Release even when a write above failed, so the panel is not left
            # holding a touch, then close the fd whatever the release did.
            try:
                ev(3, 0x39, -1)
                ev(1, 0x14A, 0)
                ev(0, 0, 0)
            finally:
                os.close(fd)


if action == "start":
    subprocess.call(["/etc/init.d/grumpyscreen", "stop"])
    time.sleep(0.5)
    # Fixed argv, BIN is the operator's own test binary.
    if subprocess.call(["start-stop-daemon", "-S", "-b", "-m", "-p", PID, "-x", BIN]) != 0:
        sys.exit("could not start %s. Upload the test binary first, then rerun. "
                 "Put the stock UI back with /etc/init.d/grumpyscreen restart" % BIN)
elif action == "krestart":
    print("restart", post("/printer/restart"))
elif action == "estop":
    print("estop", post("/printer/emergency_stop"))

# Line-buffered and written as the capture runs, so a run cut short still
# leaves an index for the frames it wrote.
index = open(os.path.join(out, "index.txt"), "w", buffering=1)


def note(line):
    index.write(line + "\n")


t0 = time.time()
i = 0
while time.time() - t0 < secs:
    now = time.time() - t0
    for tp in taps:
        if not tp[3] and now >= tp[0]:
            threading.Thread(target=tap, args=(tp[1], tp[2], tp[4])).start()
            tp[3] = True
            note("TAP %d,%d at %d ms hold %d ms" % (tp[1], tp[2], int(now * 1000), int(tp[4] * 1000)))
    with open("/dev/fb0", "rb") as fb:
        raw = fb.read(FB_BYTES)
    if len(raw) != FB_BYTES:
        note("SHORT READ %d of %d bytes" % (len(raw), FB_BYTES))
    ms = int((time.time() - t0) * 1000)
    fn = "%03d_%05d.z" % (i, ms)
    with open(os.path.join(out, fn), "wb") as o:
        o.write(zlib.compress(raw, 1))
    note("%s %s" % (fn, kstate()))
    i += 1
    time.sleep(0.2)
missed = ["%d,%d@%g" % (tp[1], tp[2], tp[0]) for tp in taps if not tp[3]]
if missed:
    # A tap at or past SECONDS never fires: say so instead of passing quietly.
    msg = "MISSED taps %s: the %g s capture ended first. Raise SECONDS and rerun." % (
        ", ".join(missed),
        secs,
    )
    note(msg)
    print(msg)
index.close()
print("frames", i, "final", kstate())
if action == "start":
    print("stock UI is still stopped. Restore it with /etc/init.d/grumpyscreen restart")
