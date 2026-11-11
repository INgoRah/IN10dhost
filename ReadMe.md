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

## saving the config

The config (`data.json`, see `-d`) is saved at shutdown, on demand and,
if set, periodically:

```sh
echo 1 > /mnt/1wire/settings/save              # save now
echo 3600 > /mnt/1wire/settings/save_interval  # every hour, 0 = off (default), up to 86400
```

A save never overwrites the file in place: it writes `data.json.tmp`,
syncs it to disk and renames it over `data.json`. A crash or power loss
leaves either the old or the new file, never a broken one.
`status/health` shows the time of the last save.

## status

`/status` shows how the daemon is doing:

- `version`: the release from `include/version.h`, with the git commit
  it was built from (`-dirty` for local changes) unless it is a Debian
  package build: `0.4.0 (6aea5439)`
- `health`: uptime in seconds, the number of warnings and errors since
  the start (also those the log level does not show) and the last one
  of each with its time
- `1wire.vcd`: the recent 1-Wire bus traffic as a VCD trace

```
uptime 86400
warnings 3
errors 0
last_warning 2026-10-09 08:12:03 20.0200F8FE66771E: DS2450 read result CRC mismatch
last_error -
last_save 2026-10-09 08:00:00
```

`1wire.vcd` only holds the last few thousand bus events and is gone
with a restart, so it is also saved as `1wire-<YYYYMMDD-HHMMSS>.vcd`
into `/opt/lib/in10dfs` (`--log-dir` for another one, `$HOME` if it
cannot be written): at shutdown, when a ds2408 reports a watchdog reset
(at most once a minute) and on demand. The newest 20 are kept.

```sh
echo 1 > /mnt/1wire/settings/vcd_save          # save the trace now
```

## ds2408 watchdog

A ds2408 reset by its watchdog loses its outputs, PWM levels, threshold
and brightness. The alarm handling notices it from the status register,
writes the cached state back (the outputs as they were before the
reset), counts it in `<rom>/watchdog` and hands plugins an
`ACT_DEV_WATCHDOG` event, e.g. for a script to apply a current
brightness (see `plugin/example.js`). The same happens when any other
register read sees the reset, e.g. an uncached read of `<rom>/BYTE`.

An owSlave firmware older than 2024-11 raises no alarm after a
watchdog reset. For such a device let in10dfs check its status
regularly, the latches are left alone:

```sh
echo 60 > /mnt/1wire/29.0800F7FF66772C/poll    # every 60 s
```

## temperature polling

A ds1820 (or an owSlave with temperature support) is read every
`<rom>/poll` seconds: the conversion is started and read 1 s later,
without holding the bus in between. Intervals below 2 s convert and
read at once, the value may then be the one before.

```sh
echo 60 > /mnt/1wire/28.0501FAFE6677A0/poll    # every 60 s
```

While the temperature changes, e.g. with a window open, it is polled
faster: a reading more than `fast_delta` K off the one before polls
every `poll / fast_factor` seconds (1 s at least), until 3 readings in a
row stayed within `fast_delta` K. Both are set per device in the json
config, defaults 0.2 K and 5 (60 s become 12 s); a `fast_factor` of 1
or a `fast_delta` of 0 switches it off:

```json
{"type": "ds1820", "bus": 1, "rom": "28.0501FAFE6677A0", "id": 0,
 "name": "living room", "poll": 60, "fast_delta": 0.2, "fast_factor": 5}
```

An alarm of the device (TH/TL; an owSlave also raises one on each
change with config_info2[4] bit 0) reads its last conversion at once,
but starts none: the interval stays what starts the conversions.

## firmware update

An owSlave with the 1-Wire bootloader (ATmega168, ATtiny84 or
ATtiny85, owSlave env `<mcu>-boot`, see its README) can be updated over
the bus. Write the image - Intel
HEX as PlatformIO builds it (`firmware.hex`) or the raw binary - to
the device's `firmware` file, read it to see how it goes:

```sh
echo /tmp/firmware.hex > /mnt/1wire/29.0500FAFF6677FB/firmware
cat /mnt/1wire/29.0500FAFF6677FB/firmware    # writing 12/50, done, 6338 bytes
```

The image is checked when written (a broken file is the answer of the
write); the update runs in the background, the other devices keep
working. in10dfs asks the application into its bootloader (config
write `0xB0 0x07 0xF8`), writes the pages, each read back, and
starts the new application. Page 0 is erased first and written last: an
update broken off leaves the device in its bootloader, it answers there
with its own ROM, and the update can simply be done again. Meanwhile
the device's other files answer `EBUSY`. After the update the outputs
are restored as after a watchdog reset.

## switches

A switch connects a button input (latch) to an output (PIO). Add one by
writing to `/switches/add`, remove it by writing the same line to
`/switches/del`, `/switches/list` shows all:

```sh
# [bus].[adr].[latch] [bus].[adr].[pio] [secs] [on_press] [dark]
echo "1.2.3 2.2.0" > /mnt/1wire/switches/add          # toggles 2.2.0
echo "1.2.5 2.2.1 30" > /mnt/1wire/switches/add       # on for 30s
echo "1.2.6 2.2.1 30 1" > /mnt/1wire/switches/add     # on for 30s, press again: off
echo "1.2.7 2.2.3 60 dark" > /mnt/1wire/switches/add  # on for 60s, only when dark
```

