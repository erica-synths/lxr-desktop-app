#!/usr/bin/env python3
"""Send MIDI to an LXR for the device tests in this directory.

Written for a Roland UM-ONE (USB MIDI cable to the LXR's DIN MIDI in), but it
works with any MIDI output. Channels are 1-16, as on the LXR's display.

Setup:
    python3 -m venv venv && venv/bin/pip install -r requirements.txt

Examples:
    venv/bin/python lxr_midi.py ports
    venv/bin/python lxr_midi.py cc 0 5                  # bank select: kit 5
    venv/bin/python lxr_midi.py -c 3 cc 0 64            # ...on channel 3
    venv/bin/python lxr_midi.py bank 0 1 63 64 100 127  # step through values
    venv/bin/python lxr_midi.py note 48 --velocity 100
    venv/bin/python lxr_midi.py panic                   # all notes off
"""
import argparse
import sys
import time

import mido

DEFAULT_PORT = "UM-ONE"   # substring of the port name


def find_port(match):
    names = mido.get_output_names()
    hits = [n for n in names if match.lower() in n.lower()]
    if not hits:
        sys.exit("No MIDI output matching %r. Available outputs: %s\n"
                 "Is the UM-ONE plugged in (and its driver installed)? "
                 "Use --port to pick another." % (match, names or "none"))
    return hits[0]


def check(value, lo, hi, what):
    if not lo <= value <= hi:
        sys.exit("%s must be %d-%d, got %d" % (what, lo, hi, value))
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-p", "--port", default=DEFAULT_PORT,
                   help="substring of the MIDI output name (default: %(default)s)")
    p.add_argument("-c", "--channel", type=int, default=1,
                   help="MIDI channel 1-16 (default: %(default)s)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("ports", help="list MIDI outputs")

    s = sub.add_parser("cc", help="send one control change")
    s.add_argument("number", type=int)
    s.add_argument("value", type=int)

    s = sub.add_parser("bank", help="send CC 0 (bank select) values one by one, "
                       "pausing so you can note what the LXR does")
    s.add_argument("values", type=int, nargs="+")
    s.add_argument("--auto", type=float, metavar="SECONDS",
                   help="wait this long between values instead of pressing Enter")

    s = sub.add_parser("note", help="send a note on, then off")
    s.add_argument("note", type=int)
    s.add_argument("--velocity", type=int, default=100)
    s.add_argument("--length", type=float, default=0.3, help="seconds")

    sub.add_parser("panic", help="all notes off on the channel")

    a = p.parse_args()
    if a.cmd == "ports":
        for n in mido.get_output_names():
            print(n)
        return

    ch = check(a.channel, 1, 16, "channel") - 1
    port = find_port(a.port)
    with mido.open_output(port) as out:
        print("port: %s, channel %d" % (port, ch + 1))

        def cc(n, v):
            check(n, 0, 127, "CC number"); check(v, 0, 127, "CC value")
            out.send(mido.Message("control_change", channel=ch, control=n, value=v))
            print("sent CC %d = %d" % (n, v))

        if a.cmd == "cc":
            cc(a.number, a.value)
        elif a.cmd == "bank":
            for v in a.values:
                cc(0, v)
                if a.auto is not None:
                    time.sleep(a.auto)
                else:
                    input("  note what the LXR does, then press Enter... ")
        elif a.cmd == "note":
            check(a.note, 0, 127, "note"); check(a.velocity, 1, 127, "velocity")
            out.send(mido.Message("note_on", channel=ch, note=a.note, velocity=a.velocity))
            time.sleep(a.length)
            out.send(mido.Message("note_off", channel=ch, note=a.note))
            print("sent note %d" % a.note)
        elif a.cmd == "panic":
            cc(123, 0)


if __name__ == "__main__":
    main()
