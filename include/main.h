#ifndef _MAIN_H
#define _MAIN_H

#include "version.h"
#include <chrono>
#include <thread>
#include "logger.h"

// MAX_BUS is defined in ow_devices.h (guarded by #ifndef), not here:
// this used to also define it as 3, which - since main.h is included
// before ow_devices.h nearly everywhere - silently overrode the real
// value (4) in most translation units. cache.busses/dev_list etc. are
// all sized off the 4-bus definition, so a stale 3 here caused
// out-of-bounds access (e.g. cache.busses[dev->bus] in update_data())
// for any device actually found on the 4th bus.
#define MAX_ADR 13

typedef uint8_t byte;

extern Logger logger;

extern void log_time();
extern void printDst8(union d_adr_8 dst);
extern void printDst(union pio dst);
extern void printSrc(union s_adr src);
extern void dumpCfg();

extern uint8_t min;
extern uint8_t hour;
extern uint8_t light;
extern byte light_sensor;
extern unsigned long host_lock;
extern int wake_fd;

#define delay(ms) 	std::this_thread::sleep_for(std::chrono::milliseconds(ms));

#endif
