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
unchanged just gets its config handed back — which is how `jsengine`
notices an edited script without the library itself being touched.
A reloaded plugin keeps its config across the swap.

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
    "jsengine": { "script": "/opt/lib/in10dfsd/example.js" }
}
```

The key is the plugin name; the daemon loads `lib<name>.so`, looking
first next to the executable and then in `/opt/lib/in10dfsd`. The value
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

Available to scripts:

| call | does |
| --- | --- |
| `ow.log(msg)` | write to the daemon log |
| `ow.pio_set(rom, pio)` | drive a ds2408 PIO, returns true on success |
| `ow.temp(rom)` | read a ds1820 temperature, `null` if that rom is not one |
| `ow.volt(rom, channel)` | read a cached ds2450 voltage |

`rom` is always the dotted string form, `"29.0200FDFF6677F8"`, the same
form the event payloads carry. It is a string rather than a number
because a 64 bit rom code does not survive a JavaScript double intact.

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

### Events

| code | value | when | `val` / `data` |
| --- | --- | --- | --- |
| `ACT_INITIALIZED` | 2 | after config load | |
| `ACT_READY` | 3 | devices are up and usable | |
| `ACT_PERIODIC_SECOND` | 4 | once a second | |
| `ACT_ALARM_AFTER` | 7 | alarm, after default handling | `val` = bus, `data` = `{bus, rom}` |
| `ACT_DEV_CHANGE` | 8 | a device changed state | `data` = `{bus, rom, pio}` |

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
