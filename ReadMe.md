# Introduction

## Features

## Limitations

# Building and running

## Requirements
apt install fuse3 libfuse3 libfuse3-dev nlohmann-json3-dev
if using gpio:
apt install libgpiod3 libgpiod-dev
sudo apt install libgtest-dev libgmock-dev

## native build
mkdir build-native
cmake -DUSE_GPIO=NO -DUSE_I2C=NO -S . -B build-native
cmake --build build-native

## Cross build
Requires
apt install fuse3:armhf libfuse3-4:armhf libfuse3-dev:armhf libgpiod3:armhf libgpiod-dev:armhf nlohmann-json3-dev
mkdir build-arm
cmake -DUSE_I2C=ON -DUSE_GPIO=NO \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -S . -B build-arm
cmake --build build-arm

## run as

'''
sudo build-arm/bin/in10dfs -f -o allow_other /mnt/1wire
'''

optional with -d for the data file

## Sbuild debian package

sudo sbuild-apt trixie-armhf apt-get install gpiod libgpiod-dev fuse3 libfuse3-dev

'''
sbuild --dist=trixie --arch=armhf --no-run-lintian --no-clean-source --extra-repository="deb http://deb.debian.org/debian trixie-backports main"
'''

optional with -chroot=trixie-armhf


# start up

- passive mode: no i2c interaction, no gpio interrupt:
  arduino mode != 16, poll=0 and --gpio-pin=0
- soft start: full service, but no initial interraction

## arduino mode = 0x10 and gpio set

Full handling of arduino interrupts

## switch mode 0x14

Full switching

## switches

A switch connects a button input (latch) to an output (PIO). Add one by
writing to `/switches/add`, remove it by writing the same line to
`/switches/del`, `/switches/list` shows all:

```sh
# [bus].[adr].[latch] [bus].[adr].[pio] [secs] [on_press]
echo "1.2.3 2.2.0" > /mnt/1wire/switches/add          # toggles 2.2.0
echo "1.2.5 2.2.1 30" > /mnt/1wire/switches/add       # on for 30s
echo "1.2.6 2.2.1 30 1" > /mnt/1wire/switches/add     # on for 30s, press again: off
```

Latch 11..18 is a long press, 21..28 the start of a long press.

`/switches/list` shows one line per switch. It starts the way the
switch was added, so it can be copied to `add` or `del` as is, followed
by the device and pin names (the rom and `PIO.<n>` while none are set,
see `name` and `pin.<n>/name` of a device) and, for a timed switch, its
timing and when its running timer switches off:

```
3 switches
1.2.3 -> 2.2.0: Hallway, PIO.2 (latch 3) -> Light board, Ceiling
1.2.5 -> 2.2.1 30 0: Hallway, Stairs button (latch 5) -> Light board, Stairs, timed 30s, press restarts, off in 12s
1.2.16 -> 2.2.1 30 1: Hallway, Stairs button (latch 6, long) -> Light board, Stairs, timed 30s, press off
```

Without `secs` a press toggles the output. With `secs` it is a timed
switch: a press on the output while it is off switches it on and starts
its timer, which switches it off after `secs` seconds. A press while
the timer runs depends on `on_press`:

- `0` (default): the timer starts again, the output stays on
- `1`: the output is switched off and the timer stopped

A timer only starts when the timed switch itself switches the output
on. If it is on already (switched on through the file system or by
another button), a timed press starts no timer: with `on_press` `1` it
switches the output off, with `0` it leaves it on. Switching off ends
the timer, whether by a button or by the timer. A plain button ends
the timer whenever it switches, on or off: switched on by it, the
output stays on until it is switched off again. Timed buttons of one
output share its timer, each restarting it with its own time.
So buttons can be combined, e.g. one that switches on for good and one
that switches on for a while.

The timer belongs to the output, so all timed switches of one output
share it. Adding a switch that exists already sets its timing anew.

## mode: soft
 - no reading of any device status or config
 - no initialization of any device setting (thresholds, brightness, etc.)

## mode: default
