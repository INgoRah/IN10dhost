# Plugins

The daemon loads plugins as shared objects at runtime and forwards
events to them. There are two ways to write one: a C++ plugin
implementing the `Plugin` interface, or a JavaScript file run by the
bundled `jsengine` plugin.

## Managing plugins through the filesystem

Every loaded plugin appears as a file under `settings/plugins` in the
mounted filesystem, and the usual filesystem verbs manage them:

```sh
ls  /mnt/1wire/settings/plugins          # what is loaded
cat /mnt/1wire/settings/plugins/jsengine # that plugin's config

touch /mnt/1wire/settings/plugins/example    # load libexample.so
touch /mnt/1wire/settings/plugins/example    # already loaded -> reload
rm    /mnt/1wire/settings/plugins/example    # unload
```

`touch` on a name that is not loaded yet loads it; on one that already
exists it reloads. Reloading only does real work when something
changed: the loader keeps a content hash of each `.so`, so a redeploy
that produced identical bytes is a no-op, and a plugin whose library is
unchanged just gets its config handed back, so a plugin can re-read
external files. A reloaded plugin keeps its config across the swap.

`jsengine` does not need this for its script: it notices an edited
script by itself, see below.

Writing to a plugin file works too, and `reload` there reloads every
plugin rather than just the one:

```sh
echo reload > /mnt/1wire/settings/plugins/jsengine
echo rm     > /mnt/1wire/settings/plugins/example   # same as rm
```

Plugins listed in the config file (`data.json`) are loaded at startup:

```json
"plugins": {
    "example":  { "enabled": true },
    "jsengine": { "script": "/opt/lib/in10dfs/example.js" }
}
```

The key is the plugin name; the daemon loads `lib<name>.so`, looking
first next to the executable and then in `/opt/lib/in10dfs`. The value
is handed to the plugin's `config_set()` and comes back out of
`config_get()` when the config is saved.

## Writing a JavaScript plugin

Point `jsengine` at a `.js` file and define a global `onAction`. See
[example.js](example.js) for a working one.

```js
function onAction(code, val, data)
{
    if (code == 7 && data)            /* ACT_ALARM_AFTER */
        ow.log("alarm on bus " + val + " from " + data.rom);
    return 1;                         /* 1 = handled, 0 = ignored */
}
```

Several scripts can run side by side, listed in the config:

```json
"jsengine": { "scripts": [ "/opt/lib/in10dfs/brightness.js",
                           "/opt/lib/in10dfs/vacation.js" ] }
```

`"script": "<file>"` still works for a single one. Every script runs in
a context of its own, so they do not see each other's globals: each can
have its own `onAction`, `onRead` and `onWrite` and variables of the
same name. Every event goes to all of them in the order of the list,
and what they return is added up. A script that fails to load or
throws leaves the others running, and a file one of them added cannot
be added by another.

Available to scripts:

