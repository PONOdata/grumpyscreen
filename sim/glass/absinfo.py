"""Print the touch panel's axis ranges, so tap coordinates match the glass.

Runs ON the printer. If MT_X or MT_Y max is not 479 or 271, the panel reports
in its own units and glassrun.py taps need scaling first.
"""
import fcntl
import os
import struct


def EVIOCGABS(code):
    return 0x80184540 + code  # _IOR('E', 0x40 + code, struct input_absinfo), 24 bytes


fd = os.open("/dev/input/event0", os.O_RDONLY)
for name, code in (("X", 0), ("Y", 1), ("MT_X", 0x35), ("MT_Y", 0x36), ("MT_SLOT", 0x2F), ("MT_TID", 0x39)):
    v = struct.unpack("6i", fcntl.ioctl(fd, EVIOCGABS(code), bytes(24)))
    print(name, "value=%d min=%d max=%d" % (v[0], v[1], v[2]))
