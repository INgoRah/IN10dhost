#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cstring>
#include <gpiod.h>
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

Logger logger;

#define GPIO_LINE 6  // Arduino Interrupt
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
#if USE_GPIO
	struct pollfd fds[2];
	int state;
	int ret;
#else
	struct pollfd fds[1];
#endif
	int timeout = ow.get_poll();
	Ard_i2c* arduino;
	HrClock::time_point tp;

	if (timeout == 0)
		timeout = -1;
	else
		timeout *= 1000;
	arduino = (Ard_i2c*)ow.find(0, 9, 0xAD);
	if (!arduino) {
		logger.warn("No arduino device");
		return;
	}
	// Setup the buffer before the loop
	fds[0].fd = wake_fd;
#if USE_GPIO
	struct gpiod_edge_event_buffer *event_buffer = gpiod_edge_event_buffer_new(16);
	fds[1].fd = gpiod_line_request_get_fd(line);
#endif
	while (running.load()) {
		// periodic work
		// setup 1 sec timer for polling
#if USE_GPIO
		ret = 0;
		state = gpiod_line_request_get_value(line, GPIO_LINE);
		if (state == 1) {
			int tm;

			fds[0].events = POLLIN;
			fds[1].events = POLLIN | POLLERR;
			// define next timeout for polling the devices, periodic second timer
			if (timeout == -1)
				tm = ow.poll_time();
			else
				tm = std::min(ow.poll_time(), timeout);

			ret = poll(fds, 2, tm);
#if 0
			tp = HrClock::now();
#endif
		}
		//logger.verbose("Poll " + std::to_string(ret) + " handled, GPIO state=" + std::to_string(state));
		arduino->interrupt();
#if 0
		if (state == 1) {
			auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(HrClock::now() - tp);
			logger.info(std::format("used {} ", duration));
		}
#endif
		if (ret > 0 && (fds[1].revents & POLLIN))
			// READ THE EVENTS to clear the poll status
			gpiod_line_request_read_edge_events(line, event_buffer, 16);
		if (ret == 0)
			ow.poll();
#else
		poll(fds, 1, 5000);
#endif
		// External wake (CLI / FUSE)
		if (fds[0].revents & POLLIN) {
			uint64_t v;
			(void)read(wake_fd, &v, sizeof(v)); // clears event
		}
	}
	printf("Background worker exiting.\n");
}

void setup_gpio()
{
#if USE_GPIO
	chip = gpiod_chip_open("/dev/gpiochip0");
	if (!chip)
		perror("gpiod_chip_open");
#ifdef GPIOD_V2
	struct gpiod_line_config *cfg = gpiod_line_config_new();
	static const unsigned int offsets = GPIO_LINE;
	struct gpiod_line_settings *settings;
	settings = gpiod_line_settings_new();
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);
	gpiod_line_config_add_line_settings(cfg, &offsets, 1, settings);

	/* Request line */
	line = gpiod_chip_request_lines(chip, NULL, cfg);
	gpiod_line_config_free(cfg);
#else
#error not tested/supported
	line = gpiod_chip_get_line(chip, GPIO_LINE);  // GPIO number
	if (!line)
		perror("gpiod_chip_get_line");
	if (gpiod_line_request_both_edges_events(
		line,
		"ds2482-daemon") < 0) {
		perror("gpiod_line_request");
	}
#endif
	int state = gpiod_line_request_get_value(line, GPIO_LINE);
	logger.debug("GPIO set up, state=" + std::to_string(state));
#endif
}

int setup()
{
	setup_gpio();
	wake_fd = eventfd(0, EFD_NONBLOCK);
	ow.begin(&ds);
	swHdl.begin(&ds);
#ifdef USE_I2C_HOST
#endif
	//arduino.begin(&ow);

	return 0;
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

	std::signal(SIGSEGV, segfault_handler);
	enable_rt();
	setup();

	string f = std::filesystem::current_path();
	logger.set_level(LogLevel::INFO);
	f = f + "/data.json";
	fs_init(&fs_ops);
	try {
		ow.load(f.c_str());
	}
	catch (const std::exception& e) {
		printf("loading failed %s\n" , e.what());
	}
	std::thread worker(background_worker);
	ret = fuse_main(argc, argv, &fs_ops, nullptr);
	printf("fuse ended with %d\n", ret);

	// trigger wake up of thread
	uint64_t v = 1;
	(void)write(wake_fd, &v, sizeof(v)); // Triggers fds[0]
	try {
		running.store(false);
		worker.join();
	}catch (const std::exception& e) {
		printf("worker stopping failed with an exception: %s\n", e.what());
	}
#ifdef USE_GPIO
	gpiod_line_request_release(line);
	gpiod_chip_close(chip);
#endif
	try {
		ow.save(f.c_str());
	} catch (const std::exception& e) {
		printf("Failed to save config: %s\n", e.what());
	}

	return ret;
}
