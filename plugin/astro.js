/*
 * Astro flag: works out sunrise and sunset for a place and sets the
 * switches' dark flag (/switches/dark) from them, so switches added
 * with "dark" only switch on between sunset and sunrise. No network
 * needed, the sun's position is calculated (NOAA sunrise equation,
 * about a minute off at most).
 *
 *   "plugins": {
 *       "jsengine": { "scripts": [ "/opt/lib/in10dfs/astro.js" ] }
 *   }
 *
 * The flag is only set when the script's own day/night changes (and
 * once at the start), so a value written to /switches/dark by hand
 * holds until the next sunrise or sunset.
 *
 * Today's times can be read from status/astro:
 *
 *   sunrise 07:01
 *   sunset 16:34
 *   dark 0 (day until 16:34)
 */

/* keep in sync with enum action_code in include/plugin.h */
const ACT_INITIALIZED     = 2;
const ACT_READY           = 3;
const ACT_PERIODIC_SECOND = 4;

/* the place: degrees, north and east positive */
const LATITUDE  = 48.0351795816986;
const LONGITUDE = 11.598468571901321;

/* how far below the horizon the sun is at "sunrise"/"sunset":
   90.833 the official sunrise and sunset (upper rim of the sun, with
   refraction), 96 civil twilight (still light enough outside),
   102 nautical twilight */
const ZENITH = 90.833;

/* minutes added to the times: dark from sunset + DARK_DELAY, day from
   sunrise + DAY_DELAY, e.g. -30 for dark half an hour before sunset */
const DARK_DELAY = 0;
const DAY_DELAY  = 0;

/* rooms with times of their own, e.g. an inner room that gets dark
   early and light late: { "hall": { dark: -60, day: 60 } }, minutes as
   DARK_DELAY and DAY_DELAY. They get their own flag, see
   /rooms/<room>/dark; all others follow /switches/dark. */
const ROOMS = {};

/* how often to check, in seconds */
const CHECK_SECS = 60;

const STATUS_FILE = "status/astro";
const MIN_MS = 60 * 1000;
const DAY_MS = 24 * 60 * MIN_MS;
const RAD = Math.PI / 180;

let seconds = 0;
/* last value set per room, "" the astro flag itself */
let last = {};

function julian(ms)  { return ms / DAY_MS + 2440587.5; }
function unix_ms(jd) { return (jd - 2440587.5) * DAY_MS; }

/* Sunrise and sunset around the sun's transit nearest to t (ms since
   the epoch), as ms since the epoch: { rise, set }. Without either on
   that day (polar day or night): { polar: "day" | "night" }. */
function sun_times(t)
{
	/* days since 2000-01-01 12:00 UTC of the transit nearest to t */
	const n = Math.round(julian(t) - 2451545.0 - 0.0009 + LONGITUDE / 360);
	const j = n + 0.0009 - LONGITUDE / 360;
	/* mean anomaly, equation of the center, ecliptic longitude */
	const m = (357.5291 + 0.98560028 * j) % 360;
	const c = 1.9148 * Math.sin(m * RAD) + 0.0200 * Math.sin(2 * m * RAD)
		+ 0.0003 * Math.sin(3 * m * RAD);
	const l = (m + c + 180 + 102.9372) % 360;
	const transit = 2451545.0 + j + 0.0053 * Math.sin(m * RAD)
		- 0.0069 * Math.sin(2 * l * RAD);
	/* declination of the sun, hour angle at ZENITH */
	const sin_d = Math.sin(l * RAD) * Math.sin(23.4397 * RAD);
	const cos_d = Math.cos(Math.asin(sin_d));
	const cos_w = (Math.cos(ZENITH * RAD) - Math.sin(LATITUDE * RAD) * sin_d)
		/ (Math.cos(LATITUDE * RAD) * cos_d);

	if (cos_w > 1)
		return { polar: "night" };
	if (cos_w < -1)
		return { polar: "day" };
	const w = Math.acos(cos_w) / RAD / 360;
	return { rise: unix_ms(transit - w), set: unix_ms(transit + w) };
}

/* Dark at now, with the delays in minutes: { dark, until } - until is
   when it changes next, null on a polar day or night. */
function dark_at(now, dark_delay, day_delay)
{
	const s = sun_times(now);

	if (s.polar)
		return { dark: s.polar == "night", until: null };
	const day = s.rise + day_delay * MIN_MS;
	const dark = s.set + dark_delay * MIN_MS;
	if (now < day)
		return { dark: true, until: day };
	if (now < dark)
		return { dark: false, until: dark };
	/* after sunset: day again with tomorrow's sunrise */
	const next = sun_times(now + DAY_MS / 2);
	return { dark: true, until: next.polar ? null : next.rise + day_delay * MIN_MS };
}

function hhmm(ms)
{
	const d = new Date(ms);
	return ("0" + d.getHours()).slice(-2) + ":" + ("0" + d.getMinutes()).slice(-2);
}

/* sets a flag when it differs from what this script set last */
function apply(room, state)
{
	if (last[room] === state.dark)
		return false;
	last[room] = state.dark;
	ow.log("astro: " + (room ? room + " " : "") + (state.dark ? "dark" : "day")
		+ (state.until !== null ? " until " + hhmm(state.until) : ""));
	if (room)
		ow.dark_set(state.dark, room);
	else
		ow.dark_set(state.dark);
	return true;
}

function check()
{
	const now = Date.now();
	let changed = apply("", dark_at(now, DARK_DELAY, DAY_DELAY));

	for (const room in ROOMS) {
		const r = ROOMS[room];
		if (apply(room, dark_at(now, r.dark || 0, r.day || 0)))
			changed = true;
	}
	return changed;
}

function onRead(path)
{
	const now = Date.now();
	const s = sun_times(now);
	const state = dark_at(now, DARK_DELAY, DAY_DELAY);
	let text;

	if (s.polar)
		text = "polar " + s.polar + "\n";
	else
		text = "sunrise " + hhmm(s.rise) + "\nsunset " + hhmm(s.set) + "\n";
	text += "dark " + (state.dark ? 1 : 0);
	if (state.until !== null)
		text += " (" + (state.dark ? "dark" : "day") + " until " + hhmm(state.until) + ")";
	return text + "\n";
}

function onAction(code, val, data)
{
	switch (code) {
	case ACT_INITIALIZED:
		ow.file_add(STATUS_FILE, 128);
		return 1;

	/* the flag is not saved: set it right away after a (re)start */
	case ACT_READY:
		last = {};
		check();
		return 1;

	case ACT_PERIODIC_SECOND:
		/* also the first one after a reload of this script */
		if (seconds++ % CHECK_SECS != 0)
			return 0;
		return check() ? 1 : 0;
	}
	return 0;
}