Latch 11..18 is a long press, 21..28 the start of a long press.

The target `0.0.0` switches every light off, all pins with the role
`light` (see roles and rooms below), and stops their off timers. It
takes no timing:

```sh
echo "1.2.4 0.0.0" > /mnt/1wire/switches/add          # all lights off
```

`/switches/list` shows one line per switch. It starts the way the
switch was added, so it can be copied to `add` or `del` as is, followed
by the device and pin names (the rom and `PIO.<n>` while none are set,
see `name` and `pin.<n>/name` of a device) and, for a timed switch, its
timing and when its running timer switches off:

```
3 switches, dark
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

With `dark` at the end a switch switches its output on only while it
is dark; switching it off works at any time. Whether it is dark is the
astro flag `/switches/dark`, `1` from sunset to sunrise, `0` during the
day. in10dfs does not work it out itself: an external script sets it,
through the file or from the jsengine (`ow.dark_set(true)`), e.g.
`plugin/astro.js`, which calculates sunrise and sunset for a place. It is
`1` until set, so without a script the switches work as without
`dark`. It is not saved either, the script sets it after a restart.

```sh
echo 0 > /mnt/1wire/switches/dark                     # sunrise
echo 1 > /mnt/1wire/switches/dark                     # sunset
```

A room can have a dark flag of its own, e.g. an inner room without
windows or one with its own brightness sensor: `/rooms/<room>/dark`
(see rooms below). A `dark` switch then goes by the flag of the room
its output pin is in, the others keep following `/switches/dark`.
`/switches/list` shows the room's flag after `only when dark`, e.g.
`, only when dark (kitchen day)`.

Switches can be masked for a while: writing a switch's line to
`/switches/disable` makes its presses do nothing, `all` masks every
switch. `/switches/enable` takes the same and undoes it. Timers already
running still switch off. The mask is saved with the config (a
switch's `disabled`, and `switches_disabled` for `all`), so it holds
across a restart once the config was saved (see saving the config
above). `/switches/list` marks a masked switch with
`, disabled`, and its first line with `, all disabled`:

```sh
echo "1.2.3 2.2.0" > /mnt/1wire/switches/disable      # this one
echo all > /mnt/1wire/switches/disable                # every switch
echo all > /mnt/1wire/switches/enable
```

## roles and rooms

Every ds2408 pin can be told what is connected to it and where, besides
its `func` (how the pin behaves). Both are only kept in the config,
never written to the device:

```sh
echo light   > /mnt/1wire/29.0701F8FE6677F4/pin.2/role
echo kitchen > /mnt/1wire/29.0701F8FE6677F4/pin.2/room
```

The roles are `light`, `socket`, `fan`, `heating`, `blind` (outputs),
`button`, `contact`, `motion` (inputs) and `none`, the default. An empty
write resets the role to `none` and clears the room.

A room is a name of letters, digits, `-` and `_`, no spaces, kept in
lower case: `Kitchen` is written as `kitchen`. A room of an older
config is converted when loaded (`Living Room` becomes `living_room`).

`/role/<role>/` collects the pins of one role across all devices:

```sh
cat /mnt/1wire/role/light/all        # how many lights are on
echo 0 > /mnt/1wire/role/light/all   # switch all lights off
cat /mnt/1wire/role/light/list       # <rom>.<pin>  on|off  room  name
cat /mnt/1wire/role/light/29.0701F8FE6677F4.2      # 1 on, 0 off
echo 1 > /mnt/1wire/role/light/29.0701F8FE6677F4.2
```

Only outputs are switched; an input with a role is listed with its
sensed level.

`/rooms/<room>/` collects the pins of one room, whatever their role.
`/rooms` lists every room a pin is in; the names are matched
case-insensitively, `rooms/Kitchen` is `rooms/kitchen`:

```sh
cat /mnt/1wire/rooms/kitchen/list    # <rom>.<pin>  role  on|off  name
cat /mnt/1wire/rooms/kitchen/dark    # 1 dark, 0 day, as its switches see it
echo 1 > /mnt/1wire/rooms/kitchen/dark      # its own flag, whatever the astro flag says
echo auto > /mnt/1wire/rooms/kitchen/dark   # follow /switches/dark again
```

`dark` reads the flag in effect: the room's own while it has one,
otherwise `/switches/dark`. A flag can be set for a room no pin is in
yet; such a room is listed as well until its flag is set back to
`auto` (or an empty write). Like the astro flag the room flags are not
saved. From the jsengine: `ow.dark_set(dark, room)`,
`ow.dark_clear(room)` and `ow.is_dark(room)`. Scripts get the same through `ow.pins()` and
`ow.role_switch()`, and a ds2408's change event names the role and room
of every output that changed (see `plugin/README.md`).

## mode: soft
 - no reading of any device status or config
 - no initialization of any device setting (thresholds, brightness, etc.)

## mode: default
