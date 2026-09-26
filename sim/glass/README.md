# Glass harness

Checks a UI change on the printer's own panel, frame by frame. The desktop
simulator in `sim/` cannot reproduce layer order under real touch, a
Moonraker reconnect, or a Klipper restart. This can. It was used to check #67.

| File | Runs on | Does |
|---|---|---|
| `glassrun.py` | printer | reads `/dev/fb0` about every 0.2 s, optionally starts a test binary, restarts Klipper or triggers an E-STOP, and injects taps with a hold |
| `absinfo.py` | printer | prints the touch axis ranges so tap coordinates match the glass |
| `sheet.py` | desktop | turns a capture into a labelled contact sheet (needs Pillow) |

`../verify_glass.sh` is the older one-shot version: single taps, one frame
per capture, no hold.

## Run one

```sh
P=root@192.168.50.112
ssh $P 'cat > /tmp/glassrun.py' < glassrun.py
# optional: put a cross-built binary beside the live one, then start it
ssh $P 'cat > /tmp/gs-new && chmod +x /tmp/gs-new' < build/bin/guppyscreen
ssh $P 'python3 /tmp/glassrun.py boot 20 start'
ssh $P 'tar -C /tmp/glass -cf - boot' > boot.tar && tar -xf boot.tar
python sheet.py boot boot.png
```

Afterwards put the stock UI back with `/etc/init.d/grumpyscreen restart`.

## Make each test tell two cases apart

A tap test that passes when nothing happens also passes when the injector
is broken. Pair each one with a case that must fire. For the screen saver
in #67:

- saver up, `tap:X,Y@3~1.5` on the E-STOP spot: the cockpit wakes and no
  confirm opens
- screen awake, the same hold: "Emergency stop the printer?" opens

To get the saver up quickly, lower `display_sleep_sec` in
`/etc/klipper/config/grumpyscreen.cfg`. Set it back to 600 when done.

## Device notes

- Moonraker is on plain port 80 on this printer, not 7125.
- klippy.log is at `http://<printer>/server/files/logs/klippy.log`.
- BusyBox `head` needs `-n N`.
- A running binary cannot be overwritten in place. Upload beside it and
  `mv -f` it over.
- `estop` sends M112. Use it only with the printer idle and someone watching it.
