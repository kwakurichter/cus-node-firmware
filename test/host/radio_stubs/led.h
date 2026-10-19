#pragma once
#include <stdbool.h>
typedef enum { ledRanging, ledSync, ledMode } led_e;
static inline void ledOn(led_e led) { (void)led; }
static inline void ledBlink(led_e led, bool once) { (void)led; (void)once; }
