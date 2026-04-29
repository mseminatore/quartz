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
    rtos_tick_t        period_ticks;
    rtos_tick_t        abs_expiry_tick; // absolute tick at which the timer fires
    int                periodic;        // 1 = auto-reload, 0 = one-shot
    int                active;
    void             (*cb)(rtos_handle_t timer);
    struct rtos_timer *next;            // intrusive list link (sorted by abs_expiry_tick)
} rtos_timer_t;

rtos_handle_t rtos_timer_create(rtos_timer_t *timer,
                                const char   *name,
                                rtos_tick_t   period_ticks,
                                int           periodic,
                                void        (*cb)(rtos_handle_t timer));

void rtos_timer_start(rtos_handle_t timer);
void rtos_timer_stop(rtos_handle_t timer);
void rtos_timer_reset(rtos_handle_t timer);   // restart countdown from full period

// Returns 1 if the timer is currently active (running), 0 otherwise.
int rtos_timer_is_active(rtos_handle_t timer);

// Called by the tick handler — not part of the public application API.
// now is the current tick count at the time of the call.
void rtos_timer_tick(rtos_tick_t now);

// Returns ticks until the earliest active timer fires, or RTOS_WAIT_FOREVER
// if no timers are active. Used by tickless idle. Not part of the public API.
rtos_tick_t rtos_timer_min_remaining(void);

#endif // RTOS_TIMER_H