| call | does |
| --- | --- |
| `ow.log(msg)` | write to the daemon log |
| `ow.pio_set(rom, byte)` | write all ds2408 PIOs at once (raw output latch byte) |
| `ow.pin_switch(rom, pin, state[, level])` | switch one ds2408 pin; `state` is `ow.ON`, `ow.OFF` or `ow.TOGGLE`, `level` is used by a PWM pin |
| `ow.level_set(rom, pin, level)` | set a ds2408 PWM pin to `level` 0..255, 0 is off |
| `ow.brightness_set(rom, value)` | hand a ds2408 the current brightness 0..255, for devices without their own light sensor |
| `ow.threshold_set(rom, value)` | brightness 0..255 above which a ds2408's timed lights switch |
| `ow.temp(rom)` | read a ds1820 temperature, `null` if that rom is not one |
| `ow.volt(rom, channel)` | read a cached ds2450 voltage |
| `ow.pins([role])` | the pins with that role (`"light"`, ...), or with any role, as `[{rom, pin, name, role, room, output, on}, ...]` |
| `ow.role_switch(role, on)` | switch every output with that role on (`true`/`ow.ON`) or off, true when all of them switched |
| `ow.file_add(path, size)` | add a file to the file system, e.g. `"status/my_stuff"`, true when it could be added (see [Files](#files)) |
| `ow.file_remove(path)` | remove one of the script's files again |

An unknown role throws a `RangeError`. For an input `on` is its sensed
level, for an output whether it is on.

Reading one of the script's files calls `onRead(path)`; what it returns
is the content, a number reads as its digits, `null` or `undefined`
makes the file unreadable. Writing calls `onWrite(path, data)` with the
written text, returning `false` refuses it. Without `onWrite` the files
are read only. A reload of the script drops its files, the new version
adds its own, typically from its top level:

```js
let target = "21";
ow.file_add("heating/target", 8);

function onRead(path)        { return target; }
function onWrite(path, data) { target = data.trim(); }
```

A ds2408's `ACT_DEV_CHANGE` payload carries, besides the raw `pio`
byte, a `changed` list with the outputs that changed: `{pin, on, name,
role, room}`. That is enough to notice a light coming on while nobody
is home:

```js
case ACT_DEV_CHANGE:
    if (data && data.changed)
        for (const c of data.changed)
            if (c.role == "light" && c.on && away)
                ow.log(c.room + " " + c.name + " switched on");
    return 1;
```

The ds2408 calls return true on success and false when `rom` is not a
ds2408 or the bus transfer failed. A `pin` outside 0..7 or a value
outside 0..255 throws a `RangeError`.

`rom` is always the dotted string form, `"29.0200FDFF6677F8"`, the same
form the event payloads carry. It is a string rather than a number
because a 64 bit rom code does not survive a JavaScript double intact.

Saving a script is enough to change it, no `touch` or reload needed.
Every event first checks the file's modification time, and on a change
its contents, and loads it again when they differ. The once-a-second
`ACT_PERIODIC_SECOND` event means an edit is live within about a
second. Like any load this starts that script from a clean global
object, so values kept in global variables start over. A script with a
syntax error is logged and stays inactive until it is fixed and saved
again. The other scripts keep running untouched.

Two limits worth knowing. A callback is interrupted after 200 ms, so a
runaway loop cannot hang the daemon. And events that arrive *while* a
callback is running are dropped rather than nested — otherwise a script
that sets a PIO from its `ACT_DEV_CHANGE` handler would trigger itself
forever.

Running scripts needs `libqjs0` installed on the target (it is in
`trixie-backports` for Debian 13). Without quickjs at build time the
`jsengine` plugin is simply not built and everything else still works.

## Writing a C++ plugin

Drop a `.cpp` into this directory: the build turns every file here into
its own `lib<file>.so`. Start from [example.cpp](example.cpp).

Implement `Plugin` (see [../include/plugin.h](../include/plugin.h)) and
export the two factory functions:

```cpp
extern "C" Plugin* create_plugin()        { return new MyPlugin(); }
extern "C" void destroy_plugin(Plugin* p) { delete p; }
```

The interface is:

| method | when |
| --- | --- |
| `init(logger, devices)` | right after loading |
| `action(ev)` | on every event, see below |
| `config_get()` / `config_set(j)` | config save/load, and on reload |
| `info()` | print a version banner |
| `exit()` | before unloading |
| `fs_read(path, buf, size)` / `fs_write(path, buf, size)` | a read or write of one of its files, see below |

`action()` receives an `ActionEvent`:

```cpp
struct ActionEvent {
    int         code;   /* ACT_* */
    int         val;    /* bus number for the alarm actions, else 0 */
    const json* data;   /* payload, or nullptr */
};
```

`data` is owned by the daemon and only valid for the duration of the
call — copy anything that has to outlive it.

### Files

A plugin can add files of its own to the file system, usually in
`init()`, through `files` (set before `init()` runs):

```cpp
files->file_add(this, "status/my_stuff", 32);   // /status/my_stuff
files->file_add(this, "heating/target", 8);     // /heating/target
files->file_remove(this, "heating/target");
```

The path is relative to the root, `/` separates directories. The
directories are made up from the paths; only reads and writes of the
files reach the plugin, in `fs_read()` and `fs_write()` with the path
as it was added. The size is what the file is listed with and the most
one read returns (the first read returns all of it); adding the same
path again changes it. Without its own `fs_write()` a file is read
only.

The root, `settings/` and `status/` can hold plugin files next to the
daemon's own, but none of the daemon's files can be taken, nor
`settings/plugins/`. `switches/`, `role/`, `alarm/`, `bus.<n>/` and the
devices belong to the daemon alone. A path taken already, by the daemon
or another plugin, as a file or as a directory, makes `file_add()`
return false. When a plugin is unloaded or reloaded its files go with
it. See [example.cpp](example.cpp), which adds `status/example` and
`example/echo`.

### Events

| code | value | when | `val` / `data` |
| --- | --- | --- | --- |
| `ACT_INITIALIZED` | 2 | after config load | |
| `ACT_READY` | 3 | devices are up and usable | |
| `ACT_PERIODIC_SECOND` | 4 | once a second | |
| `ACT_ALARM_AFTER` | 7 | alarm, after default handling | `val` = bus, `data` = `{bus, rom}` |
| `ACT_DEV_CHANGE` | 8 | a device changed state | `data` = `{bus, rom, pio}` |
| `ACT_DEV_WATCHDOG` | 9 | a ds2408 was reset by its watchdog, its cached outputs, levels, threshold and brightness are already restored | `val` = bus, `data` = `{bus, type, rom, status, brightness, threshold}`, `status` 0x88 for a watchdog, 0x80 for power-on or brown-out |

A plugin loaded after the start (`touch settings/plugins/<name>`, a
reload) gets `ACT_INITIALIZED` and `ACT_READY` as far as they were sent
already, on its own, right after its config is applied. The same goes
for a script the engine reloads because it was edited. So setting up in
`onAction` for those events (adding files, ...) works however late a
plugin or script starts.

`ACT_CFG_LOAD` (0), `ACT_CFG_SAVE` (1) and `ACT_ALARM_BEFORE` (6) are
declared in `plugin.h` but nothing dispatches them yet, so a handler
for one of those will never be called.

Return `1` from `action()` when the event was handled and `0` when it
was not. The alarm actions read more into it — see the comment on
`Plugin::action()` in `plugin.h`.

Reaching a device means looking it up and casting to the interface for
its type:

```cpp
IDev* dev = devices->get_dev(0x290200FDFF6677F8);
IDS2408* d = dynamic_cast<IDS2408*>(dev);   /* nullptr if not a ds2408 */
if (d)
    d->pio_set(1);
```

`IDS2408`, `IDS1820` and `IDS2450` are separate base classes, not
derived from `IDev`, so `dynamic_cast` sidecasts between them.

### Things to be careful about

`action()` is called from the background poll worker, from the FUSE
threads and from device code, so a plugin holding state needs its own
locking. A plugin that starts threads has to stop them in `exit()` —
the library is unloaded right after, and any thread still running in it
takes the daemon down with it.

An exception thrown out of `action()` is caught and logged, and the
other plugins still run. That containment is not airtight though: the
daemon links `-static-libstdc++`, so an exception crossing the plugin
boundary can fail to unwind and abort instead. Don't rely on throwing
across it.

## Building by hand

The CMake build does this for you; this is the equivalent single
command:

```sh
g++ -std=c++20 -fPIC -I ./include/ -I /usr/include/ -shared \
    plugin/basic.cpp -o build-native/bin/libbasic.so
```
