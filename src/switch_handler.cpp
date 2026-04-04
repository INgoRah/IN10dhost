#include <main.h>
#include "switch_handler.h"
#include "ow_devices.h"
#include "ds2408.h"

struct _sw_tbl sw_tbl[MAX_SWITCHES];

void to_json(json& j, const _sw_tbl& b) {
	j = json{
		{"src_bus", b.src.sa.bus },
		{"src_adr", b.src.sa.adr },
		{"src_latch", b.src.sa.latch },
		{"src_press", b.src.sa.press },
		{"dst_bus", b.dst.da.bus},
		{"dst_adr", b.dst.da.adr},
		{"dst_pio", b.dst.da.pio}
	};
}

void from_json(const json& j, _sw_tbl& b) {
	try {
		if (j.contains("src")) {
			j.at("src").get_to(b.src.data);
			j.at("dst").get_to(b.dst.data);
		}
		if (j.contains("src_adr")) {
			int d;
			j.at("src_bus").get_to<int>(d);
			b.src.sa.bus = d;
			j.at("src_adr").get_to(d);
			b.src.sa.adr = d;
			j.at("src_latch").get_to(d);
			b.src.sa.latch = d;
			j.at("src_press").get_to(d);
			b.src.sa.press = d;
			j.at("dst_bus").get_to(d);
			b.dst.da.bus = d;
			j.at("dst_adr").get_to(d);
			b.dst.da.adr = d;
			j.at("dst_pio").get_to(d);
			b.dst.da.pio = d;
		}
	}
	catch (const std::exception& e) {
		// setup c.switches?
	}
}

SwitchHandler::SwitchHandler()
{
	cur_latch = 0;
	this->ds = NULL;
}

SwitchHandler::SwitchHandler(OwDevices* devs)  : SwitchHandler()
{
	this->ow = devs;
}

uint8_t SwitchHandler::bitnumber()
{
	int i, res = 1;

	for (i = 0x1; i <= 0x80; i = i << 1) {
		if (cur_latch & i) {
			cur_latch -= i;
			return res;
		}
		res++;
	}
	/* invalid */
	return 0xff;
}

void SwitchHandler::initSwTable()
{
	/*
	sw_tbl[0].src.sa.bus = 2;
	sw_tbl[0].src.sa.latch = 5;
	sw_tbl[0].src.sa.adr = 2;
	sw_tbl[0].dst.da.pio = 1;
	sw_tbl[0].dst.da.bus = 1;
	sw_tbl[0].dst.da.adr = 2;
	sw_tbl[0].dst.da.type = TYPE_DS29X;
	cache.switches.push_back(sw_tbl[0]);
	*/
}

void SwitchHandler::begin(DS2482 *ow)
{
	this->ds = ow;
	mode = MODE_ALRAM_HANDLING | MODE_ALRAM_POLLING | MODE_AUTO_SWITCH;
	logger.info("starting switch handler");
	initSwTable();
}

/* Convert from alarm location to a lookup table format (16 bit)
 * latch - bit mask to be converted to bit number
 * press - 0: pressing, > 0: press time
 *
 * latch = cur_latch (from data[2])
 * press = data[6]
 */
uint16_t SwitchHandler::srcData(uint8_t busNr, uint8_t adr1)
{
	union s_adr src;
	//uint8_t v;

	src.data = 0;
	src.sa.bus = busNr;
	src.sa.adr =  adr1 & 0x3f;
	// get (the first if multiple) bit which is set
	// TODO returns 0xff if invalid -> check here
	// or return immediately if cur_latch == 0
	src.sa.latch = bitnumber();
	//v = ow->getVersion(src.sa.bus, src.sa.adr);
	if (data[6] == 0xff)
		src.sa.press = 0;
	else {
		/* first two latches are usually output
		* signaled from auto switch. The corresponding latch
		* is not signaled!
		* */
		if (data[6] == 0)
			src.sa.press = 2;
		else if (data[6] > (350 / 32))
			src.sa.press = 1;
		else
			src.sa.press = 0;
	}
	logger.verbose(std::format("src {}.{}.{}.{}", (int)src.sa.bus, (int)src.sa.adr, (int)src.sa.latch, (int)src.sa.press));
#if 0
	if (debug > 1) {
		printf("%d.%d.", src.sa.bus, src.sa.adr);
		if (src.sa.press)
			printf("%d ", 10 * src.sa.press + src.sa.latch);
		else
			printf("%d ", src.sa.latch);
		if (data[6] != 0xff) {
			printf(" time=%d", data[6] * 32);
		}
		printf("\n");
	}
#endif

	return src.data;
}

/**
 * low level set the level for supporting PIOs
 *
 * @param id id in dimLevel cache array
 * @param d data read from PIO before
 * @param level level [0..100] will be converted to 8 bit (0..254)
 * @return true if successful otherwise false on error
 *
 * @remark This function does not notify the host for any change. Needs to be done by the
 * calling function using the updated value in "d"
 */
