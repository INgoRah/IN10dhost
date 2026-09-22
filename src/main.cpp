#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cstring>
#ifdef GPIOD_V2
#include <gpiod.h>
#endif
#include <poll.h>
#include <time.h>
#include <sched.h>
#include <csignal>
#include <iostream>
/* threading */
#include <atomic>
#include <chrono>
#include <sys/eventfd.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <fuse3/fuse.h>
#include <filesystem>

#include "main.h"
#include "fs.h"
#include "ds2482.h"
#include "ow_devices.h"
#include "ds2408.h"
#include "switch_handler.h"
#include "ard_i2c.h"
#include "cli.h"

Logger logger;

// The global Cli instance (cli) lives in cli.cpp: pure argv handling,
// with no FUSE/GPIO/hardware dependency, so it's reachable from GTest
// too (main.cpp itself is swapped out for test/main.cpp in TESTING
// builds).
struct gpiod_chip *chip;
#ifdef GPIOD_V2
struct gpiod_line_info *linfo;
struct gpiod_line_request *line;
#else
struct gpiod_line *line;
#endif

extern void fs_init(fuse_operations* fs_ops);

struct fuse_operations fs_ops = {};
DS2482 ds("/dev/i2c-0", 0x18);
OwDevices ow;
SwitchHandler swHdl (&ow);

using HrClock = std::chrono::high_resolution_clock;

void segfault_handler(int signal)
{
	std::cerr << "Caught segmentation fault (signal " << signal << ")\n";

	std::exit(signal); // Gracefully terminate
}

std::atomic<bool> running{true};
int wake_fd;

void background_worker()
{
#ifdef USE_GPIO
	struct pollfd fds[2];
	Ard_i2c* arduino = nullptr;
	struct gpiod_edge_event_buffer *event_buffer = nullptr;
	nfds_t nfds = 1;
	// cli.gpio_pin == 0 (--gpio-pin=0), a failed setup_gpio(), or no Arduino
	// device configured on the bus all mean "no GPIO edge fd to watch";
	// the device-polling loop below is identical either way, it just
	// doesn't get the extra fd to also watch for interrupts on.
	arduino = (Ard_i2c*)ow.find(0, 9, 0xAD);
	if (arduino)
		arduino->set_mode(0x10);
	if (cli.gpio_pin != 0 && line != nullptr) {
		if (!arduino) {
			// TODO restart working after search
			logger.warn("No arduino device, GPIO edge handling disabled");
		} else {
			event_buffer = gpiod_edge_event_buffer_new(16);
			fds[1].fd = gpiod_line_request_get_fd(line);
			nfds = 2;
			logger.info("periodic worker started on GPIO pin " + std::to_string(cli.gpio_pin));
		}
	}
	if (nfds == 1)
		logger.info("periodic worker started, GPIO inactive");
#else
	struct pollfd fds[1];
	const nfds_t nfds = 1;
	logger.info("periodic worker started, built without GPIO support");
#endif
	fds[0].fd = wake_fd;

	int ret;
	int tm;
	int timeout = ow.get_poll();

	if (timeout == 0)
		timeout = -1;
	else
		timeout *= 1000;

	while (running.load()) {
		// Single, GPIO-independent device-polling cadence: block for up
		// to the next due poll, then run it - identical whether GPIO is
		// active, deactivated, or not built in at all. When GPIO *is*
		// active, fds[1] rides along on the same poll() call so a wire
		// interrupt is serviced immediately rather than waiting out tm.
		tm = (timeout == -1) ? ow.poll_time() : std::min(ow.poll_time(), timeout);

		fds[0].events = POLLIN;
#ifdef USE_GPIO
		if (nfds == 2)
			fds[1].events = POLLIN | POLLERR;
#endif
		ret = poll(fds, nfds, tm);

#ifdef USE_GPIO
		if (nfds == 2 && ret > 0 && (fds[1].revents & POLLIN)) {
			logger.verbose("GPIO edge, servicing Arduino");
			ds.log_event(STATE_EVENT, 0);
			arduino->interrupt();
			// clear the pending edge status
			gpiod_line_request_read_edge_events(line, event_buffer, 16);
		}
#endif
		if (ret == 0) {
			ds.log_event(STATE_POLL, tm);
			ow.dev_poll();
			ow.alarm_poll();
		}
		// External wake (CLI / FUSE)
		if (ret > 0 && (fds[0].revents & POLLIN)) {
			uint64_t v;
			ret = read(wake_fd, &v, sizeof(v)); // clears event
		}
	}
	printf("Background worker exiting.\n");
}

