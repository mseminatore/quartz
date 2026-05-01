//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Software timer implementation (tick-driven, periodic and one-shot).
//
// The active timer list is kept sorted in ascending abs_expiry_tick order
// (using signed subtraction for correct uint32_t wraparound comparison).
// rtos_timer_tick() only needs to inspect the head, giving O(k) behaviour
// where k is the number of timers firing this tick (usually 0 or 1).
//
// NOTE: Timer callbacks must not call rtos_timer_reset() on the timer that
// is currently firing (i.e. on themselves). Calling rtos_timer_stop() on
// the currently-firing timer from its own callback is supported.
//---------------------------------------------------------------------------
#include <stdint.h>
#include "../include/rtos_timer.h"
#include "../include/rtos_trace.h"
#include "port.h"

#if RTOS_ENABLE_SOFTWARE_TIMERS

// Retrieve the current tick count without pulling in all of task.c.
extern rtos_tick_t rtos_task_tick_count(void);

static rtos_timer_t *g_timer_list  = NULL;  // sorted active timer list
                                             // owned by core 0 — only core 0's
                                             // rtos_tick_handler calls rtos_timer_tick()
static size_t        g_timer_count = 0;     // number of active timers

//---------------------------------------------------------------------------
// Insert timer into g_timer_list sorted by ascending abs_expiry_tick.
// Signed subtraction makes the comparison correct across uint32_t wrap.
// Caller must hold the critical section.
//---------------------------------------------------------------------------
static void timer_list_insert_sorted(rtos_timer_t *timer)
{
    timer->next = NULL;

    if (!g_timer_list ||
        (int32_t)(timer->abs_expiry_tick - g_timer_list->abs_expiry_tick) <= 0)
    {
        timer->next  = g_timer_list;
        g_timer_list = timer;
        return;
    }

    rtos_timer_t *cur = g_timer_list;
    while (cur->next &&
           (int32_t)(timer->abs_expiry_tick - cur->next->abs_expiry_tick) > 0)
        cur = cur->next;

    timer->next = cur->next;
    cur->next   = timer;
}

//---------------------------------------------------------------------------
// Remove timer from g_timer_list (does nothing if not present).
// Caller must hold the critical section.
//---------------------------------------------------------------------------
static void timer_list_remove(rtos_timer_t *timer)
{
    if (g_timer_list == timer)
    {
        g_timer_list = timer->next;
    } else
    {
        rtos_timer_t *cur = g_timer_list;
        while (cur && cur->next != timer)
            cur = cur->next;
        if (cur)
            cur->next = timer->next;
    }
    timer->next = NULL;
}

//---------------------------------------------------------------------------
// Create a timer. The caller must provide storage for the timer struct,
// which can be on the caller's stack or in static memory. Returns a handle
// to the timer, or NULL on failure (e.g. invalid parameters). The timer is
// created in the inactive state; call rtos_timer_start() to start it.
//---------------------------------------------------------------------------
rtos_handle_t rtos_timer_create(rtos_timer_t *timer,
                                const char   *name,
                                rtos_tick_t   period_ticks,
                                int           periodic,
                                void        (*cb)(rtos_handle_t timer))
{
    if (!timer || !cb || period_ticks == 0) return NULL;

    size_t i = 0;
    while (name && name[i] && i < RTOS_TASK_NAME_LEN - 1) {
        timer->name[i] = name[i];
        i++;
    }
    timer->name[i]        = '\0';
    timer->period_ticks   = period_ticks;
    timer->abs_expiry_tick = 0;
    timer->periodic        = periodic;
    timer->active          = 0;
    timer->cb              = cb;
    timer->next            = NULL;
    return (rtos_handle_t)timer;
}

