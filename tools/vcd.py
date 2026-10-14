#!/usr/bin/env python3
"""Decodes a 1-Wire trace (status/1wire.vcd or a saved
<log dir>/1wire-<YYYYMMDD-HHMMSS>.vcd) into a readable timeline.

  tools/vcd.py 1wire-20261013-191350.vcd [--from SECONDS] [--raw]

By default a 1-Wire transaction - a reset and the bytes up to the next
reset or other event - is one line, written bytes and read bytes split
by "|", ROM commands decoded:
  <seconds> +<ms since previous> ch<n> MATCH 29.0402FBFD66778F F0 88 00 | FD FD ..
A register read (F0) also gets its CRC16 checked.

--raw prints one line per logged event instead:
  <seconds since first entry> +<ms since previous> <state> <name> ch<n> <data>

bus_state holds the event code (see DS2482::log_event() callers), the
chN_data variables the byte that came with it. Data is only in the trace
for channels 0..2, events on channel 3 or without a channel show "ch-".
"""
import sys

STATES = {
	'-': "idle",
	'0': "reset",
	'S': "search",
	'R': "read",
	'W': "write",
	'C': "channel",
	'?': "poll",		# background worker woke up, data = timeout ms
	'!': "irq",		# Arduino interrupt serviced
	'1': "alarm",		# ds2408 alarm, data = device adr[1]
	'2': "switch",		# actor_handle(), data = pio
	'3': "second",		# periodic second, data = plugin actions
	'4': "dev_poll",
	'5': "alarm_poll",
	'E': "crc_err",		# ds2408 register read CRC mismatch, data = status read
}


def parse(path):
	"""yields (timestamp us, state, channel or None, data or None)"""
	ts = None
	state = ch = data = None
	with open(path) as f:
		for line in f:
			line = line.strip()
			if line.startswith('#'):
				if state is not None:
					yield ts, state, ch, data
				ts = int(line[1:])
				state = ch = data = None
			elif line.startswith('b') and ' ' in line:
				val, var = line[1:].split()
				if not val:
					continue
				val = int(val, 2)
				if var == 'a':
					state = val
				elif var in 'bcd':
					ch = 'bcd'.index(var)
					data = val
	if state is not None:
		yield ts, state, ch, data


ROM_CMDS = {
	0x55: "MATCH",
	0xCC: "SKIP",
	0xF0: "SEARCH",
	0xEC: "ALARM_SEARCH",
}


def crc16(data, crc=0):
	"""1-Wire CRC16, as DS2482::crc16()"""
	odd = (0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0)
	for b in data:
		c = (b ^ crc) & 0xff
		crc >>= 8
		if odd[c & 0xf] ^ odd[c >> 4]:
			crc ^= 0xC001
		c <<= 6
		crc ^= c
		c <<= 1
		crc ^= c
	return crc & 0xffff


def byte(b):
	# bus 3 bytes are not in the trace
	return "--" if b is None else f"{b:02X}"


def rom_str(rom):
	return byte(rom[0]) + "." + "".join(byte(b) for b in rom[1:])


def hexs(data):
	return " ".join(byte(b) for b in data)


def decode(ch, events):
	"""one transaction: events (state, data) after the reset"""
	text = f"ch{'-' if ch is None else ch} "
	ops = list(events)
	search = [d for c, d in ops if c == 'S']
	ops = [(c, d) for c, d in ops if c != 'S']
	if not ops:
		return text + "RESET"
	first = ops[0][1] if ops[0][0] == 'W' else None
	if first in ROM_CMDS:
		text += ROM_CMDS[first]
		ops = ops[1:]
	if first == 0x55:
		rom = [d for _, d in ops[:8]]
		ops = ops[8:]
		text += " " + (rom_str(rom) if len(rom) == 8 else hexs(rom))
	elif first in (0xF0, 0xEC):
		# data per found device is all the trace has of a search
		text += " -> " + (rom_str(search) if len(search) == 8
				  else hexs(search) if search else "none")
	# runs of written / read bytes
	runs = []
	for c, d in ops:
		if runs and runs[-1][0] == c:
			runs[-1][1].append(d)
		else:
			runs.append((c, [d]))
	if runs:
		text += (" " if first in ROM_CMDS else "") + " | ".join(hexs(r) for _, r in runs)
	# register read: command, address, data, inverted CRC16
	if (len(runs) == 2 and runs[0][0] == 'W' and runs[1][0] == 'R'
	    and runs[0][1][0] == 0xF0 and len(runs[0][1]) == 3
	    and len(runs[1][1]) > 2):
		data = runs[0][1] + runs[1][1][:-2]
		crc = ~crc16(data) & 0xffff
		ok = runs[1][1][-2:] == [crc & 0xff, crc >> 8]
		text += "  CRC ok" if ok else f"  CRC BAD (expected {crc & 0xff:02X} {crc >> 8:02X})"
	return text


def transactions(entries):
	"""groups bus events from a reset on: yields (ts, line, last ts)"""
	cur = None
	for ts, state, ch, data in entries:
		c = chr(state)
		if cur and c in "WRS" and ch == cur[1]:
			cur[2].append((c, data))
			cur[3] = ts
			continue
		if cur:
			yield cur[0], decode(cur[1], cur[2]), cur[3]
			cur = None
		if c == '0':
			cur = [ts, ch, [], ts]
		else:
			name = STATES.get(c, '?')
			ds = '' if data is None else f" {data:02x}"
			yield ts, f"{c} {name}{ds}", ts
	if cur:
		yield cur[0], decode(cur[1], cur[2]), cur[3]


def main():
	args = sys.argv[1:]
	start = 0.0
	raw = '--raw' in args
	if raw:
		args.remove('--raw')
	if '--from' in args:
		i = args.index('--from')
		start = float(args[i + 1])
		del args[i:i + 2]
	if len(args) != 1:
		print(__doc__.strip(), file=sys.stderr)
		return 1
	t0 = prev = None
	if raw:
		for ts, state, ch, data in parse(args[0]):
			if t0 is None:
				t0 = prev = ts
			if (ts - t0) / 1e6 >= start:
				c = chr(state)
				print(f"{(ts - t0) / 1e6:12.6f} +{(ts - prev) / 1000:9.3f}ms "
				      f"{c} {STATES.get(c, '?'):10} "
				      f"ch{'-' if ch is None else ch} "
				      f"{'' if data is None else f'{data:02x}'}")
			prev = ts
		return 0
	for ts, line, end in transactions(parse(args[0])):
		if t0 is None:
			t0 = prev = ts
		if (ts - t0) / 1e6 >= start:
			dur = f" ({(end - ts) / 1000:.1f}ms)" if end != ts else ""
			print(f"{(ts - t0) / 1e6:12.6f} +{(ts - prev) / 1000:9.3f}ms {line}{dur}")
		prev = ts
	return 0


if __name__ == '__main__':
	sys.exit(main())
