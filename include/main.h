#ifndef _MAIN_H
#define _MAIN_H

#include "version.h"
#include <chrono>
#include <thread>
#include "logger.h"

#define MAX_BUS 3
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