//---------------------------------------------------------------------------
// Start a timer. If the timer is already active, this has no effect.
// Returns without starting if RTOS_MAX_TIMERS active timers already exist.
//---------------------------------------------------------------------------
void rtos_timer_start(rtos_handle_t handle)
{
    rtos_timer_t *timer = (rtos_timer_t *)handle;
    if (!timer || timer->active) return;

    port_enter_critical();

    if (g_timer_count >= RTOS_MAX_TIMERS)
    {
        port_exit_critical();
        return;
    }

    timer->abs_expiry_tick = rtos_task_tick_count() + timer->period_ticks;
    timer->active          = 1;
    g_timer_count++;
    timer_list_insert_sorted(timer);

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

    timer_list_remove(timer);
    timer->active = 0;
    g_timer_count--;

    port_exit_critical();
}

//---------------------------------------------------------------------------
// Reset a timer's countdown to its full period. If the timer is not active,
// start it. The new deadline is set relative to the current tick.
//---------------------------------------------------------------------------
void rtos_timer_reset(rtos_handle_t handle)
{
    rtos_timer_t *timer = (rtos_timer_t *)handle;
    if (!timer) return;

    port_enter_critical();

    if (timer->active)
    {
        timer_list_remove(timer);
        g_timer_count--;
    }
    else if (g_timer_count >= RTOS_MAX_TIMERS)
    {
        port_exit_critical();
        return;
    }

    timer->abs_expiry_tick = rtos_task_tick_count() + timer->period_ticks;
    timer->active          = 1;
    g_timer_count++;
    timer_list_insert_sorted(timer);

    port_exit_critical();
}

//---------------------------------------------------------------------------
// Returns 1 if the timer is currently active (running), 0 otherwise.
//---------------------------------------------------------------------------
int rtos_timer_is_active(rtos_handle_t handle)
{
    const rtos_timer_t *timer = (const rtos_timer_t *)handle;
    return (timer && timer->active) ? 1 : 0;
}

//---------------------------------------------------------------------------
// Returns the number of ticks until the soonest active timer fires, or
// RTOS_WAIT_FOREVER if no timers are active. O(1) because the list is sorted.
// Used by rtos_idle_next_wakeup_ticks() for tickless idle.
//---------------------------------------------------------------------------
rtos_tick_t rtos_timer_min_remaining(void)
{
    port_enter_critical();
    if (!g_timer_list) {
        port_exit_critical();
        return RTOS_WAIT_FOREVER;
    }
    rtos_tick_t now  = rtos_task_tick_count();
    int32_t     diff = (int32_t)(g_timer_list->abs_expiry_tick - now);
    port_exit_critical();
    return diff > 0 ? (rtos_tick_t)diff : 0;
}

//---------------------------------------------------------------------------
// Called from rtos_tick_handler() on every tick (core 0 only).
// now = current tick count at time of call.
// Pops and fires all timers whose absolute deadline has been reached.
// For tickless idle, now may be many ticks ahead of the previous call;
// periodic timers will fire once per missed period within the elapsed window.
//---------------------------------------------------------------------------
void rtos_timer_tick(rtos_tick_t now)
{
    while (g_timer_list && (int32_t)(now - g_timer_list->abs_expiry_tick) >= 0)
    {
        rtos_timer_t *cur = g_timer_list;
        g_timer_list = cur->next;
        cur->next    = NULL;

        RTOS_TRACE_TIMER_FIRE(cur);
        cur->cb((rtos_handle_t)cur);

        // Re-arm or deactivate. Check cur->active: the callback may have
        // called rtos_timer_stop(cur), in which case active is already 0.
        if (cur->active)
        {
            if (cur->periodic)
            {
                // Advance deadline by one period, skipping any missed periods
                // that elapsed during a tickless-idle suppression window.
                cur->abs_expiry_tick += cur->period_ticks;
                while ((int32_t)(now - cur->abs_expiry_tick) >= 0)
                    cur->abs_expiry_tick += cur->period_ticks;
                timer_list_insert_sorted(cur);
            } else {
                cur->active = 0;
                g_timer_count--;
            }
        }
    }
}

#endif // RTOS_ENABLE_SOFTWARE_TIMERS
