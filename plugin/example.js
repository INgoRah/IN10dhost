/*
 * Example IN10dhost script, run by the jsengine plugin.
 *
 * Point the plugin at this file from data.json:
 *
 *   "plugins": {
 *       "jsengine": { "script": "/opt/lib/in10dfs/example.js" }
 *   }
 *
 * The daemon calls onAction() for every event. Return 1 when the event
 * was handled, 0 to leave it alone - see action_code in plugin.h.
 */

/* keep in sync with enum action_code in include/plugin.h */
const ACT_CFG_LOAD        = 0;
const ACT_CFG_SAVE        = 1;
const ACT_INITIALIZED     = 2;
const ACT_READY           = 3;
const ACT_PERIODIC_SECOND = 4;
const ACT_ALARM_BEFORE    = 6;
const ACT_ALARM_AFTER     = 7;
const ACT_DEV_CHANGE      = 8;

/* a switch we care about, and the light it should drive */
const BUTTON = "29.0200FDFF6677F8";
const LAMP   = "29.0500FAFF6677FB";

let seconds = 0;

function onAction(code, val, data)
{
    switch (code) {

    case ACT_READY:
        ow.log("devices are up, script running");
        return 1;

    /* an alarming device was handled; val is the bus number and data
       carries the full address of the device that raised it */
    case ACT_ALARM_AFTER:
        if (data)
            ow.log("alarm on bus " + val + " from " + data.rom);
        return 1;

    /* a device changed state - note this also fires for changes this
       script causes itself, so guard against driving in a loop */
    case ACT_DEV_CHANGE:
        if (data) {
            if (data.type == "ds2408")
                ow.log(data.rom + " pio is now 0x" + data.pio.toString(16));
            /* a light sensor on channel A: hand its reading on to the
               lamp's ds2408 as brightness (0..5 V -> 0..255), so its
               timers only switch when it is dark enough */
            if (data.type == "ds2450") {
                const b = Math.min(255, Math.round(data.volt_a * 51));
                ow.log(data.rom + " brightness is now " + b);
                ow.brightness_set(LAMP, b);
            }
        }
        return 1;

    /* once a second: read a sensor and switch the lamp on when it gets
       warm. temp() returns null when that rom is not a ds1820. */
    case ACT_PERIODIC_SECOND:
        seconds++;
        if (seconds % 30 != 0)
            return 0;
        const t = ow.temp("28.0501FAFE6677A0");
        if (t !== null) {
            ow.log("temperature " + t.toFixed(2));
            if (t > 24.0)
                ow.pin_switch(LAMP, 0, ow.ON);
        }
        return 1;
    }

    return 0;
}
