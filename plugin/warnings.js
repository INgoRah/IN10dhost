/* Forwards device problems (ACT_DEV_WARN: CRC errors) to Telegram, see
   the telegram plugin in README.md.

   "crc_failed" (a ds2408 read failed on every retry) is sent at once,
   but each at most once per QUIET seconds. The plain "crc" ones are
   mostly line noise that the retry already fixed: they are counted and
   one message per device and kind goes out per QUIET seconds, with how
   many there were. */
const ACT_CFG_LOAD		= 0;
const ACT_CFG_SAVE		= 1;
const ACT_INITIALIZED	 = 2;
const ACT_READY		   = 3;
const ACT_PERIODIC_SECOND = 4;
const ACT_ALARM_BEFORE	= 6;
const ACT_ALARM_AFTER	 = 7;
const ACT_DEV_CHANGE	  = 8;
const ACT_DEV_WATCHDOG	= 9;
const ACT_DEV_WARN		= 10;

const QUIET = 600;

let now = 0;
/* rom/kind -> { at: when last sent, n: counted since } */
const seen = {};

function onAction(code, val, data)
{
	switch (code) {

	case ACT_READY:
		ow.notify("Restarted in10dfs");
		return 1;
	/* an alarming device was handled; val is the bus number and data
	   carries the full address of the device that raised it */
	case ACT_ALARM_AFTER:
		return 1;
	/* a device changed state - note this also fires for changes this
	   script causes itself, so guard against driving in a loop */
	case ACT_DEV_CHANGE:
		return 1;
	case ACT_DEV_WATCHDOG:
		if (data) {
			ow.notify(data.rom + " Watchdog! stat=0x" +
				data.status.toString(16).toUpperCase().padStart(2, "0"));
		}
		return 1;
	case ACT_DEV_WARN:
		if (data) {
			const key = data.rom + "/" + data.kind;
			const s = seen[key] || (seen[key] = { at: -QUIET, n: 0 });

			s.n++;
			if (now - s.at < QUIET)
				return 0;
			const times = s.n > 1 ? " (x" + s.n + ")" : "";
			const icon = data.kind === "crc_failed" ? "❌ " : "⚠️ ";
			ow.notify(icon + data.rom + " bus " + data.bus + ": " + data.msg + times);
			s.at = now;
			s.n = 0;
		}
		return 1;

	case ACT_PERIODIC_SECOND:
		now++;
		return 0;
	}

	return 0;
}
