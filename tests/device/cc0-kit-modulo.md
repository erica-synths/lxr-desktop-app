# Device test: MIDI bank select (CC 0) values past the last kit

The LXR loads a kit when it receives MIDI CC 0 on a channel it listens to.
There are 64 kits (0-63) but a CC value goes up to 127. This change makes
values 64-127 wrap around to a valid kit: `kit = value % 64`, so 64 is kit 0,
67 is kit 3, 100 is kit 36 and 127 is kit 63. Before, they loaded memory past
the end of the kit array as if it were a kit. Values 0-63 are unchanged.

The unit test (`test_frontpanel_cc`) covers the parser. This covers the real
hardware.

## What you need
- An LXR-02 and a way to send a single MIDI CC. `lxr_midi.py` in this
  directory does it from a computer through a USB MIDI interface such as a
  Roland UM-ONE (OUT cable to the LXR's MIDI in). Set it up with
  `python3 -m venv venv && venv/bin/pip install -r requirements.txt`, check
  with `venv/bin/python lxr_midi.py ports`, and use `-c N` for the channel.
- Two firmware images: one built from `main` (without the change) and one with
  it. Build with `cmake --preset release && cmake --build --preset release`;
  the image is `build/release/LXR_V2_V1.7.img`.
- Back up the SD card first.

## Naming the images on the SD card
The updater installs the **first** file it finds in the card's top folder whose
name starts with `LXR` and ends with `.img`. It does not look for the newest,
and the order is the order of the directory, not necessarily alphabetical. So
at any time exactly one file may match `LXR*.img`:
- Name the image you want to install `LXR_...img`, for example
  `LXR_V2_V1.7_baseline.img` or `LXR_V2_V1.7_cc0-kit-modulo.img`.
- Park every other image under a name that does not start with `LXR`, or end
  it with something other than `.img`, for example `PARKED_cc0-kit-modulo.img.bak`
  or `OLD_stock_v1.71.img.bak`. Keep a copy of the stock image off the card.
- Files starting with `._` (left by Finder) are ignored. Eject the card in
  Finder before removing it.
- To switch between the two builds, rename the current one to a parked name,
  then put the other one in as `LXR_...img`, and check with `ls LXR*.img` that
  only one is listed.

## Setup (both firmware versions)
1. Update the firmware: put the `.img` on the SD card (only one `LXR*.img`),
   hold the Data knob while switching on until `LXR Updater` appears, wait for
   `Update OK!`. The display then says `please reboot!`: switch the LXR off and
   on again by hand, without holding the knob.
2. On the global settings page (Shift + Config), check that the CC receive
   filter is on and note the MIDI channel to send on. It must be the global
   channel or one of the voice channels.
3. Stop the sequencer. Don't save a kit, pattern or project during the test.
4. Go to the Load page with Kit selected (hold Load and press Kit). Don't
   touch the encoder.
5. Write down the names of kits 0, 3, 36 and 63 (scroll the encoder without
   confirming, then go back), so you can tell which kit loaded.

## A. Without the change (shows the problem)
1. Send CC 0 value 3 (`lxr_midi.py -c N bank 3 20`). The kit should change,
   as before.
2. Send CC 0 value 67 (`lxr_midi.py -c N bank 67 100 127`; it waits for Enter
   after each value). Without the change this loads memory past the kit
   array as a kit, not kit 3.
3. Write down what you see after each value: the kit number shown, the kit
   name, the sound, and any odd behaviour. The result depends on what is in
   memory, so it may differ between runs. Repeat once after a power cycle.

## B. With the change (shows it is fixed)
1. Update to the new firmware and repeat the setup.
2. Send CC 0 values 0, 3, 63 (`lxr_midi.py -c N bank 0 3 63`). Each loads that
   kit, as before.
3. Send 64, 67, 100 and 127 (`lxr_midi.py -c N bank 64 67 100 127`). The
   expected kits are 0, 3, 36 and 63. Check that the kit number shown matches
   and that the sound is the kit you noted for that number.

## Record
Firmware (commit), MIDI source, channel, and for each value sent: the kit
number, kit name and anything unusual.

## Results (2026-11-08)
Roland UM-ONE, MIDI channel 1, CC 0 sent with `lxr_midi.py`, Load + Kit page.

Before, firmware built from the commit this branch is based on, without the
change (source v1.7):

| CC 0 | Kit shown |
|---|---|
| 7 | 7 "PA" |
| 67 | [67], no name (there is no kit 67) |

Before, stock LXR-02 firmware v1.71 on the same unit:

| CC 0 | Kit shown |
|---|---|
| 7 | 7 "pa" |
| 17 | 17 "robowasp" |
| 67 | [67], no name |
| 73 | [73], no name |

After (firmware built from this branch, source v1.7):

| CC 0 | Expected (value % 64) | Kit shown |
|---|---|---|
| 7 | 7 | 7 "pa" |
| 64 | 0 | 0 "brownie" |
| 67 | 3 | 3 "VHS" |
| 127 | 63 | 63 "octopus" |

Not run on the device: 73 and 100 after the change (the unit test covers every
value 0-127).
