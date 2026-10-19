#pragma once
#include <stdint.h>
extern uint32_t test_tick;
#define xTaskGetTickCount() test_tick
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
