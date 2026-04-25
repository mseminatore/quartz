//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Software timer API (tick-driven, periodic and one-shot).
//---------------------------------------------------------------------------
#ifndef RTOS_TIMER_H
#define RTOS_TIMER_H

#include <stdint.h>
#include "rtos_config.h"
#include "rtos_task.h"   // defines rtos_handle_t

// Timer storage — declare as a static variable and pass its address.
typedef struct rtos_timer {
    char               name[RTOS_TASK_NAME_LEN];
    uint32_t           period_ticks;
    uint32_t           remaining_ticks;
    int                periodic;       // 1 = auto-reload, 0 = one-shot
    int                active;
    void             (*cb)(rtos_handle_t timer);
    struct rtos_timer *next;           // intrusive list link
} rtos_timer_t;

rtos_handle_t rtos_timer_create(rtos_timer_t *timer,
                                const char   *name,
                                uint32_t      period_ticks,
                                int           periodic,
                                void        (*cb)(rtos_handle_t timer));

void rtos_timer_start(rtos_handle_t timer);
void rtos_timer_stop(rtos_handle_t timer);
void rtos_timer_reset(rtos_handle_t timer);   // restart countdown from full period

// Called by the tick handler — not part of the public application API.
void rtos_timer_tick(void);

#endif // RTOS_TIMER_H
