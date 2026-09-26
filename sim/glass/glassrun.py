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
action = sys.argv[3] if len(sys.argv) > 3 else "none"
taps = []
for a in sys.argv[4:]:
    xy, t = a[4:].split("@")
    t, _, hold = t.partition("~")
    x, y = xy.split(",")
    taps.append([float(t), int(x), int(y), False, float(hold or 0.12)])

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


def tap(x, y, hold):
    fd = os.open("/dev/input/event0", os.O_WRONLY)

    def ev(t, c, v):
        os.write(fd, EV.pack(0, 0, t, c, v))

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
    ev(3, 0x39, -1)
    ev(1, 0x14A, 0)
    ev(0, 0, 0)
    os.close(fd)


if action == "start":
    subprocess.call(["/etc/init.d/grumpyscreen", "stop"])
    time.sleep(0.5)
    # Fixed argv, BIN is the operator's own test binary.
    subprocess.call(["start-stop-daemon", "-S", "-b", "-m", "-p", PID, "-x", BIN])
elif action == "krestart":
    print("restart", post("/printer/restart"))
elif action == "estop":
    print("estop", post("/printer/emergency_stop"))

t0 = time.time()
lines = []
i = 0
while time.time() - t0 < secs:
    now = time.time() - t0
    for tp in taps:
        if not tp[3] and now >= tp[0]:
            threading.Thread(target=tap, args=(tp[1], tp[2], tp[4])).start()
            tp[3] = True
            lines.append("TAP %d,%d at %d ms hold %d ms" % (tp[1], tp[2], int(now * 1000), int(tp[4] * 1000)))
    with open("/dev/fb0", "rb") as fb:
        raw = fb.read(FB_BYTES)
    ms = int((time.time() - t0) * 1000)
    fn = "%03d_%05d.z" % (i, ms)
    with open(os.path.join(out, fn), "wb") as o:
        o.write(zlib.compress(raw, 1))
    lines.append("%s %s" % (fn, kstate()))
    i += 1
    time.sleep(0.2)
with open(os.path.join(out, "index.txt"), "w") as f:
    f.write("\n".join(lines) + "\n")
print("frames", i, "final", kstate())
