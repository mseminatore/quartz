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
// A timer callback MAY call rtos_timer_stop() or rtos_timer_reset() on the
// timer that is currently firing (i.e. on itself). Such re-entrant calls
// are safe: the post-callback re-arm/deactivate logic respects whatever
// state the callback left the timer in.
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

// Set to the timer whose callback is currently executing (NULL otherwise).
// Used to make rtos_timer_reset/start/stop re-entrant from inside a callback
// without corrupting g_timer_list or g_timer_count.
static rtos_timer_t *g_firing_timer    = NULL;
// Set to 1 when the firing timer's callback re-armed it via reset/start;
// the post-callback path uses this to avoid double-inserting / mis-counting.
static uint8_t       g_firing_rearmed  = 0;

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

    // Re-entrant call from within the timer's own callback: the timer is
    // currently outside the active list (was popped by rtos_timer_tick),
    // and g_timer_count was already decremented if the callback called stop
    // first. Mark it active and let the post-callback path re-insert it.
    if (timer == g_firing_timer)
    {
        timer->abs_expiry_tick = rtos_task_tick_count() + timer->period_ticks;
        timer->active          = 1;
        g_firing_rearmed       = 1;
        port_exit_critical();
        return;
    }

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

    // Re-entrant call from inside the timer's own callback: the timer was
    // already removed from g_timer_list before the callback ran, and
    // g_timer_count was not yet decremented. Just mark inactive and clear
    // any prior in-callback rearm so the post-callback logic skips it.
    if (timer == g_firing_timer)
    {
        timer->active    = 0;
        g_firing_rearmed = 0;
        port_exit_critical();
        return;
    }

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

    // Re-entrant call from inside the timer's own callback. The timer is
    // currently off-list; defer the re-insert to the post-callback path.
    if (timer == g_firing_timer)
    {
        timer->abs_expiry_tick = rtos_task_tick_count() + timer->period_ticks;
        timer->active          = 1;
        g_firing_rearmed       = 1;
        port_exit_critical();
        return;
    }

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

        // Mark this timer as currently firing so reset/start/stop calls from
        // within the callback are routed through the re-entrant path.
        g_firing_timer   = cur;
        g_firing_rearmed = 0;

        RTOS_TRACE_TIMER_FIRE(cur);
        cur->cb((rtos_handle_t)cur);

        uint8_t rearmed = g_firing_rearmed;
        g_firing_timer   = NULL;
        g_firing_rearmed = 0;

        if (rearmed)
        {
            // Callback called rtos_timer_reset(self) or rtos_timer_start(self).
            // Timer is still off the list and the active count was not
            // decremented when we popped it, so just re-insert.
            timer_list_insert_sorted(cur);
        }
        else if (cur->active)
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
        else
        {
            // Callback called rtos_timer_stop(self) — already inactive.
            // Decrement count to balance the popped one-shot.
            g_timer_count--;
        }
    }
}

#endif // RTOS_ENABLE_SOFTWARE_TIMERS
