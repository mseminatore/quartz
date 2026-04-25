//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Software timer implementation (tick-driven, periodic and one-shot).
//---------------------------------------------------------------------------
#include <stdint.h>
#include "../include/rtos_timer.h"
#include "../include/rtos_trace.h"
#include "port.h"

static rtos_timer_t *g_timer_list = NULL;  // singly-linked active timer list
                                            // owned by core 0 — only rtos_tick_handler
                                            // on core 0 calls rtos_timer_tick()

//---------------------------------------------------------------------------
// Create a timer. The caller must provide storage for the timer struct, 
// which can be on the caller's stack or in static memory. Returns a handle 
// to the timer, or NULL on failure (e.g. invalid parameters). The timer is 
// created in the inactive state; call xTimerStart() to start it.
//---------------------------------------------------------------------------
rtos_handle_t rtos_timer_create(rtos_timer_t *timer,
                                const char   *name,
                                uint32_t      period_ticks,
                                int           periodic,
                                void        (*cb)(rtos_handle_t timer))
{
    if (!timer || !cb || period_ticks == 0) return NULL;

    size_t i = 0;
    while (name && name[i] && i < RTOS_TASK_NAME_LEN - 1) {
        timer->name[i] = name[i];
        i++;
    }
    timer->name[i]         = '\0';
    timer->period_ticks    = period_ticks;
    timer->remaining_ticks = period_ticks;
    timer->periodic        = periodic;
    timer->active          = 0;
    timer->cb              = cb;
    timer->next            = NULL;
    return (rtos_handle_t)timer;
}

//---------------------------------------------------------------------------
// Start a timer. If the timer is already active, this has no effect.
//---------------------------------------------------------------------------
void rtos_timer_start(rtos_handle_t handle)
{
    rtos_timer_t *timer = (rtos_timer_t *)handle;
    if (!timer || timer->active) return;

    port_enter_critical();

    timer->remaining_ticks = timer->period_ticks;
    timer->active          = 1;
    timer->next            = g_timer_list;
    g_timer_list           = timer;

    port_exit_critical();
}

//---------------------------------------------------------------------------
// Stop a timer. If the timer is not active, this has no effect.
//---------------------------------------------------------------------------
void rtos_timer_stop(rtos_handle_t handle)
{
    rtos_timer_t *timer = (rtos_timer_t *)handle;
    if (!timer || !timer->active) return;

    port_enter_critical();

    // Remove from list
    if (g_timer_list == timer) 
    {
        g_timer_list = timer->next;
    } else 
    {
        rtos_timer_t *cur = g_timer_list;
        while (cur && cur->next != timer)
            cur = cur->next;

        if (cur) cur->next = timer->next;
    }

    timer->active = 0;
    timer->next   = NULL;

    port_exit_critical();
}

//---------------------------------------------------------------------------
// Reset a timer's count to its period. If the timer is not active, start it.
//---------------------------------------------------------------------------
void rtos_timer_reset(rtos_handle_t handle)
{
    rtos_timer_t *timer = (rtos_timer_t *)handle;
    if (!timer) return;

    port_enter_critical();
    timer->remaining_ticks = timer->period_ticks;

    if (!timer->active) 
    {
        timer->active = 1;
        timer->next   = g_timer_list;
        g_timer_list  = timer;
    }

    port_exit_critical();
}

//---------------------------------------------------------------------------
// Called from rtos_tick_handler() in task.c on every tick.
//---------------------------------------------------------------------------
void rtos_timer_tick(void)
{
    rtos_timer_t *cur = g_timer_list;
    rtos_timer_t *prev = NULL;

    while (cur)
    {
        rtos_timer_t *next = cur->next;

        if (cur->remaining_ticks > 0)
            cur->remaining_ticks--;

        if (cur->remaining_ticks == 0) 
        {
            RTOS_TRACE_TIMER_FIRE(cur);
            cur->cb((rtos_handle_t)cur);

            if (cur->periodic) {
                cur->remaining_ticks = cur->period_ticks;
                prev = cur;
            } else {
                // Remove one-shot timer from list
                if (prev)
                    prev->next = next;
                else
                    g_timer_list = next;
                cur->active = 0;
                cur->next   = NULL;
            }
        } else 
        {
            prev = cur;
        }

        cur = next;
    }
}
