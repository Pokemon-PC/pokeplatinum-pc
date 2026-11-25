#!/usr/bin/env python3
"""Prints the PPL_TEST_KEYS and PPL_TEST_TOUCH values that play Platinum from
power-on to the player's room on a blank save (French ROM, frame timings of
the PC port at 59.83 Hz): skip the intro, title, NEW GAME, Rowan's control
tutorial (touch YES), "no more info", touch the Poke Ball, default gender,
a name typed with A, first suggested rival name.

    eval "$(python tools/pc_port/newgame_keys.py [--until FRAME])"
    -> exports PPL_TEST_KEYS and PPL_TEST_TOUCH

The player is in the room (TV programme) after about frame 15500.
"""
import argparse


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--until", type=int, default=16000, help="last frame of A presses")
    a = ap.parse_args()
    keys = ["1000:START", "1300:START", "1600:START", "1900:A", "2200:A"]
    keys += ["%d:A" % f for f in range(2400, 5551, 45)]
    keys += ["6100:A", "6200:DOWN", "6240:DOWN", "6280:A"]       # NON MERCI
    keys += ["%d:A" % f for f in range(6400, 7301, 45)]
    keys += ["%d:A" % f for f in range(8000, a.until + 1, 45)]
    touch = ["%d:120:80" % f for f in range(5600, 6001, 60)]     # OUI (understood)
    touch += ["%d:128:96" % f for f in range(7350, 7901, 60)]    # Poke Ball
    print("export PPL_TEST_KEYS='%s'" % ",".join(keys))
    print("export PPL_TEST_TOUCH='%s'" % ",".join(touch))


if __name__ == "__main__":
    main()