bool SwitchHandler::setLevel(union pio dst, uint8_t adr[8], uint8_t id, uint8_t level)
{
	(void)dst;
	(void)adr;
	(void)id;
	(void)level;
	return false;
}

/**
 * Low level set the PIO on or off and checking the target state before
 *
 * @param id id in dimLevel cache array
 * @param d data read from PIO before
 * @return true if switched (was not in the target state), otherwise false
 * @remark This function notifies the host for any change.

 */
/* TODO check for light at the light sensor ...
	bus == 0, adr == 1, pio = 1
	light_sensor = 1;
*/
bool SwitchHandler::setPio(union pio dst, uint8_t adr[8], uint8_t d, enum _pio_mode state)
{
	(void)dst;
	(void)adr;
	(void)d;
	(void)state;

	return false;
}

/**
 * Called from top level control to switch to a dedicated level or
 * simply on or off.
 * If set already it will ignore and return false
 * level 0 .. 100 will be translated in values from 0 .. 254 in the lower
 * function. If level < 32 directly switch off for non PWM outputs
 * */
bool SwitchHandler::switchLevel(union pio dst, uint8_t level)
{
	(void)dst;
	(void)level;

	return false;
}

/* latch in cur_latch - bit mask to be converted to bit number
   uses data[1] for actual IO status
   data[6] for press time */
bool SwitchHandler::switchHandle(uint8_t busNr, uint8_t adr1)
{
	union s_adr src;
	uint8_t i;

	src.data = srcData(busNr, adr1);
	logger.debug("switch handling id " + std::to_string(src.sa.adr));
	for (i = 0; i < MAX_TIMED_SWITCH; i++) {
	}
	for (i = 0; i < cache.switches.size(); i++) {
		if (src.data == cache.switches[i].src.data) {
			logger.debug(std::format("sw {}.{}.{} -> {}.{}.{}",
				(int)cache.switches[i].src.sa.bus,
				(int)cache.switches[i].src.sa.adr,
				(int)cache.switches[i].src.sa.latch,
				(int)cache.switches[i].dst.da.bus,
				(int)cache.switches[i].dst.da.adr,
				(int)cache.switches[i].dst.da.pio
			));
			union pio p = cache.switches[i].dst;
			ds2408* dev = (ds2408*)ow->find(p.da.bus, p.da.adr, 0x29);
			if (dev) {
				logger.debug("dev found...");
				uint8_t d = dev->data[PIO_OUT] & (0x1 << p.da.pio);
				if (d)
					d = dev->data[PIO_OUT] & ~(0x01 << p.da.pio);
				else
					d = dev->data[PIO_OUT] | (0x01 << p.da.pio);
				dev->pio_set(d);
			} else {
				logger.debug("dev not found...");
			}
		}
	}
	return false;
}

bool SwitchHandler::dev_alarm(uint8_t bus, uint8_t adr[8])
{
	if (adr[0] == 0x29) {
		uint8_t res, to = 30;
		ds2408* dev = (ds2408*)ow->find(bus, adr[1], 0x29);
		if (!dev)
			return false;
		//dev->set_alarm(true);
		res = dev->reg_read(true);
		/* fill data for use in switchHandle */
		if (res == 0xaa || res == 0xff) {
			// loop over all set bits
			// cur_latch is reduced by each call to bitnumber
			cur_latch = dev->data[PIO_LATCH];
			//logger.verbose(std::format(" -> {} {}", cur_latch, data[5]));
			if (dev->data[STAT] & 0x40) {
				/* status in 5 signals a dimming down */
			}
			if (dev->data[STAT] & 0x88) {
				uint8_t i;
				logger.warn("Watchdog!");
				for (i = 0; i < 9; i++) {
					printf("%02X ", dev->data[i]);
				}
				printf("%02X ", dev->data[i]);
				//ds->dump();
				return false;
			}
			if (cur_latch == 0xff)
				return false;
			while (cur_latch != 0 && to > 0) {
				data[6] = dev->data[6];
				switchHandle(bus, adr[1]);
				to--;
			}
		}
	}
	return true;
}

bool SwitchHandler::alarmHandler(uint8_t busNr)
{
	uint8_t adr[8];
	uint8_t j = 0;
	uint8_t cnt = 10;
	bool ret;

	if (!ds)
		return false;
	//ds = bus[busNr];
	ret = ds->selectChannel(busNr);
	if (!ret)
		// this could be a timeout or other issue
		// must be repeated
		return false;
	ds->reset_search();
	while (ds->search(adr, false)) {
		j++;
		logger.debug(std::format("Alarm {}.{}", busNr, adr[1]));
		dev_alarm(busNr, adr);
		cnt--;
		if (ds->last_err || cnt == 0) {
#ifdef USE_DEBUG
			log_time();
			printf("Error searching = %d\n", ds->last_err);
#endif
		}
	}

	return j > 0 ? true : false;
}