void setup_gpio()
{
#ifdef USE_GPIO
	if (cli.gpio_pin == 0) {
		logger.info("GPIO deactivated (--gpio-pin=0)");
		return;
	}
	chip = gpiod_chip_open("/dev/gpiochip0");
	if (!chip) {
		perror("gpiod_chip_open");
		return;
	}
#ifdef GPIOD_V2
	struct gpiod_line_config *cfg = gpiod_line_config_new();
	unsigned int offsets = (unsigned int)cli.gpio_pin;
	struct gpiod_line_settings *settings;
	settings = gpiod_line_settings_new();
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);
	gpiod_line_config_add_line_settings(cfg, &offsets, 1, settings);

	/* Request line */
	line = gpiod_chip_request_lines(chip, NULL, cfg);
	gpiod_line_config_free(cfg);
	if (!line) {
		perror("gpiod_chip_request_lines");
		gpiod_chip_close(chip);
		chip = nullptr;
		return;
	}
#else
#error not tested/supported
	line = gpiod_chip_get_line(chip, cli.gpio_pin);  // GPIO number
	if (!line)
		perror("gpiod_chip_get_line");
	if (gpiod_line_request_both_edges_events(
		line,
		"ds2482-daemon") < 0) {
		perror("gpiod_line_request");
	}
#endif
	int state = gpiod_line_request_get_value(line, cli.gpio_pin);
	logger.debug("GPIO set up on pin " + std::to_string(cli.gpio_pin) + ", state=" + std::to_string(state));
#endif
}

string setup()
{
	string f;

	setup_gpio();
	wake_fd = eventfd(0, EFD_NONBLOCK);

	if (cli.data_path.empty()) {
		f = std::filesystem::current_path();
		f = f + "/data.json";
	} else {
		f = cli.data_path;
	}
	logger.info("Data file: " + f);
	logger.set_level(LogLevel::INFO);
	fs_init(&fs_ops);
	// resets dev_list/cache.devices/timers to a clean slate; must run
	// before load() populates them, not after (it would wipe out
	// everything load() just loaded)
	ow.init();
	try {
		ow.load(f.c_str());
	}
	catch (const std::exception& e) {
		printf("loading failed %s\n" , e.what());
		// create default config
	}
	ow.begin(&ds, cli.soft_start);
	swHdl.begin(&ds);

	return f;
}

void enable_rt(void)
{
	struct sched_param param;
	memset(&param, 0, sizeof(param));
	param.sched_priority = 80;  // 1–99

	if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
		perror("sched_setscheduler");
	}
}

int main(int argc, char* argv[])
{
	int ret;
	string f;

	//std::signal(SIGSEGV, segfault_handler);
	printf("Starting IN10dhost daemon %s...\n", __TIME__);
	cli.parse(argc, argv);
	if (cli.help_requested) {
		// -h/--help was deliberately left in argv by cli.parse(), so
		// fuse_main() below also prints its own help for FUSE's own
		// options; skip everything else (RT priority, GPIO/DS2482/
		// switch setup, the worker thread) since none of that matters
		// for a --help invocation.
		cli.print_help();
		fs_init(&fs_ops);
		return fuse_main(argc, argv, &fs_ops, nullptr);
	}
	logger.info("GPIO pin: " + std::to_string(cli.gpio_pin) + (cli.gpio_pin == 0 ? " (deactivated)" : ""));
	if (cli.soft_start)
		logger.info("Soft start requested"); // TODO: not yet acted on
	enable_rt();
	f = setup();
	std::thread worker(background_worker);
	ret = fuse_main(argc, argv, &fs_ops, nullptr);
	printf("fuse ended with %d\n", ret);
	// trigger wake up of thread
	uint64_t v = 1;
	if (write(wake_fd, &v, sizeof(v)) < 0) // Triggers fds[0]
		perror("write wake_fd");
	try {
		running.store(false);
		worker.join();
	}catch (const std::exception& e) {
		printf("worker stopping failed with an exception: %s\n", e.what());
	}
#ifdef USE_GPIO
	if (line)
		gpiod_line_request_release(line);
	if (chip)
		gpiod_chip_close(chip);
#endif
	try {
		ow.save(f.c_str());
	} catch (const std::exception& e) {
		printf("Failed to save config: %s\n", e.what());
	}

	return ret;
}
