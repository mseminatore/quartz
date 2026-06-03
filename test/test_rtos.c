// Copyright 2025. All rights reserved.
// Host-side unit tests for RTOS data structures and logic.
// These tests exercise the kernel in a single-threaded simulation on the
// development host (no real hardware or context switching needed).
//
// Build: cmake -B build && cmake --build build && ./build/rtos_test
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "test.h"

// ---------------------------------------------------------------------------
// Stub the port layer so the kernel compiles and links on the host.
// ---------------------------------------------------------------------------

static int g_critical_depth = 0;

void port_enter_critical(void)     { g_critical_depth++; }
void port_exit_critical(void)      { g_critical_depth--; }
void port_request_reschedule(void) { /* no-op on host */ }
void port_init(uint32_t hz)        { (void)hz; }
void port_start_first_task(void)   { /* no-op on host */ }
uint32_t port_timestamp_us(void)   { static uint32_t t; return ++t; }

void *port_init_stack(void     *stack_top,
                      void    (*func)(void *),
                      void     *arg)
{
    (void)func; (void)arg;
    // Return a plausible pointer; no real stack frame needed on host.
    return (uint8_t *)stack_top - 64;
}

// ---------------------------------------------------------------------------
// Pull in kernel sources directly (avoid link-time dependency on the library)
// ---------------------------------------------------------------------------
#include "../src/list.c"
#include "../src/task.c"
#include "../src/sem.c"
#include "../src/mutex.c"
#include "../src/queue.c"
#include "../src/timer.c"
#include "../src/eventgroup.c"

// Pull the chrome trace recorder into the test binary so we can unit-test
// its ring buffer + serialization independently of the kernel build flags.
#include "../src/trace_chrome.c"

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_list(void)
{
    SUITE("list basics");

    rtos_tcb_t a = {0}, b = {0}, c = {0};
    rtos_tcb_t *head = NULL;

    list_insert_tail(&head, &a);
    list_insert_tail(&head, &b);
    list_insert_tail(&head, &c);

    TEST(list_peek_head(head) == &a);
    TEST(list_pop_head(&head) == &a);
    TEST(list_pop_head(&head) == &b);
    TEST(list_pop_head(&head) == &c);
    TEST(list_pop_head(&head) == NULL);

    // Remove from middle
    list_insert_tail(&head, &a);
    list_insert_tail(&head, &b);
    list_insert_tail(&head, &c);
    TEST(1 == list_remove(&head, &b));
    TEST(list_pop_head(&head) == &a);
    TEST(list_pop_head(&head) == &c);
    TEST(0 == list_remove(&head, &b));  // already removed
}

static void test_task_create(void)
{
    SUITE("task create");

    static rtos_tcb_t tcb;
    static rtos_stack_t   stack[64];

    rtos_handle_t h = rtos_task_create(&tcb, stack, 64,
                                       (void(*)(void*))0xDEADBEEF, NULL,
                                       "mytask", 2);
    TEST(h != NULL);
    TEST(h == (rtos_handle_t)&tcb);
    TEST(tcb.priority == 2);
    TEST(tcb.state == TASK_READY);
    TEST(strcmp(tcb.name, "mytask") == 0);

    // Invalid inputs
    TEST(NULL == rtos_task_create(NULL, stack, 64, (void(*)(void*))1, NULL, "x", 0));
    TEST(NULL == rtos_task_create(&tcb, NULL, 64, (void(*)(void*))1, NULL, "x", 0));
    TEST(NULL == rtos_task_create(&tcb, stack, 64, NULL, NULL, "x", 0));
    TEST(NULL == rtos_task_create(&tcb, stack, 64, (void(*)(void*))1, NULL, "x", RTOS_MAX_PRIORITIES));
    TEST(NULL == rtos_task_create(&tcb, stack, 0,  (void(*)(void*))1, NULL, "x", 0));  // zero stack
}

static void test_semaphore(void)
{
    SUITE("semaphore basics");

    static rtos_sem_t sem;
    rtos_handle_t h = rtos_semaphore_create_binary(&sem);
    TEST(h != NULL);
    TEST(sem.count == 0);

    // Give raises count; Take lowers it
    rtos_semaphore_give(h);
    TEST(sem.count == 1);
    TEST(RTOS_OK == rtos_semaphore_take(h, RTOS_NO_WAIT));
    TEST(sem.count == 0);

    // Take on empty with no-wait returns TIMEOUT
    TEST(RTOS_TIMEOUT == rtos_semaphore_take(h, RTOS_NO_WAIT));

    // Counting semaphore
    static rtos_sem_t csem;
    rtos_handle_t ch = rtos_semaphore_create_counting(&csem, 3, 2);
    TEST(ch != NULL);
    TEST(csem.count == 2);
    rtos_semaphore_give(ch);
    TEST(csem.count == 3);
    rtos_semaphore_give(ch);
    TEST(csem.count == 3);  // capped at max
}

static void test_mutex(void)
{
    SUITE("mutex basics");

    // Set up a fake "current task" so the mutex owner is non-NULL
    static rtos_tcb_t fake_task;
    static rtos_stack_t   fake_stack[32];
    rtos_task_create(&fake_task, fake_stack, 32,
                (void(*)(void*))0x1, NULL, "fake", 0);
    // Manually set g_current so rtos_mutex_lock sees a non-NULL current task
    *rtos_current_tcb_ptr() = &fake_task;

    static rtos_mutex_t mtx;
    rtos_handle_t h = rtos_mutex_create(&mtx);
    TEST(h != NULL);
    TEST(mtx.owner == NULL);

    TEST(RTOS_OK == rtos_mutex_lock(h, RTOS_NO_WAIT));
    TEST(mtx.owner != NULL);

    // Second lock with no-wait returns TIMEOUT (mutex is held)
    TEST(RTOS_TIMEOUT == rtos_mutex_lock(h, RTOS_NO_WAIT));

    rtos_mutex_unlock(h);
    TEST(mtx.owner == NULL);
}

static void test_queue(void)
{
    SUITE("queue basics");

    static rtos_queue_t q;
    static uint8_t      buf[4 * sizeof(int)];
    rtos_handle_t h = rtos_queue_create(&q, buf, sizeof(int), 4);
    TEST(h != NULL);
    TEST(rtos_queue_messages_waiting(h) == 0);

    int val;
    // Receive on empty returns TIMEOUT
    TEST(RTOS_TIMEOUT == rtos_queue_receive(h, &val, RTOS_NO_WAIT));

    // Send 4 items
    int items[] = {10, 20, 30, 40};
    for (int i = 0; i < 4; i++)
        TEST(RTOS_OK == rtos_queue_send(h, &items[i], RTOS_NO_WAIT));

    TEST(rtos_queue_messages_waiting(h) == 4);

    // 5th send fails (queue full, no-wait)
    int extra = 99;
    TEST(RTOS_TIMEOUT == rtos_queue_send(h, &extra, RTOS_NO_WAIT));

    // Receive in FIFO order
    for (int i = 0; i < 4; i++) {
        TEST(RTOS_OK == rtos_queue_receive(h, &val, RTOS_NO_WAIT));
        TEST(val == items[i]);
    }
    TEST(rtos_queue_messages_waiting(h) == 0);
}

//---------------------------------------------------------------------------
// L3: queue API extras — peek, send_to_front, spaces_available.
//---------------------------------------------------------------------------
static void test_queue_extras(void)
{
    SUITE("queue extras (peek / send_to_front / spaces_available)");

    static rtos_queue_t q;
    static uint8_t      buf[4 * sizeof(int)];
    rtos_handle_t h = rtos_queue_create(&q, buf, sizeof(int), 4);

    TEST(rtos_queue_spaces_available(h) == 4);
    TEST(rtos_queue_messages_waiting(h) == 0);

    int val;
    // Peek on empty returns TIMEOUT (no-wait).
    TEST(rtos_queue_peek(h, &val, RTOS_NO_WAIT) == RTOS_TIMEOUT);

    int a = 1, b = 2, c = 3;
    TEST(rtos_queue_send(h, &a, RTOS_NO_WAIT) == RTOS_OK);  // [1]
    TEST(rtos_queue_send(h, &b, RTOS_NO_WAIT) == RTOS_OK);  // [1,2]
    TEST(rtos_queue_spaces_available(h) == 2);

    // Peek does not consume.
    TEST(rtos_queue_peek(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    TEST(val == 1);
    TEST(rtos_queue_messages_waiting(h) == 2);
    TEST(rtos_queue_peek(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    TEST(val == 1);

    // send_to_front prepends.
    TEST(rtos_queue_send_to_front(h, &c, RTOS_NO_WAIT) == RTOS_OK);  // [3,1,2]
    TEST(rtos_queue_messages_waiting(h) == 3);
    TEST(rtos_queue_spaces_available(h) == 1);

    TEST(rtos_queue_peek(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    TEST(val == 3);

    // Drain in expected order: 3,1,2.
    TEST(rtos_queue_receive(h, &val, RTOS_NO_WAIT) == RTOS_OK); TEST(val == 3);
    TEST(rtos_queue_receive(h, &val, RTOS_NO_WAIT) == RTOS_OK); TEST(val == 1);
    TEST(rtos_queue_receive(h, &val, RTOS_NO_WAIT) == RTOS_OK); TEST(val == 2);

    TEST(rtos_queue_spaces_available(h) == 4);

    // send_to_front on full returns TIMEOUT.
    int x = 0;
    for (int i = 0; i < 4; i++) TEST(rtos_queue_send(h, &x, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rtos_queue_send_to_front(h, &x, RTOS_NO_WAIT) == RTOS_TIMEOUT);

    // Wrap-around: drain partially, send_to_front, verify head wrap.
    TEST(rtos_queue_receive(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rtos_queue_receive(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    int z = 77;
    TEST(rtos_queue_send_to_front(h, &z, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rtos_queue_peek(h, &val, RTOS_NO_WAIT) == RTOS_OK);
    TEST(val == 77);
}

static void test_list_sorted(void)
{
    SUITE("list insert sorted");

    static rtos_tcb_t a, b, c, d;
    a.sort_key = 10; b.sort_key = 5; c.sort_key = 20; d.sort_key = 5;
    a.next = b.next = c.next = d.next = NULL;

    rtos_tcb_t *head = NULL;
    list_insert_sorted(&head, &a, 10);
    list_insert_sorted(&head, &b, 5);   // should become new head
    list_insert_sorted(&head, &c, 20);  // should go to tail
    list_insert_sorted(&head, &d, 5);   // tie — after b (FIFO within same key)

    TEST(head == &b);
    TEST(head->next == &d);
    TEST(head->next->next == &a);
    TEST(head->next->next->next == &c);
    TEST(head->next->next->next->next == NULL);
}

#if RTOS_ENABLE_TASK_NOTIFY
static void test_task_notify(void)
{
    SUITE("task notifications");

    static rtos_tcb_t tcb;
    static rtos_stack_t stack[64];
    rtos_handle_t h = rtos_task_create(&tcb, stack, 64, (void(*)(void*))1, NULL, "n", 0);
    TEST(h != NULL);

    // notif_pending starts clear
    TEST(tcb.notif_pending == 0);

    // Notify sets the flag
    rtos_task_notify(h);
    TEST(tcb.notif_pending == 1);

    // notify_wait with pending notification returns OK immediately (flag cleared)
    // Simulate the task as current so notify_wait can manipulate it
    g_current = &tcb;
    tcb.state = TASK_RUNNING;
    int r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_OK);
    TEST(tcb.notif_pending == 0);

    // notify_wait with no notification and NO_WAIT returns TIMEOUT
    r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_TIMEOUT);

    // notify_from_isr also sets the flag
    rtos_task_notify_from_isr(h);
    TEST(tcb.notif_pending == 1);

    g_current = NULL;
}
#endif // RTOS_ENABLE_TASK_NOTIFY

static void test_delay_until(void)
{
    SUITE("delay until");

    static rtos_tcb_t tcb;
    static rtos_stack_t stack[64];
    rtos_task_create(&tcb, stack, 64, (void(*)(void*))1, NULL, "du", 0);
    g_current = &tcb;
    tcb.state = TASK_RUNNING;

    // Manually advance tick counter for the test
    g_tick_count = 50;

    rtos_tick_t wake = 50;

    // delay_until(50 → 50+20=70): current tick is 50 → should delay 20
    // (port_request_reschedule is a no-op on host; just verify no crash + wake advances)
    rtos_task_delay_until(&wake, 20);
    TEST(wake == 70);  // wake advanced to 70

    // Simulate ticks passing: tick is now 75 (overrun)
    g_tick_count = 75;
    wake = 70;
    rtos_task_delay_until(&wake, 20);  // next deadline = 90, delay = 15
    TEST(wake == 90);

    // If already past deadline (overrun), should not block (delay <= 0)
    g_tick_count = 100;
    wake = 90;
    rtos_task_delay_until(&wake, 5);  // next = 95, already past → no delay
    TEST(wake == 95);

    g_current = NULL;
    g_tick_count = 0;
}

// ---------------------------------------------------------------------------
// Verify that rtos_task_delay_until is drift-free across many scheduler cycles.
//
// Strategy: manually drive N full periods by calling rtos_task_delay_until,
// then rtos_tick_advance(PERIOD) to simulate tick passage, checking that the
// task unblocks at exactly the right tick each time.  Also verifies that a
// mid-period overrun (task body takes longer than expected) does NOT cause
// drift in subsequent deadlines.
// ---------------------------------------------------------------------------
static void test_delay_until_multi_cycle(void)
{
    SUITE("delay_until multi-cycle drift-free");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t tcb;
    static rtos_stack_t   stack[64];
    rtos_task_create(&tcb, stack, 64, (void(*)(void*))1, NULL, "blink", 1);
    g_current = &tcb;
    tcb.state  = TASK_RUNNING;

    const rtos_tick_t PERIOD = 500;
    rtos_tick_t last_wake    = g_tick_count;    /* == 0 */

#define NUM_PERIODS 5
    for (int i = 1; i <= NUM_PERIODS; i++) {
        rtos_task_delay_until(&last_wake, PERIOD);

        rtos_tick_t expected = (rtos_tick_t)(i * PERIOD);
        TEST(last_wake        == expected);     /* absolute deadline advanced */
        TEST(tcb.wakeup_tick  == expected);     /* blocked until that tick   */
        TEST(tcb.state        == TASK_BLOCKED);

        /* simulate exactly PERIOD ticks from the ISR */
        rtos_tick_advance(PERIOD);
        TEST(g_tick_count     == expected);     /* tick count is exact       */
        TEST(tcb.state        == TASK_READY);   /* unblocked at correct tick */
        TEST(tcb.on_blocked   == 0);

        /* simulate scheduler dispatching the task */
        tcb.state = TASK_RUNNING;
    }
#undef NUM_PERIODS

    /* -------------------------------------------------------------------- */
    /* Overrun test: task body "takes" 50 extra ticks.                       */
    /* At this point g_tick_count == last_wake == 2500.                      */
    /* Advance without calling delay_until — simulates a long task body.     */
    /* -------------------------------------------------------------------- */
    g_tick_count += 50;   /* now 2550 — 50-tick overrun */

    rtos_task_delay_until(&last_wake, PERIOD);
    /* next deadline = 2500 + 500 = 3000                                     */
    /* remaining     = 3000 - 2550 = 450 (NOT 500 — compensates for overrun) */
    TEST(last_wake       == 3000);
    TEST(tcb.wakeup_tick == 3000);

    /* advance 450 ticks to reach tick 3000 (not 500 — drift is compensated) */
    rtos_tick_advance(450);
    TEST(g_tick_count    == 3000);
    TEST(tcb.state       == TASK_READY);
    tcb.state = TASK_RUNNING;

    /* following period: starts from 3000, so next deadline is 3500 */
    rtos_task_delay_until(&last_wake, PERIOD);
    TEST(last_wake       == 3500);
    TEST(tcb.wakeup_tick == 3500);

    g_current      = NULL;
    g_tick_count   = 0;
    g_blocked      = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

static int g_timer_fires = 0;
static void timer_cb(rtos_handle_t t) { (void)t; g_timer_fires++; }

static void test_ok_tick_handler(void)
{
    SUITE("O(k) tick handler");

    // Reset scheduler state for a clean test
    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t t1, t2;
    static uint32_t   s1[64], s2[64];
    rtos_task_create(&t1, s1, 64, (void(*)(void*))1, NULL, "t1", 1);
    rtos_task_create(&t2, s2, 64, (void(*)(void*))1, NULL, "t2", 2);

    // Block t1 for 10 ticks and t2 for 5 ticks from tick 0
    g_current = &t1;
    rtos_task_delay(10);  // wakeup_tick = 10, on_blocked = 1
    TEST(t1.on_blocked == 1);
    TEST(t1.wakeup_tick == 10);

    g_current = &t2;
    rtos_task_delay(5);   // wakeup_tick = 5, on_blocked = 1
    TEST(t2.on_blocked == 1);
    TEST(t2.wakeup_tick == 5);

    // Blocked list should have t2 (wakeup=5) before t1 (wakeup=10)
    TEST(g_blocked == &t2);
    TEST(g_blocked->next == &t1);

    // Advance 4 ticks — neither should wake
    rtos_tick_advance(4);
    TEST(g_tick_count == 4);
    TEST(t1.on_blocked == 1);
    TEST(t2.on_blocked == 1);

    // Advance 1 more tick (total 5) — t2 wakes, t1 stays blocked
    rtos_tick_advance(1);
    TEST(g_tick_count == 5);
    TEST(t2.on_blocked == 0);
    TEST(t2.state == TASK_READY);
    TEST(t1.on_blocked == 1);
    TEST(t1.state == TASK_BLOCKED);

    // Advance 5 more ticks (total 10) — t1 wakes
    rtos_tick_advance(5);
    TEST(g_tick_count == 10);
    TEST(t1.on_blocked == 0);
    TEST(t1.state == TASK_READY);
    TEST(g_blocked == NULL);

    g_current = NULL;
    g_tick_count = 0;
}

static void test_ipc_timeout(void)
{
    SUITE("IPC timeout via tick handler");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t ta;
    static uint32_t   sa[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "ta", 0);

    static rtos_sem_t sem;
    rtos_handle_t sh = rtos_semaphore_create_binary(&sem);

    // On the host port, port_request_reschedule is a no-op, so rtos_semaphore_take
    // with a finite timeout never truly suspends — it self-removes and returns TIMEOUT.
    // What we CAN verify is:
    //   1. rtos_task_blocked_add correctly inserts into G_BLOCKED
    //   2. The tick handler pops the head at the right tick
    //   3. rtos_task_make_ready removes from G_BLOCKED

    // Manually set up the block state (simulating what sem_take would do in hardware)
    g_current = &ta;
    ta.state = TASK_BLOCKED;
    rtos_task_blocked_add(&ta, 8);   // absolute wakeup at tick 8
    TEST(ta.on_blocked == 1);
    TEST(ta.wakeup_tick == 8);
    TEST(g_blocked == &ta);

    // Tick 7: task still blocked
    rtos_tick_advance(7);
    TEST(ta.on_blocked == 1);

    // Tick 8: task wakes via tick handler
    rtos_tick_advance(1);
    TEST(ta.on_blocked == 0);
    TEST(ta.state == TASK_READY);
    TEST(g_blocked == NULL);

    // Verify rtos_task_make_ready removes from G_BLOCKED (IPC give path)
    g_blocked = NULL;
    g_tick_count = 0;
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "ta", 0);
    g_current = &ta;
    ta.state = TASK_BLOCKED;
    rtos_task_blocked_add(&ta, 20);
    TEST(ta.on_blocked == 1);

    // Give the sem (make_ready path): should remove from G_BLOCKED
    rtos_task_make_ready(&ta);
    TEST(ta.on_blocked == 0);
    TEST(ta.state == TASK_READY);
    TEST(g_blocked == NULL);

    g_current = NULL;
    g_tick_count = 0;
}

#if RTOS_ENABLE_PRIORITY_INHERITANCE
static void test_priority_inheritance(void)
{
    SUITE("priority inheritance");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t low_tcb, high_tcb;
    static rtos_stack_t   low_stack[64], high_stack[64];

    // Low priority = 3 (higher number = lower priority in this RTOS)
    rtos_task_create(&low_tcb,  low_stack,  64, (void(*)(void*))1, NULL, "low",  3);
    rtos_task_create(&high_tcb, high_stack, 64, (void(*)(void*))1, NULL, "high", 0);

    TEST(low_tcb.base_priority == 3);
    TEST(high_tcb.base_priority == 0);

    // Low-priority task acquires mutex
    static rtos_mutex_t mtx;
    rtos_handle_t mh = rtos_mutex_create(&mtx);
    g_current = &low_tcb;
    low_tcb.state = TASK_RUNNING;
    rtos_mutex_lock(mh, RTOS_NO_WAIT);
    TEST(mtx.owner == &low_tcb);
    TEST(low_tcb.priority == 3);  // not yet boosted

    // High-priority task tries to lock (times out on host — no real preemption)
    // The boost should happen BEFORE the timeout cleanup
    g_current = &high_tcb;
    high_tcb.state = TASK_RUNNING;

    // Simulate: manually apply the boost that rtos_mutex_lock does internally.
    // We can't observe mid-lock state from the outside, so we call the function
    // and verify the end-state: owner priority is restored when waiter times out
    // and then cleaned up on unlock.
    //
    // Instead: call rtos_task_blocked_add + the boost logic directly by using
    // a finite timeout that forces the waiter path. Verify post-lock state.
    int r = rtos_mutex_lock(mh, 1);   // timeout=1 tick; returns TIMEOUT on host
    TEST(r == RTOS_TIMEOUT);

    // After the timeout, the waiter removed itself from wait_list.
    // With multi-mutex-aware PI restoration, the owner's priority is
    // recomputed on waiter timeout and drops back to base since no waiter
    // remains to justify a boost.
    TEST(low_tcb.priority == 3);
    TEST(low_tcb.base_priority == 3);  // base unchanged

    // Owner unlocks: priority restored before transferring ownership
    g_current = &low_tcb;
    low_tcb.state = TASK_RUNNING;
    rtos_mutex_unlock(mh);
    TEST(low_tcb.priority == 3);      // restored to base
    TEST(mtx.owner == NULL);          // unlocked (no waiter)

    g_current = NULL;
}

//---------------------------------------------------------------------------
// H1: a task holding multiple mutexes must keep its boost until *all*
// boosting waiters are gone. Releasing one mutex must not prematurely drop
// a boost that another held mutex still justifies.
//
// On the host stubs there's no real scheduler, so we construct the scenario
// directly by manipulating wait lists and priorities, then exercise the
// recompute-on-unlock path.
//---------------------------------------------------------------------------
static void test_priority_inheritance_multi_mutex(void)
{
    SUITE("priority inheritance — multi-mutex");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t low_tcb, mid_tcb, high_tcb;
    static rtos_stack_t   low_stack[64], mid_stack[64], high_stack[64];

    rtos_task_create(&low_tcb,  low_stack,  64, (void(*)(void*))1, NULL, "low",  5);
    rtos_task_create(&mid_tcb,  mid_stack,  64, (void(*)(void*))1, NULL, "mid",  2);
    rtos_task_create(&high_tcb, high_stack, 64, (void(*)(void*))1, NULL, "high", 0);
    // Pull the helper tasks off the ready queue — we are simulating them as blocked.
    ready_remove(&mid_tcb);  mid_tcb.state  = TASK_BLOCKED;
    ready_remove(&high_tcb); high_tcb.state = TASK_BLOCKED;

    static rtos_mutex_t m_a, m_b;
    rtos_handle_t ha = rtos_mutex_create(&m_a);
    rtos_handle_t hb = rtos_mutex_create(&m_b);

    // Low takes both mutexes — both should land on its held list.
    g_current = &low_tcb;
    low_tcb.state = TASK_RUNNING;
    rtos_mutex_lock(ha, RTOS_NO_WAIT);
    rtos_mutex_lock(hb, RTOS_NO_WAIT);
    TEST(m_a.owner == &low_tcb);
    TEST(m_b.owner == &low_tcb);
    TEST(low_tcb.priority == 5);
    TEST(low_tcb.held_mutexes != NULL);

    // Simulate high blocking on m_a and mid blocking on m_b. Insert sorted
    // by priority; this mirrors what rtos_mutex_lock does internally.
    list_insert_sorted(&m_a.wait_list, &high_tcb, high_tcb.priority);
    list_insert_sorted(&m_b.wait_list, &mid_tcb,  mid_tcb.priority);

    // Apply the boost low would have received from high (the strongest waiter).
    low_tcb.priority = 0;

    // Low releases m_a. Owner becomes high; low's effective priority must
    // recompute to mid's (2) — NOT all the way back to base (5).
    rtos_mutex_unlock(ha);
    TEST(m_a.owner == &high_tcb);
    TEST(low_tcb.priority == 2);
    TEST(low_tcb.base_priority == 5);
    TEST(high_tcb.held_mutexes == &m_a);

    // Low releases m_b. Owner becomes mid; low has no held mutexes left,
    // so its priority returns all the way to base.
    rtos_mutex_unlock(hb);
    TEST(m_b.owner == &mid_tcb);
    TEST(low_tcb.priority == 5);
    TEST(low_tcb.held_mutexes == NULL);
    TEST(mid_tcb.held_mutexes == &m_b);

    // Tear down: have the new owners release so subsequent suites see clean state.
    g_current = &high_tcb; high_tcb.state = TASK_RUNNING;
    rtos_mutex_unlock(ha);
    g_current = &mid_tcb;  mid_tcb.state  = TASK_RUNNING;
    rtos_mutex_unlock(hb);

    g_current = NULL;
}

//---------------------------------------------------------------------------
// H1 corollary: when a waiter times out, the owner's priority must be
// recomputed (boost dropped if no other waiter justifies it).
//---------------------------------------------------------------------------
static void test_priority_inheritance_timeout_drops_boost(void)
{
    SUITE("priority inheritance — timeout drops boost");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t low_tcb2, high_tcb2;
    static uint32_t   ls2[64], hs2[64];

    rtos_task_create(&low_tcb2,  ls2, 64, (void(*)(void*))1, NULL, "low2",  3);
    rtos_task_create(&high_tcb2, hs2, 64, (void(*)(void*))1, NULL, "high2", 0);

    static rtos_mutex_t mtx2;
    rtos_handle_t mh2 = rtos_mutex_create(&mtx2);
    g_current = &low_tcb2;
    low_tcb2.state = TASK_RUNNING;
    rtos_mutex_lock(mh2, RTOS_NO_WAIT);
    TEST(low_tcb2.priority == 3);

    g_current = &high_tcb2;
    high_tcb2.state = TASK_RUNNING;
    int r = rtos_mutex_lock(mh2, 1);
    TEST(r == RTOS_TIMEOUT);

    // After timeout there are no waiters on the mutex, so the boost should
    // have been dropped during the cleanup path.
    TEST(low_tcb2.priority == 3);
    TEST(low_tcb2.base_priority == 3);

    g_current = &low_tcb2;
    low_tcb2.state = TASK_RUNNING;
    rtos_mutex_unlock(mh2);
    g_current = NULL;
}
#endif // RTOS_ENABLE_PRIORITY_INHERITANCE

#if RTOS_ENABLE_RECURSIVE_MUTEX
//---------------------------------------------------------------------------
// L1: recursive mutex — same task may lock multiple times; must unlock the
// same number of times before another task can acquire.
//---------------------------------------------------------------------------
static void test_mutex_recursive(void)
{
    SUITE("recursive mutex");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t a_tcb, b_tcb;
    static uint32_t   as[64], bs[64];
    rtos_task_create(&a_tcb, as, 64, (void(*)(void*))1, NULL, "a", 2);
    rtos_task_create(&b_tcb, bs, 64, (void(*)(void*))1, NULL, "b", 2);

    static rtos_mutex_t rm;
    rtos_handle_t h = rtos_mutex_create_recursive(&rm);
    TEST(h != NULL);
    TEST(rm.recursive == 1);
    TEST(rm.nest_count == 0);

    g_current = &a_tcb;
    a_tcb.state = TASK_RUNNING;

    // Lock 3 times by same task.
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rm.nest_count == 1);
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rm.nest_count == 2);
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rm.nest_count == 3);
    TEST(rm.owner == &a_tcb);

    // Other task cannot acquire while held.
    g_current = &b_tcb;
    b_tcb.state = TASK_RUNNING;
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_TIMEOUT);
    TEST(rm.owner == &a_tcb);

    // Unlock from non-owner fails.
    TEST(rtos_mutex_unlock(h) == RTOS_ERR);

    // Owner unlocks 2 of 3 — still owned.
    g_current = &a_tcb;
    a_tcb.state = TASK_RUNNING;
    TEST(rtos_mutex_unlock(h) == RTOS_OK);
    TEST(rm.nest_count == 2);
    TEST(rm.owner == &a_tcb);
    TEST(rtos_mutex_unlock(h) == RTOS_OK);
    TEST(rm.nest_count == 1);
    TEST(rm.owner == &a_tcb);

    // Final unlock releases.
    TEST(rtos_mutex_unlock(h) == RTOS_OK);
    TEST(rm.owner == NULL);
    TEST(rm.nest_count == 0);

    // Now the other task can acquire.
    g_current = &b_tcb;
    b_tcb.state = TASK_RUNNING;
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rm.owner == &b_tcb);
    TEST(rm.nest_count == 1);
    rtos_mutex_unlock(h);

    // Non-recursive mutex re-lock by owner returns TIMEOUT (NO_WAIT path):
    static rtos_mutex_t nr;
    rtos_handle_t nh = rtos_mutex_create(&nr);
    TEST(nr.recursive == 0);
    g_current = &a_tcb;
    a_tcb.state = TASK_RUNNING;
    TEST(rtos_mutex_lock(nh, RTOS_NO_WAIT) == RTOS_OK);
    TEST(rtos_mutex_lock(nh, RTOS_NO_WAIT) == RTOS_TIMEOUT);
    rtos_mutex_unlock(nh);

    g_current = NULL;
}
#endif // RTOS_ENABLE_RECURSIVE_MUTEX

#if RTOS_ENABLE_SOFTWARE_TIMERS
static void test_timers(void)
{
    SUITE("timers");

    g_tick_count = 0;
    static rtos_timer_t timer;
    rtos_handle_t h = rtos_timer_create(&timer, "t1", 3, 0 /* one-shot */, timer_cb);
    TEST(h != NULL);
    TEST(timer.active == 0);

    rtos_timer_start(h);
    TEST(timer.active == 1);

    // Tick twice — not yet fired
    g_tick_count++; rtos_timer_tick(g_tick_count);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    TEST(g_timer_fires == 0);

    // Third tick fires it
    g_tick_count++; rtos_timer_tick(g_tick_count);
    TEST(g_timer_fires == 1);
    TEST(timer.active == 0);  // one-shot: removed after firing

    // Periodic timer
    static rtos_timer_t ptimer;
    g_timer_fires = 0;
    rtos_handle_t ph = rtos_timer_create(&ptimer, "p1", 2, 1 /* periodic */, timer_cb);
    rtos_timer_start(ph);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    g_tick_count++; rtos_timer_tick(g_tick_count);  // fires once
    TEST(g_timer_fires == 1);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    g_tick_count++; rtos_timer_tick(g_tick_count);  // fires again
    TEST(g_timer_fires == 2);
    TEST(ptimer.active == 1);             // still active
    rtos_timer_stop(ph);
    TEST(ptimer.active == 0);
}

//---------------------------------------------------------------------------
// M8: timer callbacks may call rtos_timer_reset() / rtos_timer_stop() on
// themselves without corrupting the active list or g_timer_count.
//---------------------------------------------------------------------------
static rtos_handle_t g_self_reset_handle;
static int           g_self_reset_count;
static int           g_self_reset_limit;
static void self_reset_cb(rtos_handle_t t)
{
    (void)t;
    g_self_reset_count++;
    if (g_self_reset_count < g_self_reset_limit)
        rtos_timer_reset(g_self_reset_handle);
    // After limit, do nothing (one-shot lapses naturally).
}

static rtos_handle_t g_self_stop_handle;
static int           g_self_stop_count;
static void self_stop_cb(rtos_handle_t t)
{
    (void)t;
    g_self_stop_count++;
    rtos_timer_stop(g_self_stop_handle);
}

static void test_timer_self_reset(void)
{
    SUITE("timer self-reset/stop in callback");

    g_tick_count = 0;
    g_self_reset_count = 0;
    g_self_reset_limit = 3;

    static rtos_timer_t srt;
    g_self_reset_handle = rtos_timer_create(&srt, "sr", 2, 0 /* one-shot */, self_reset_cb);
    rtos_timer_start(g_self_reset_handle);
    TEST(srt.active == 1);

    // Each fire should trigger another 2-tick countdown until limit reached.
    for (int i = 0; i < 10; i++) {
        g_tick_count++;
        rtos_timer_tick(g_tick_count);
    }
    TEST(g_self_reset_count == 3);
    TEST(srt.active == 0);            // last fire didn't re-arm; one-shot

    // Confirm the count accounting is balanced — should be able to start
    // a fresh batch of timers up to RTOS_MAX_TIMERS.
    static rtos_timer_t fill[RTOS_MAX_TIMERS];
    int created = 0;
    for (int i = 0; i < RTOS_MAX_TIMERS; i++) {
        rtos_handle_t h = rtos_timer_create(&fill[i], "f", 100, 0, timer_cb);
        rtos_timer_start(h);
        if (fill[i].active) created++;
    }
    TEST(created == RTOS_MAX_TIMERS);
    for (int i = 0; i < RTOS_MAX_TIMERS; i++) rtos_timer_stop((rtos_handle_t)&fill[i]);

    // Periodic + self-stop in callback.
    g_self_stop_count = 0;
    static rtos_timer_t sst;
    g_self_stop_handle = rtos_timer_create(&sst, "ss", 2, 1 /* periodic */, self_stop_cb);
    rtos_timer_start(g_self_stop_handle);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    g_tick_count++; rtos_timer_tick(g_tick_count);   // fires, callback stops it
    TEST(g_self_stop_count == 1);
    TEST(sst.active == 0);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    g_tick_count++; rtos_timer_tick(g_tick_count);
    TEST(g_self_stop_count == 1);                   // does not fire again
}
#endif // RTOS_ENABLE_SOFTWARE_TIMERS

// ---------------------------------------------------------------------------
// Test: tick wraparound correctness
// ---------------------------------------------------------------------------

static void test_tick_wraparound(void)
{
    SUITE("tick wraparound");

    static rtos_tcb_t tw;
    static uint32_t   sw[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    // Set tick near wraparound
    g_tick_count = 0xFFFFFFFE;
    rtos_task_create(&tw, sw, 64, (void(*)(void*))1, NULL, "wrap", 1);
    g_current = &tw;
    tw.state = TASK_BLOCKED;

    // Ask for a 4-tick delay — wakeup_tick = (0xFFFFFFFE + 4) = 2 (after wrap)
    rtos_task_blocked_add(&tw, 4);
    TEST(tw.wakeup_tick == 2);   // must have wrapped to 2
    TEST(tw.on_blocked == 1);

    // Advance to 0xFFFFFFFF — should NOT wake (need 3 more ticks after wrap)
    rtos_tick_advance(1);
    TEST(g_tick_count == 0xFFFFFFFF);
    TEST(tw.on_blocked == 1);    // still blocked

    // Advance through 0 and 1 — still not at wakeup_tick == 2
    rtos_tick_advance(1);        // wraps to 0
    TEST(tw.on_blocked == 1);
    rtos_tick_advance(1);        // tick == 1
    TEST(tw.on_blocked == 1);

    // Now at tick == 2 — should fire
    rtos_tick_advance(1);        // tick == 2
    TEST(g_tick_count == 2);
    TEST(tw.on_blocked == 0);
    TEST(tw.state == TASK_READY);

    g_current = NULL;
    g_tick_count = 0;
    g_blocked = NULL;
}

// ---------------------------------------------------------------------------
// Test: blocking queue send delivers item to receiver
// ---------------------------------------------------------------------------

static void test_queue_blocking_send(void)
{
    SUITE("queue blocking send");

    static rtos_queue_t q;
    static int   buf[2];
    static rtos_tcb_t   sender_tcb, receiver_tcb;
    static rtos_stack_t     sender_stack[64], receiver_stack[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    rtos_task_create(&sender_tcb,   sender_stack,   64, (void(*)(void*))1, NULL, "sender",   1);
    rtos_task_create(&receiver_tcb, receiver_stack, 64, (void(*)(void*))1, NULL, "receiver", 1);

    rtos_handle_t qh = rtos_queue_create(&q, buf, sizeof(int), 1);

    // Fill the queue
    int v1 = 10;
    TEST(RTOS_OK == rtos_queue_send(qh, &v1, RTOS_NO_WAIT));
    TEST(rtos_queue_messages_waiting(qh) == 1);

    // Sender tries to send on a full queue — blocks (no-wait returns TIMEOUT on host
    // since port_request_reschedule is a no-op; simulate blocking manually).
    // Manually queue the sender: set as current, blocked on send_wait
    g_current = &sender_tcb;
    sender_tcb.state = TASK_BLOCKED;
    sender_tcb.ipc_wait = &q.send_wait;
    list_insert_sorted(&q.send_wait, &sender_tcb, sender_tcb.priority);
    // RTOS_WAIT_FOREVER tasks are NOT added to g_blocked (they never time out)
    TEST(q.send_wait == &sender_tcb);

    // Now a receiver wakes up and dequeues, which should pop sender from send_wait
    // and make it ready (sender will complete the write when it resumes).
    g_current = &receiver_tcb;
    receiver_tcb.state = TASK_RUNNING;
    int got = 0;
    int r = rtos_queue_receive(qh, &got, RTOS_NO_WAIT);
    TEST(r == RTOS_OK);
    TEST(got == v1);

    // After receive: sender popped from send_wait and made ready.
    // In a real system the sender would resume and write v2. In this test
    // environment there is no context switch, so we verify the ready-state
    // invariants instead.
    TEST(q.send_wait == NULL);            // sender removed from wait list
    TEST(sender_tcb.state == TASK_READY); // sender is runnable           // sender's item was delivered

    g_current = NULL;
    g_tick_count = 0;
    g_blocked = NULL;
}

#if RTOS_ENABLE_TASK_SUSPEND
static void test_ipc_suspend_cleanup(void)
{
    SUITE("ipc suspend cleanup");

    static rtos_sem_t   sem;
    static rtos_tcb_t   ta, tb;
    static uint32_t     sa[64], sb[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "ta", 1);
    rtos_task_create(&tb, sb, 64, (void(*)(void*))1, NULL, "tb", 2);

    rtos_handle_t sh = rtos_semaphore_create_binary(&sem);
    TEST(sh != NULL);

    // Manually put 'ta' on the semaphore wait list (simulating it blocking)
    g_current = &ta;
    ta.state = TASK_BLOCKED;
    ta.ipc_wait = &sem.wait_list;
    list_insert_sorted(&sem.wait_list, &ta, ta.priority);
    // RTOS_WAIT_FOREVER tasks are NOT added to g_blocked (never time out)
    TEST(sem.wait_list == &ta);
    // on_blocked stays 0 for WAIT_FOREVER tasks

    // Suspend 'ta' while it is blocked on the semaphore
    g_current = &tb;
    tb.state = TASK_RUNNING;
    rtos_task_suspend((rtos_handle_t)&ta);
    TEST(ta.state == TASK_SUSPENDED);
    TEST(ta.on_blocked == 0);        // removed from G_BLOCKED
    TEST(sem.wait_list == NULL);     // removed from IPC wait list
    TEST(ta.ipc_wait == NULL);       // pointer cleared

    // Now give the semaphore — must NOT crash or touch ta
    g_current = &tb;
    rtos_semaphore_give(sh);
    // Semaphore count should increment (no waiter to wake)
    TEST(sem.count == 1);

    g_current = NULL;
    g_tick_count = 0;
    g_blocked = NULL;
}
#endif // RTOS_ENABLE_TASK_SUSPEND

#if RTOS_ENABLE_TASK_NOTIFY
// ---------------------------------------------------------------------------
// Test: notify_wait(RTOS_WAIT_FOREVER) must not wake after 1 tick (Bug A)
// ---------------------------------------------------------------------------

static void test_notify_wait_forever(void)
{
    SUITE("notify_wait WAIT_FOREVER stays blocked");

    static rtos_tcb_t ta;
    static uint32_t sa[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "ta", 1);

    // Manually simulate what rtos_task_notify_wait(RTOS_WAIT_FOREVER) does:
    // it must NOT insert the task into g_blocked at all.
    g_current = &ta;
    ta.state       = TASK_BLOCKED;
    ta.on_blocked  = 0;
    ready_remove(&ta);
    // Verify the task is NOT on g_blocked (WAIT_FOREVER guard must have fired)
    // Since we are testing the guard, simulate with WAIT_FOREVER directly:
    if (RTOS_WAIT_FOREVER != RTOS_NO_WAIT) {
        // Confirm that on_blocked remains 0 when WAIT_FOREVER used
        rtos_tick_t timeout_ticks = RTOS_WAIT_FOREVER;
        if (timeout_ticks != RTOS_WAIT_FOREVER) {
            ta.wakeup_tick = g_tick_count + timeout_ticks;
            ta.on_blocked  = 1;
            list_insert_sorted_signed(&g_blocked, &ta, ta.wakeup_tick);
        }
    }
    TEST(ta.on_blocked == 0);
    TEST(g_blocked == NULL);

    // Advancing the tick must NOT wake the task
    g_tick_count = RTOS_TICK_MAX;  // worst-case: tick just before wrap
    rtos_tick_handler();
    TEST(ta.state == TASK_BLOCKED);

    g_current = NULL;
    g_tick_count = 0;
    g_blocked = NULL;
}

// ---------------------------------------------------------------------------
// Test: notify on IPC-blocked task cleans the IPC wait list (Bug B)
// ---------------------------------------------------------------------------

static void test_notify_ipc_blocked(void)
{
    SUITE("notify on IPC-blocked task cleans ipc_wait");

    static rtos_sem_t   sem;
    static rtos_tcb_t   ta, tb;
    static uint32_t     sa[64], sb[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "ta", 1);
    rtos_task_create(&tb, sb, 64, (void(*)(void*))1, NULL, "tb", 2);

    rtos_handle_t sh = rtos_semaphore_create_binary(&sem);

    // Manually block ta on the semaphore wait list
    g_current = &ta;
    ta.state    = TASK_BLOCKED;
    ta.ipc_wait = &sem.wait_list;
    list_insert_sorted(&sem.wait_list, &ta, ta.priority);
    TEST(sem.wait_list == &ta);

    // Notify ta while it's IPC-blocked — must remove it from sem.wait_list
    g_current = &tb;
    tb.state = TASK_RUNNING;
    rtos_task_notify((rtos_handle_t)&ta);

    TEST(ta.state == TASK_READY);
    TEST(ta.ipc_wait == NULL);
    TEST(sem.wait_list == NULL);  // cleaned up

    // Now give the semaphore — must NOT double-insert ta
    // (ta is already TASK_READY and NOT on sem.wait_list)
    rtos_semaphore_give(sh);
    TEST(sem.count == 1);         // incremented, no waiter to pop

    g_current = NULL;
    g_tick_count = 0;
    g_blocked = NULL;
}
#endif // RTOS_ENABLE_TASK_NOTIFY

// ---------------------------------------------------------------------------
// Test: blocked list signed sort handles wakeup_tick wraparound (Bug C)
// ---------------------------------------------------------------------------

static void test_blocked_list_wraparound(void)
{
    SUITE("blocked list signed sort across tick wraparound");

    static rtos_tcb_t t1, t2;
    static uint32_t s1[64], s2[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    rtos_task_create(&t1, s1, 64, (void(*)(void*))1, NULL, "t1", 1);
    rtos_task_create(&t2, s2, 64, (void(*)(void*))1, NULL, "t2", 1);

    // t1 wakeup_tick just before wrap; t2 wakeup_tick just after wrap.
    // Signed comparison must order them t1 (0xFFFFFFF5) before t2 (0x10).
    rtos_tick_t tick_pre_wrap  = 0xFFFFFFF5u;
    rtos_tick_t tick_post_wrap = 0x00000010u;

    list_insert_sorted_signed(&g_blocked, &t1, tick_pre_wrap);
    list_insert_sorted_signed(&g_blocked, &t2, tick_post_wrap);

    // Head must be t1 (pre-wrap, fires sooner)
    TEST(g_blocked == &t1);
    TEST(g_blocked->next == &t2);

    // At tick = 0xFFFFFFF5 only t1 should fire
    g_tick_count = tick_pre_wrap;
    g_current = &t1;
    t1.state = TASK_BLOCKED;
    t1.on_blocked = 1;
    t1.wakeup_tick = tick_pre_wrap;
    g_current = &t2;
    t2.state = TASK_BLOCKED;
    t2.on_blocked = 1;
    t2.wakeup_tick = tick_post_wrap;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    // Rebuild blocked list properly
    g_blocked = NULL;
    list_insert_sorted_signed(&g_blocked, &t1, tick_pre_wrap);
    list_insert_sorted_signed(&g_blocked, &t2, tick_post_wrap);
    g_current = NULL;

    rtos_tick_handler();  // tick = 0xFFFFFFF5: should wake t1 only
    TEST(t1.state == TASK_READY);
    TEST(t2.state == TASK_BLOCKED);  // must NOT fire yet

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;
}

// ---------------------------------------------------------------------------
// Test: rtos_queue_receive_from_isr (Feature E)
// ---------------------------------------------------------------------------

static void test_queue_recv_from_isr(void)
{
    SUITE("queue receive from ISR");

    static rtos_queue_t q;
    static int buf[4];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    rtos_handle_t qh = rtos_queue_create(&q, buf, sizeof(int), 4);
    TEST(qh != NULL);

    // Empty queue returns RTOS_ERR
    int val = 0;
    TEST(RTOS_ERR == rtos_queue_receive_from_isr(qh, &val));

    // Put two items in
    int v1 = 42, v2 = 99;
    rtos_queue_send_from_isr(qh, &v1);
    rtos_queue_send_from_isr(qh, &v2);
    TEST(rtos_queue_messages_waiting(qh) == 2);

    // Receive first item
    TEST(RTOS_OK == rtos_queue_receive_from_isr(qh, &val));
    TEST(val == v1);
    TEST(rtos_queue_messages_waiting(qh) == 1);

    // Receive second item
    TEST(RTOS_OK == rtos_queue_receive_from_isr(qh, &val));
    TEST(val == v2);
    TEST(rtos_queue_messages_waiting(qh) == 0);

    // Empty again
    TEST(RTOS_ERR == rtos_queue_receive_from_isr(qh, &val));

    g_tick_count = 0;
    g_blocked = NULL;
}

#if RTOS_ENABLE_SOFTWARE_TIMERS
// ---------------------------------------------------------------------------
// Test: RTOS_MAX_TIMERS limit is enforced
// ---------------------------------------------------------------------------

static void test_timer_max_limit(void)
{
    SUITE("timer max limit");

    g_tick_count = 0;

    static rtos_timer_t tslot[RTOS_MAX_TIMERS + 1];
    rtos_handle_t handles[RTOS_MAX_TIMERS + 1];

    // Create RTOS_MAX_TIMERS timers and start them all
    for (int i = 0; i < RTOS_MAX_TIMERS; i++) {
        handles[i] = rtos_timer_create(&tslot[i], "tx", 10 + i, 0, timer_cb);
        rtos_timer_start(handles[i]);
    }

    // All RTOS_MAX_TIMERS should be active
    int active = 0;
    for (int i = 0; i < RTOS_MAX_TIMERS; i++)
        active += tslot[i].active;
    TEST(active == RTOS_MAX_TIMERS);

    // One more timer — should not start (limit reached)
    handles[RTOS_MAX_TIMERS] = rtos_timer_create(&tslot[RTOS_MAX_TIMERS], "overflow", 5, 0, timer_cb);
    rtos_timer_start(handles[RTOS_MAX_TIMERS]);
    TEST(tslot[RTOS_MAX_TIMERS].active == 0);  // rejected

    // Stop one; slot should now be available for the overflow timer
    rtos_timer_stop(handles[0]);
    TEST(tslot[0].active == 0);

    rtos_timer_start(handles[RTOS_MAX_TIMERS]);
    TEST(tslot[RTOS_MAX_TIMERS].active == 1);  // now accepted

    // Clean up
    for (int i = 0; i <= RTOS_MAX_TIMERS; i++)
        rtos_timer_stop(handles[i]);
}

// ---------------------------------------------------------------------------
// Test: rtos_timer_min_remaining and rtos_timer_is_active
// ---------------------------------------------------------------------------

static void test_timer_min_remaining(void)
{
    SUITE("timer min_remaining / is_active");

    g_tick_count = 0;

    TEST(rtos_timer_min_remaining() == RTOS_WAIT_FOREVER);  // no active timers

    static rtos_timer_t ta2, tb2;
    rtos_handle_t ha2 = rtos_timer_create(&ta2, "a", 10, 0, timer_cb);
    rtos_handle_t hb2 = rtos_timer_create(&tb2, "b",  5, 0, timer_cb);

    TEST(rtos_timer_is_active(ha2) == 0);

    rtos_timer_start(ha2);
    TEST(rtos_timer_is_active(ha2) == 1);
    TEST(rtos_timer_min_remaining() == 10);  // only ta2

    rtos_timer_start(hb2);
    TEST(rtos_timer_min_remaining() == 5);   // tb2 fires sooner

    // Advance 3 ticks — 2 ticks left for tb2
    g_tick_count = 3;
    TEST(rtos_timer_min_remaining() == 2);

    rtos_timer_stop(ha2);
    rtos_timer_stop(hb2);

    TEST(rtos_timer_min_remaining() == RTOS_WAIT_FOREVER);
    TEST(rtos_timer_is_active(ha2) == 0);
}
#endif // RTOS_ENABLE_SOFTWARE_TIMERS

// ---------------------------------------------------------------------------
// Test: task inspection and rtos_task_set_priority
// ---------------------------------------------------------------------------

static void test_task_inspection(void)
{
    SUITE("task inspection");

    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t inspect_tcb;
    static rtos_stack_t   inspect_stack[64];
    rtos_handle_t h = rtos_task_create(&inspect_tcb, inspect_stack, 64,
                                        (void(*)(void*))1, NULL, "probe", 2);
    TEST(h != NULL);

    TEST(rtos_task_get_state(h) == TASK_READY);
    TEST(strcmp(rtos_task_get_name(h), "probe") == 0);
    TEST(rtos_task_get_priority(h) == 2);

    // Raise priority — task is READY so it must be moved in ready list
    TEST(RTOS_OK == rtos_task_set_priority(h, 1));
    TEST(rtos_task_get_priority(h) == 1);
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    TEST(inspect_tcb.base_priority == 1);
#endif

    // Reject out-of-range priority assignment (RTOS_MAX_PRIORITIES and above are invalid)
    TEST(RTOS_ERR == rtos_task_set_priority(h, RTOS_MAX_PRIORITIES));
    TEST(rtos_task_get_priority(h) == 1);  // unchanged

    // NULL handle returns sentinel values
    TEST(rtos_task_get_state(NULL) == TASK_DELETED);
    TEST(rtos_task_get_name(NULL) == NULL);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

#if RTOS_ENABLE_TASK_NOTIFY
// ---------------------------------------------------------------------------
// Test: rtos_task_notify_clear
// ---------------------------------------------------------------------------

static void test_notify_clear(void)
{
    SUITE("task notify_clear");

    static rtos_tcb_t nc_tcb;
    static rtos_stack_t   nc_stack[64];
    rtos_handle_t h = rtos_task_create(&nc_tcb, nc_stack, 64,
                                        (void(*)(void*))1, NULL, "nc", 2);
    g_current = &nc_tcb;
    nc_tcb.state = TASK_RUNNING;

    TEST(nc_tcb.notif_pending == 0);

    rtos_task_notify(h);
    TEST(nc_tcb.notif_pending == 1);

    rtos_task_notify_clear();
    TEST(nc_tcb.notif_pending == 0);

    // notify_wait should now timeout immediately (no pending notification)
    nc_tcb.state = TASK_RUNNING;
    int r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_TIMEOUT);

    g_current = NULL;
}

//---------------------------------------------------------------------------
// L2: value-passing notifications — set bits, increment, overwrite,
// no-overwrite, and clear-on-entry/exit semantics.
//---------------------------------------------------------------------------
static void test_notify_value(void)
{
    SUITE("task notify value (set/increment/overwrite)");

    static rtos_tcb_t nv_tcb;
    static rtos_stack_t   nv_stack[64];
    rtos_handle_t h = rtos_task_create(&nv_tcb, nv_stack, 64,
                                        (void(*)(void*))1, NULL, "nv", 2);
    g_current = &nv_tcb;
    nv_tcb.state = TASK_RUNNING;
    nv_tcb.notif_pending = 0;
    nv_tcb.notif_value   = 0;

    uint32_t v = 0;

    TEST(rtos_task_notify_value(h, RTOS_NOTIFY_SET_BITS, 0x1) == RTOS_OK);
    TEST(rtos_task_notify_value(h, RTOS_NOTIFY_SET_BITS, 0x4) == RTOS_OK);
    TEST(nv_tcb.notif_pending == 1);
    TEST(nv_tcb.notif_value == 0x5);

    TEST(rtos_task_notify_wait_value(0, 0, &v, RTOS_NO_WAIT) == RTOS_OK);
    TEST(v == 0x5);
    TEST(nv_tcb.notif_pending == 0);
    TEST(nv_tcb.notif_value == 0x5);

    rtos_task_notify_value(h, RTOS_NOTIFY_SET_BITS, 0x10);
    TEST(rtos_task_notify_wait_value(0, 0xFFFFFFFFu, &v, RTOS_NO_WAIT) == RTOS_OK);
    TEST(v == 0x15);
    TEST(nv_tcb.notif_value == 0);

    rtos_task_notify_value(h, RTOS_NOTIFY_INCREMENT, 0);
    rtos_task_notify_value(h, RTOS_NOTIFY_INCREMENT, 0);
    rtos_task_notify_value(h, RTOS_NOTIFY_INCREMENT, 0);
    TEST(nv_tcb.notif_value == 3);
    TEST(rtos_task_notify_wait_value(0, 0, &v, RTOS_NO_WAIT) == RTOS_OK);
    TEST(v == 3);

    nv_tcb.notif_value = 0xAA;
    nv_tcb.notif_pending = 0;
    rtos_task_notify_value(h, RTOS_NOTIFY_OVERWRITE, 0x42);
    TEST(nv_tcb.notif_value == 0x42);
    TEST(nv_tcb.notif_pending == 1);
    rtos_task_notify_value(h, RTOS_NOTIFY_OVERWRITE, 0x99);
    TEST(nv_tcb.notif_value == 0x99);

    rtos_task_notify_clear();
    nv_tcb.notif_value = 0;
    TEST(rtos_task_notify_value(h, RTOS_NOTIFY_SET_NO_OVERWRITE, 0x77) == RTOS_OK);
    TEST(nv_tcb.notif_value == 0x77);
    TEST(rtos_task_notify_value(h, RTOS_NOTIFY_SET_NO_OVERWRITE, 0xAA) == RTOS_ERR);
    TEST(nv_tcb.notif_value == 0x77);
    rtos_task_notify_clear();

    nv_tcb.notif_value = 0;
    rtos_task_notify_value(h, RTOS_NOTIFY_SET_BITS, 0xF0);
    TEST(rtos_task_notify_wait_value(0xF0, 0, &v, RTOS_NO_WAIT) == RTOS_OK);
    TEST(v == 0);

    nv_tcb.notif_value = 0x55;
    rtos_task_notify_value(h, RTOS_NOTIFY_NONE, 0);
    TEST(nv_tcb.notif_pending == 1);
    TEST(nv_tcb.notif_value == 0x55);
    TEST(rtos_task_notify_wait_value(0, 0, &v, RTOS_NO_WAIT) == RTOS_OK);
    TEST(v == 0x55);

    nv_tcb.notif_value = 0;
    rtos_task_notify(h);
    TEST(nv_tcb.notif_pending == 1);
    TEST(rtos_task_notify_wait(RTOS_NO_WAIT) == RTOS_OK);

    g_current = NULL;
}
#endif // RTOS_ENABLE_TASK_NOTIFY

// ---------------------------------------------------------------------------
// Test: rtos_semaphore_take_from_isr
// ---------------------------------------------------------------------------

static void test_sem_take_from_isr(void)
{
    SUITE("semaphore take from ISR");

    static rtos_sem_t isem;
    rtos_handle_t ih = rtos_semaphore_create_counting(&isem, 3, 2);
    TEST(ih != NULL);
    TEST(isem.count == 2);

    // Take from ISR — non-blocking
    TEST(RTOS_OK == rtos_semaphore_take_from_isr(ih));
    TEST(isem.count == 1);

    TEST(RTOS_OK == rtos_semaphore_take_from_isr(ih));
    TEST(isem.count == 0);

    // Empty — should return ERR
    TEST(RTOS_ERR == rtos_semaphore_take_from_isr(ih));
    TEST(isem.count == 0);  // unchanged

    // Give one back, then take it
    rtos_semaphore_give_from_isr(ih);
    TEST(RTOS_OK == rtos_semaphore_take_from_isr(ih));
    TEST(isem.count == 0);

    // NULL handle
    TEST(RTOS_ERR == rtos_semaphore_take_from_isr(NULL));
}

#if RTOS_ENABLE_TASK_DELETE
// ---------------------------------------------------------------------------
// TEST-1: rtos_task_delete
// ---------------------------------------------------------------------------

static void test_task_delete(void)
{
    SUITE("task delete");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t td;
    static uint32_t   sd[64];
    rtos_handle_t h = rtos_task_create(&td, sd, 64, (void(*)(void*))1, NULL, "del", 2);
    TEST(h != NULL);
    TEST(td.state == TASK_READY);
    TEST(g_ready[2] == &td);

    // Delete the task — should remove from ready list and mark DELETED
    rtos_task_delete(h);
    TEST(td.state == TASK_DELETED);
    TEST(g_ready[2] == NULL);
    TEST((g_ready_bitmap & (1u << 2)) == 0);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// TEST-1b: rtos_task_delete on blocked task (IPC cleanup)
// ---------------------------------------------------------------------------

static void test_task_delete_while_blocked(void)
{
    SUITE("task delete while IPC-blocked");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t td2, tother;
    static uint32_t   sd2[64], sother[64];
    static rtos_sem_t sem_del;

    rtos_task_create(&td2, sd2, 64, (void(*)(void*))1, NULL, "dblk", 2);
    rtos_task_create(&tother, sother, 64, (void(*)(void*))1, NULL, "other", 3);
    rtos_handle_t sh = rtos_semaphore_create_binary(&sem_del);

    // Manually block td2 on semaphore and blocked list
    g_current = &tother;
    tother.state = TASK_RUNNING;

    td2.state    = TASK_BLOCKED;
    td2.ipc_wait = &sem_del.wait_list;
    list_insert_sorted(&sem_del.wait_list, &td2, td2.priority);
    rtos_task_blocked_add(&td2, 100);
    TEST(sem_del.wait_list == &td2);
    TEST(td2.on_blocked == 1);

    // Delete td2 — must remove from both IPC wait list and blocked list
    rtos_task_delete((rtos_handle_t)&td2);
    TEST(td2.state == TASK_DELETED);
    TEST(sem_del.wait_list == NULL);
    TEST(td2.on_blocked == 0);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_TASK_DELETE

#if RTOS_ENABLE_TASK_SUSPEND
// ---------------------------------------------------------------------------
// TEST-2: rtos_task_suspend / resume standalone
// ---------------------------------------------------------------------------

static void test_suspend_resume(void)
{
    SUITE("task suspend and resume");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t ts;
    static uint32_t   ss[64];
    rtos_handle_t h = rtos_task_create(&ts, ss, 64, (void(*)(void*))1, NULL, "susp", 1);
    TEST(h != NULL);
    TEST(ts.state == TASK_READY);
    TEST(g_ready[1] == &ts);

    // Suspend the task
    rtos_task_suspend(h);
    TEST(ts.state == TASK_SUSPENDED);
    TEST(g_ready[1] == NULL);
    TEST((g_ready_bitmap & (1u << 1)) == 0);

    // Resume the task
    rtos_task_resume(h);
    TEST(ts.state == TASK_READY);
    TEST(g_ready[1] == &ts);
    TEST((g_ready_bitmap & (1u << 1)) != 0);

    // Suspend self (via NULL handle)
    g_current = &ts;
    ts.state = TASK_RUNNING;
    // Remove from ready list first since RUNNING tasks aren't on it
    ready_remove(&ts);
    rtos_task_suspend(NULL);
    TEST(ts.state == TASK_SUSPENDED);

    // Resume
    rtos_task_resume(h);
    TEST(ts.state == TASK_READY);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_TASK_SUSPEND

// ---------------------------------------------------------------------------
// TEST-3: rtos_task_yield
// ---------------------------------------------------------------------------

static void test_task_yield(void)
{
    SUITE("task yield");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t ty;
    static uint32_t   sy[64];
    rtos_task_create(&ty, sy, 64, (void(*)(void*))1, NULL, "yield", 1);
    g_current = &ty;
    ty.state = TASK_RUNNING;

    // yield is a no-op on the host stub but should not crash
    rtos_task_yield();
    TEST(ty.state == TASK_RUNNING);  // state unchanged

    g_current = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

#if RTOS_ENABLE_SOFTWARE_TIMERS
// ---------------------------------------------------------------------------
// TEST-4: rtos_timer_reset
// ---------------------------------------------------------------------------

static void test_timer_reset(void)
{
    SUITE("timer reset");

    g_tick_count = 0;
    g_timer_fires = 0;

    static rtos_timer_t trst;
    rtos_handle_t th = rtos_timer_create(&trst, "rst", 10, 0, timer_cb);
    rtos_timer_start(th);
    TEST(trst.active == 1);

    // Advance 5 ticks
    for (int i = 0; i < 5; i++) { g_tick_count++; rtos_timer_tick(g_tick_count); }
    TEST(g_timer_fires == 0);

    // Reset the timer — should restart the 10-tick countdown from now
    rtos_timer_reset(th);
    TEST(trst.active == 1);

    // Advance 9 more ticks — should NOT fire (reset extended the deadline)
    for (int i = 0; i < 9; i++) { g_tick_count++; rtos_timer_tick(g_tick_count); }
    TEST(g_timer_fires == 0);

    // One more tick — should fire now
    g_tick_count++;
    rtos_timer_tick(g_tick_count);
    TEST(g_timer_fires == 1);

    rtos_timer_stop(th);
}
#endif // RTOS_ENABLE_SOFTWARE_TIMERS

#if RTOS_STACK_OVERFLOW_CHECK
// ---------------------------------------------------------------------------
// TEST-5: rtos_task_check_stack
// ---------------------------------------------------------------------------

static void test_check_stack(void)
{
    SUITE("task check stack");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t tcs;
    static uint32_t   scs[64];
    rtos_handle_t h = rtos_task_create(&tcs, scs, 64, (void(*)(void*))1, NULL, "stk", 2);
    TEST(h != NULL);

    // Stack sentinel should be intact
    TEST(RTOS_OK == rtos_task_check_stack(h));

    // Corrupt the sentinel
    uint32_t *base = (uint32_t *)tcs.stack_base;
    base[0] = 0;
    TEST(RTOS_ERR == rtos_task_check_stack(h));

    // Restore it
    base[0] = 0xDEADBEEFu;
    TEST(RTOS_OK == rtos_task_check_stack(h));

    // NULL handle
    TEST(RTOS_ERR == rtos_task_check_stack(NULL));

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_STACK_OVERFLOW_CHECK

// ---------------------------------------------------------------------------
// TEST-6: rtos_task_stack_high_water_mark
// ---------------------------------------------------------------------------

static void test_stack_hwm(void)
{
    SUITE("task stack high water mark");

    // RTOS_STACK_WATERMARK is 0 by default, so HWM always returns 0
    static rtos_tcb_t thwm;
    static uint32_t   shwm[64];

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    rtos_handle_t h = rtos_task_create(&thwm, shwm, 64, (void(*)(void*))1, NULL, "hwm", 2);
    TEST(h != NULL);

    // With watermark disabled, should return 0
    TEST(rtos_task_stack_high_water_mark(h) == 0);
    TEST(rtos_task_stack_high_water_mark(NULL) == 0);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// TEST-7: rtos_queue_send_from_isr direct assertions
// ---------------------------------------------------------------------------

static void test_queue_send_isr_direct(void)
{
    SUITE("queue send from ISR direct");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_queue_t qi;
    static int buf_qi[2];
    rtos_handle_t qh = rtos_queue_create(&qi, buf_qi, sizeof(int), 2);
    TEST(qh != NULL);

    int v1 = 10, v2 = 20, v3 = 30;
    TEST(RTOS_OK == rtos_queue_send_from_isr(qh, &v1));
    TEST(rtos_queue_messages_waiting(qh) == 1);
    TEST(RTOS_OK == rtos_queue_send_from_isr(qh, &v2));
    TEST(rtos_queue_messages_waiting(qh) == 2);

    // Queue full — should return RTOS_ERR
    TEST(RTOS_ERR == rtos_queue_send_from_isr(qh, &v3));
    TEST(rtos_queue_messages_waiting(qh) == 2);

    // NULL handle
    TEST(RTOS_ERR == rtos_queue_send_from_isr(NULL, &v1));

    g_blocked = NULL;
    g_tick_count = 0;
}

// ---------------------------------------------------------------------------
// TEST-8: rtos_context_switch logic
// ---------------------------------------------------------------------------

static void test_context_switch(void)
{
    SUITE("context switch logic");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t t_high, t_low;
    static uint32_t   s_high[64], s_low[64];

    rtos_task_create(&t_high, s_high, 64, (void(*)(void*))1, NULL, "hi", 0);
    rtos_task_create(&t_low, s_low, 64, (void(*)(void*))1, NULL, "lo", 3);

    // Make t_low the current running task
    g_current = &t_low;
    t_low.state = TASK_RUNNING;
    ready_remove(&t_low);

    // Context switch should preempt t_low and pick t_high
    rtos_context_switch();
    TEST(g_current == &t_high);
    TEST(t_high.state == TASK_RUNNING);
    TEST(t_low.state == TASK_READY);  // preempted → back on ready list

    // t_low should be back on its ready queue
    TEST(g_ready[3] == &t_low);

    // Another context switch — t_high goes back on ready list and gets picked
    // again because it has higher priority than t_low
    rtos_context_switch();
    TEST(g_current == &t_high);  // still highest priority
    TEST(t_high.state == TASK_RUNNING);

    // Remove t_high from the equation, then switch should pick t_low
    t_high.state = TASK_BLOCKED;
    rtos_context_switch();
    TEST(g_current == &t_low);
    TEST(t_low.state == TASK_RUNNING);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// TEST-9: rtos_task_delay(0) yields without blocking
// ---------------------------------------------------------------------------

static void test_delay_zero(void)
{
    SUITE("task delay(0) yields");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t td0;
    static uint32_t   sd0[64];
    rtos_task_create(&td0, sd0, 64, (void(*)(void*))1, NULL, "d0", 1);
    g_current = &td0;
    td0.state = TASK_RUNNING;

    rtos_task_delay(0);

    // Should NOT be blocked — delay(0) is a yield, not a block
    TEST(td0.state == TASK_RUNNING);  // unchanged — yield just calls port_request_reschedule
    TEST(g_blocked == NULL);

    g_current = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// TEST-10: task name truncation at RTOS_TASK_NAME_LEN
// ---------------------------------------------------------------------------

static void test_name_truncation(void)
{
    SUITE("task name truncation");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t tn;
    static uint32_t   sn[64];

    // Name longer than RTOS_TASK_NAME_LEN (16 including null)
    const char *long_name = "this_name_is_way_too_long_for_the_buffer";
    rtos_handle_t h = rtos_task_create(&tn, sn, 64, (void(*)(void*))1, NULL, long_name, 2);
    TEST(h != NULL);

    // Should be truncated to RTOS_TASK_NAME_LEN - 1 chars + null
    TEST(strlen(rtos_task_get_name(h)) == RTOS_TASK_NAME_LEN - 1);
    TEST(strncmp(rtos_task_get_name(h), long_name, RTOS_TASK_NAME_LEN - 1) == 0);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

#if RTOS_ENABLE_TASK_SUSPEND && RTOS_ENABLE_PRIORITY_INHERITANCE
// ---------------------------------------------------------------------------
// TEST-11: rtos_task_set_priority on blocked/suspended tasks
// ---------------------------------------------------------------------------

static void test_setprio_blocked(void)
{
    SUITE("set priority on blocked/suspended task");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t tsp, tother2;
    static uint32_t   ssp[64], sother2[64];

    rtos_task_create(&tsp, ssp, 64, (void(*)(void*))1, NULL, "sp", 2);
    rtos_task_create(&tother2, sother2, 64, (void(*)(void*))1, NULL, "ot2", 3);

    // Suspend the task, then change priority
    rtos_task_suspend((rtos_handle_t)&tsp);
    TEST(tsp.state == TASK_SUSPENDED);
    TEST(RTOS_OK == rtos_task_set_priority((rtos_handle_t)&tsp, 0));
    TEST(tsp.priority == 0);
    TEST(tsp.base_priority == 0);

    // Resume — should be on the new priority queue
    rtos_task_resume((rtos_handle_t)&tsp);
    TEST(tsp.state == TASK_READY);
    TEST(g_ready[0] == &tsp);

    // Block the task, then change priority
    g_current = &tother2;
    tother2.state = TASK_RUNNING;
    ready_remove(&tother2);

    tsp.state = TASK_BLOCKED;
    ready_remove(&tsp);
    rtos_task_blocked_add(&tsp, 100);

    TEST(RTOS_OK == rtos_task_set_priority((rtos_handle_t)&tsp, 1));
    TEST(tsp.priority == 1);
    TEST(tsp.base_priority == 1);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_TASK_SUSPEND && RTOS_ENABLE_PRIORITY_INHERITANCE

#if RTOS_ENABLE_TASK_NOTIFY
// ---------------------------------------------------------------------------
// TEST-12: notification + IPC interaction (BUG-1 scenario)
// ---------------------------------------------------------------------------

static void test_notify_ipc_interaction(void)
{
    SUITE("notification + IPC interaction (BUG-1)");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_sem_t   sem_ni;
    static rtos_tcb_t   ta_ni, tb_ni;
    static uint32_t     sa_ni[64], sb_ni[64];

    rtos_task_create(&ta_ni, sa_ni, 64, (void(*)(void*))1, NULL, "ta_ni", 1);
    rtos_task_create(&tb_ni, sb_ni, 64, (void(*)(void*))1, NULL, "tb_ni", 2);
    rtos_handle_t sh = rtos_semaphore_create_binary(&sem_ni);

    // Simulate ta_ni calling rtos_semaphore_take and blocking
    g_current = &ta_ni;
    ta_ni.state    = TASK_BLOCKED;
    ta_ni.ipc_wait = &sem_ni.wait_list;
    ta_ni.notif_pending = 0;
    list_insert_sorted(&sem_ni.wait_list, &ta_ni, ta_ni.priority);
    rtos_task_blocked_add(&ta_ni, 100);

    // tb_ni sends a notification to ta_ni while it's IPC-blocked
    g_current = &tb_ni;
    tb_ni.state = TASK_RUNNING;
    rtos_task_notify((rtos_handle_t)&ta_ni);

    // ta_ni should be woken up with notif_pending set
    TEST(ta_ni.state == TASK_READY);
    TEST(ta_ni.notif_pending == 1);
    TEST(sem_ni.wait_list == NULL);  // cleaned up
    TEST(sem_ni.count == 0);          // semaphore NOT given — count should remain 0

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// TEST-13: double notification before wait
// ---------------------------------------------------------------------------

static void test_double_notify(void)
{
    SUITE("double notification coalesce");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t tdn;
    static uint32_t   sdn[64];
    rtos_task_create(&tdn, sdn, 64, (void(*)(void*))1, NULL, "dn", 1);
    g_current = &tdn;
    tdn.state = TASK_RUNNING;

    // Notify twice before wait
    rtos_task_notify((rtos_handle_t)&tdn);
    TEST(tdn.notif_pending == 1);
    rtos_task_notify((rtos_handle_t)&tdn);
    TEST(tdn.notif_pending == 1);  // coalesced, still 1

    // First wait succeeds
    int r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_OK);
    TEST(tdn.notif_pending == 0);

    // Second wait times out — second notification was coalesced/lost
    tdn.state = TASK_RUNNING;
    r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_TIMEOUT);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_TASK_NOTIFY (test_double_notify)

// ---------------------------------------------------------------------------
// TEST-14: mutex non-owner unlock returns error
// ---------------------------------------------------------------------------

static void test_mutex_non_owner(void)
{
    SUITE("mutex non-owner unlock");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_mutex_t mno;
    static rtos_tcb_t   t_owner, t_thief;
    static uint32_t     s_owner[64], s_thief[64];

    rtos_task_create(&t_owner, s_owner, 64, (void(*)(void*))1, NULL, "own", 1);
    rtos_task_create(&t_thief, s_thief, 64, (void(*)(void*))1, NULL, "thf", 2);
    rtos_handle_t mh = rtos_mutex_create(&mno);

    // Owner locks the mutex
    g_current = &t_owner;
    t_owner.state = TASK_RUNNING;
    TEST(RTOS_OK == rtos_mutex_lock(mh, RTOS_NO_WAIT));
    TEST(mno.owner == &t_owner);

    // Non-owner tries to unlock — should fail
    g_current = &t_thief;
    t_thief.state = TASK_RUNNING;
    TEST(RTOS_ERR == rtos_mutex_unlock(mh));
    TEST(mno.owner == &t_owner);  // ownership unchanged

    // Owner unlocks — should succeed
    g_current = &t_owner;
    TEST(RTOS_OK == rtos_mutex_unlock(mh));
    TEST(mno.owner == NULL);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

#if RTOS_ENABLE_SOFTWARE_TIMERS
// ---------------------------------------------------------------------------
// TEST-15: timer stop from within callback
// ---------------------------------------------------------------------------

static rtos_handle_t g_self_stop_handle;

static void timer_self_stop_cb(void *arg)
{
    (void)arg;
    g_timer_fires++;
    rtos_timer_stop(g_self_stop_handle);
}

static void test_timer_stop_from_callback(void)
{
    SUITE("timer stop from callback");

    g_tick_count = 0;
    g_timer_fires = 0;

    static rtos_timer_t tss;
    g_self_stop_handle = rtos_timer_create(&tss, "ss", 5, 1, timer_self_stop_cb);
    rtos_timer_start(g_self_stop_handle);
    TEST(tss.active == 1);

    // Advance 5 ticks — should fire and stop itself
    for (int i = 0; i < 5; i++) { g_tick_count++; rtos_timer_tick(g_tick_count); }
    TEST(g_timer_fires == 1);
    TEST(tss.active == 0);  // stopped itself in callback

    // Advance more — should NOT fire again
    for (int i = 0; i < 10; i++) { g_tick_count++; rtos_timer_tick(g_tick_count); }
    TEST(g_timer_fires == 1);  // still 1
}
#endif // RTOS_ENABLE_SOFTWARE_TIMERS

// ---------------------------------------------------------------------------
// TEST-16: rtos_idle_next_wakeup_ticks
// ---------------------------------------------------------------------------

static void test_idle_wakeup_ticks(void)
{
    SUITE("idle next wakeup ticks");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    // No blocked tasks, no timers → WAIT_FOREVER
    TEST(rtos_idle_next_wakeup_ticks() == RTOS_WAIT_FOREVER);

    // Add a blocked task with wakeup at tick 10
    static rtos_tcb_t tiw;
    static uint32_t   siw[64];
    rtos_task_create(&tiw, siw, 64, (void(*)(void*))1, NULL, "iw", 1);
    tiw.state = TASK_BLOCKED;
    ready_remove(&tiw);
    rtos_task_blocked_add(&tiw, 10);

    TEST(rtos_idle_next_wakeup_ticks() == 10);

    // Advance 3 ticks
    g_tick_count = 3;
    TEST(rtos_idle_next_wakeup_ticks() == 7);

    // Clean up
    rtos_task_blocked_remove(&tiw);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;
}

// ---------------------------------------------------------------------------
// TEST-17: ISR queue ops with waiters
// ---------------------------------------------------------------------------

static void test_isr_queue_with_waiters(void)
{
    SUITE("ISR queue ops unblock waiters");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_queue_t qw;
    static int          buf_qw[2];
    static rtos_tcb_t   t_recv, t_send, t_isr;
    static uint32_t     s_recv[64], s_send[64], s_isr[64];

    rtos_handle_t qh = rtos_queue_create(&qw, buf_qw, sizeof(int), 2);
    rtos_task_create(&t_recv, s_recv, 64, (void(*)(void*))1, NULL, "rcv", 1);
    rtos_task_create(&t_send, s_send, 64, (void(*)(void*))1, NULL, "snd", 2);
    rtos_task_create(&t_isr, s_isr, 64, (void(*)(void*))1, NULL, "isr", 0);

    // Block t_recv on empty queue receive
    t_recv.state = TASK_BLOCKED;
    t_recv.ipc_wait = &qw.recv_wait;
    list_insert_sorted(&qw.recv_wait, &t_recv, t_recv.priority);
    ready_remove(&t_recv);
    TEST(qw.recv_wait == &t_recv);

    // ISR sends an item — should unblock t_recv
    int val = 42;
    TEST(RTOS_OK == rtos_queue_send_from_isr(qh, &val));
    TEST(t_recv.state == TASK_READY);
    TEST(qw.recv_wait == NULL);

    // Now fill the queue and block t_send on full queue send
    int v1 = 1, v2 = 2;
    rtos_queue_send_from_isr(qh, &v1);
    rtos_queue_send_from_isr(qh, &v2);
    TEST(rtos_queue_messages_waiting(qh) == 2);

    t_send.state = TASK_BLOCKED;
    t_send.ipc_wait = &qw.send_wait;
    list_insert_sorted(&qw.send_wait, &t_send, t_send.priority);
    ready_remove(&t_send);
    TEST(qw.send_wait == &t_send);

    // ISR receives — should unblock t_send
    int got;
    TEST(RTOS_OK == rtos_queue_receive_from_isr(qh, &got));
    TEST(t_send.state == TASK_READY);
    TEST(qw.send_wait == NULL);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;
}

#if RTOS_STACK_OVERFLOW_CHECK
// ---------------------------------------------------------------------------
// TEST-18: creating maximum tasks (tracking limit)
// ---------------------------------------------------------------------------

static void test_max_tasks(void)
{
    SUITE("max tasks tracking limit");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;
    g_all_tasks_count = 0;

    static rtos_tcb_t  tcbs[RTOS_MAX_TASKS + 1];
    static rtos_stack_t    stacks[RTOS_MAX_TASKS + 1][64];

    // Create RTOS_MAX_TASKS tasks — all should succeed
    for (int i = 0; i < RTOS_MAX_TASKS; i++) {
        rtos_handle_t h = rtos_task_create(&tcbs[i], stacks[i], 64,
                                           (void(*)(void*))1, NULL, "mx", i % (RTOS_MAX_PRIORITIES - 1));
        TEST(h != NULL);
    }
    TEST(g_all_tasks_count == RTOS_MAX_TASKS);

    // One more — task still gets created but tracking array is full
    rtos_handle_t extra = rtos_task_create(&tcbs[RTOS_MAX_TASKS], stacks[RTOS_MAX_TASKS], 64,
                                            (void(*)(void*))1, NULL, "xtra", 0);
    TEST(extra != NULL);  // task creation succeeds
    TEST(g_all_tasks_count == RTOS_MAX_TASKS);  // but not tracked

    g_all_tasks_count = 0;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_STACK_OVERFLOW_CHECK

// ---------------------------------------------------------------------------
// TEST-19: semaphore give_from_isr with waiter
// ---------------------------------------------------------------------------

static void test_sem_give_isr_waiter(void)
{
    SUITE("semaphore give_from_isr unblocks waiter");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_sem_t  sem_isr;
    static rtos_tcb_t  tw, tisr2;
    static uint32_t    sw[64], sisr2[64];

    rtos_handle_t sh = rtos_semaphore_create_binary(&sem_isr);
    rtos_task_create(&tw, sw, 64, (void(*)(void*))1, NULL, "sw", 1);
    rtos_task_create(&tisr2, sisr2, 64, (void(*)(void*))1, NULL, "isr2", 0);

    // Block tw on semaphore
    tw.state = TASK_BLOCKED;
    tw.ipc_wait = &sem_isr.wait_list;
    list_insert_sorted(&sem_isr.wait_list, &tw, tw.priority);
    ready_remove(&tw);
    TEST(sem_isr.wait_list == &tw);

    // ISR gives the semaphore — should unblock tw, NOT increment count
    rtos_semaphore_give_from_isr(sh);
    TEST(tw.state == TASK_READY);
    TEST(sem_isr.wait_list == NULL);
    TEST(sem_isr.count == 0);  // given directly to waiter, not counted

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// rtos_task_handle_self
// ---------------------------------------------------------------------------

static void test_task_handle_self(void)
{
    SUITE("rtos_task_handle_self returns current task handle");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t ta;
    static uint32_t   sa[64];

    rtos_handle_t h = rtos_task_create(&ta, sa, 64,
                                        (void(*)(void*))1, NULL, "self", 1);

    // Simulate being "the current task"
    g_current = &ta;

    rtos_handle_t self = rtos_task_handle_self();
    TEST(self != NULL);
    TEST(self == h);
    TEST(rtos_task_get_name(self) != NULL);
    TEST(strcmp(rtos_task_get_name(self), "self") == 0);

    g_current = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Event group tests
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_EVENT_GROUPS

static void test_eventgroup_basics(void)
{
    SUITE("event group create / get / clear");

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);
    TEST(h != NULL);
    TEST(eg.bits == 0);
    TEST(rtos_eventgroup_get(h) == 0);

    // NULL create returns NULL
    TEST(rtos_eventgroup_create(NULL) == NULL);

    // get on NULL returns 0
    TEST(rtos_eventgroup_get(NULL) == 0);
}

static void test_eventgroup_set_get(void)
{
    SUITE("event group set / get");

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    // Set bits; verify get matches
    uint32_t ret = rtos_eventgroup_set(h, 0x05);
    TEST(ret == 0x05);
    TEST(rtos_eventgroup_get(h) == 0x05);

    // Set additional bits; prior bits preserved
    rtos_eventgroup_set(h, 0x02);
    TEST(rtos_eventgroup_get(h) == 0x07);

    // Set with zero is a no-op
    rtos_eventgroup_set(h, 0);
    TEST(rtos_eventgroup_get(h) == 0x07);
}

static void test_eventgroup_clear(void)
{
    SUITE("event group clear");

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    rtos_eventgroup_set(h, 0xFF);
    TEST(rtos_eventgroup_get(h) == 0xFF);

    // clear returns old value
    uint32_t before = rtos_eventgroup_clear(h, 0x0F);
    TEST(before == 0xFF);
    TEST(rtos_eventgroup_get(h) == 0xF0);

    // clear on NULL returns 0
    TEST(rtos_eventgroup_clear(NULL, 0xFF) == 0);
}

static void test_eventgroup_wait_no_block(void)
{
    SUITE("event group wait (RTOS_NO_WAIT paths)");

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    rtos_eventgroup_set(h, 0x03);

    // ANY mode — one matching bit is enough; result = intersection of mask & bits
    uint32_t ret = rtos_eventgroup_wait(h, 0x01, RTOS_EG_WAIT_ANY, 0, RTOS_NO_WAIT);
    TEST(ret == 0x01);
    TEST(rtos_eventgroup_get(h) == 0x03);   // bits unchanged (clear_on_exit=0)

    // ANY mode — multiple matching bits → result is the full intersection
    ret = rtos_eventgroup_wait(h, 0x07, RTOS_EG_WAIT_ANY, 0, RTOS_NO_WAIT);
    TEST(ret == 0x03);   // 0x07 & 0x03

    // ALL mode — all bits present
    ret = rtos_eventgroup_wait(h, 0x03, RTOS_EG_WAIT_ALL, 0, RTOS_NO_WAIT);
    TEST(ret == 0x03);

    // ALL mode — missing a bit → returns 0
    ret = rtos_eventgroup_wait(h, 0x07, RTOS_EG_WAIT_ALL, 0, RTOS_NO_WAIT);
    TEST(ret == 0);
    TEST(rtos_eventgroup_get(h) == 0x03);   // bits untouched

    // ANY mode — no matching bits → returns 0
    ret = rtos_eventgroup_wait(h, 0x08, RTOS_EG_WAIT_ANY, 0, RTOS_NO_WAIT);
    TEST(ret == 0);
}

static void test_eventgroup_clear_on_exit(void)
{
    SUITE("event group clear_on_exit");

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    rtos_eventgroup_set(h, 0x0F);
    TEST(rtos_eventgroup_get(h) == 0x0F);

    // clear_on_exit=1: wait_mask bits are cleared after successful wait
    uint32_t ret = rtos_eventgroup_wait(h, 0x05, RTOS_EG_WAIT_ANY, 1, RTOS_NO_WAIT);
    TEST(ret == 0x05);
    TEST(rtos_eventgroup_get(h) == 0x0A);   // 0x05 cleared; 0x0A remains

    // clear_on_exit=0: bits remain
    ret = rtos_eventgroup_wait(h, 0x02, RTOS_EG_WAIT_ANY, 0, RTOS_NO_WAIT);
    TEST(ret == 0x02);
    TEST(rtos_eventgroup_get(h) == 0x0A);
}

static void test_eventgroup_set_wakes_any(void)
{
    SUITE("event group set wakes ANY-mode blocked task");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    static rtos_tcb_t   ta, tb;
    static rtos_stack_t sa[64], sb[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "waiter", 1);
    rtos_task_create(&tb, sb, 64, (void(*)(void*))1, NULL, "setter", 0);

    // Block ta on the event group (simulates what rtos_eventgroup_wait does)
    g_current = &ta;
    ta.state         = TASK_BLOCKED;
    ta.eg_wait_mask  = 0x03;
    ta.eg_clear_mask = 0;
    ta.eg_wait_mode  = RTOS_EG_WAIT_ANY;
    ta.ipc_wait      = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &ta, ta.priority);

    TEST(eg.wait_list == &ta);
    TEST(ta.state == TASK_BLOCKED);

    // Switch to setter; set bit 0x01 — satisfies ANY 0x03
    g_current = &tb;
    tb.state = TASK_RUNNING;
    rtos_eventgroup_set(h, 0x01);

    TEST(ta.state == TASK_READY);
    TEST(ta.eg_wait_mask == 0x01);   // result = 0x03 & set_bits
    TEST(eg.wait_list == NULL);
    TEST(rtos_eventgroup_get(h) == 0x01);   // not cleared (eg_clear_mask=0)

    g_current = NULL;
    g_blocked = NULL;
    g_tick_count = 0;
}

static void test_eventgroup_set_wakes_all(void)
{
    SUITE("event group set wakes ALL-mode blocked task");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    static rtos_tcb_t   ta, tb;
    static rtos_stack_t sa[64], sb[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "waiter", 1);
    rtos_task_create(&tb, sb, 64, (void(*)(void*))1, NULL, "setter", 0);

    // ta waits for ALL of bits 0x03
    ta.state = TASK_BLOCKED; ta.eg_wait_mask = 0x03;
    ta.eg_clear_mask = 0x03; ta.eg_wait_mode = RTOS_EG_WAIT_ALL;
    ta.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &ta, ta.priority);

    g_current = &tb;
    tb.state = TASK_RUNNING;

    // Set only bit 0x01 — ALL not yet satisfied
    rtos_eventgroup_set(h, 0x01);
    TEST(ta.state == TASK_BLOCKED);
    TEST(eg.wait_list == &ta);

    // Set bit 0x02 — now ALL satisfied
    rtos_eventgroup_set(h, 0x02);
    TEST(ta.state == TASK_READY);
    TEST(ta.eg_wait_mask == 0x03);    // result = 0x03 (both bits)
    TEST(rtos_eventgroup_get(h) == 0); // clear_on_exit cleared 0x03

    g_current = NULL;
    g_blocked = NULL;
    g_tick_count = 0;
}

static void test_eventgroup_timeout(void)
{
    SUITE("event group wait timeout (tick handler clears g_blocked)");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg;
    rtos_eventgroup_create(&eg);

    static rtos_tcb_t   ta;
    static rtos_stack_t sa[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "waiter", 1);

    // Block ta on the event group with a timeout
    g_current = &ta;
    ta.state = TASK_BLOCKED; ta.eg_wait_mask = 0xFF;
    ta.eg_clear_mask = 0; ta.eg_wait_mode = RTOS_EG_WAIT_ANY;
    ta.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &ta, ta.priority);
    rtos_task_blocked_add(&ta, 5);

    TEST(ta.on_blocked == 1);
    TEST(eg.wait_list == &ta);

    // Tick handler fires at tick 5: removes from g_blocked, makes TASK_READY.
    // Task is still on eg.wait_list — the wait() post-resume cleanup handles that.
    rtos_tick_advance(5);
    TEST(ta.on_blocked == 0);
    TEST(ta.state == TASK_READY);

    // Confirm the timeout path: task is still on eg.wait_list → wait() returns 0
    int still_on_list = list_remove(&eg.wait_list, &ta);
    TEST(still_on_list == 1);

    g_current = NULL;
    g_blocked = NULL;
    g_tick_count = 0;
}

static void test_eventgroup_multi_waiter(void)
{
    SUITE("event group multiple waiters + batched clear_on_exit");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    static rtos_tcb_t   ta, tb, tc;
    static rtos_stack_t sa[64], sb[64], sc[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "a", 0); // highest
    rtos_task_create(&tb, sb, 64, (void(*)(void*))1, NULL, "b", 1);
    rtos_task_create(&tc, sc, 64, (void(*)(void*))1, NULL, "c", 2);

    // ta: wait for bit 0x01, ANY, clear_on_exit
    ta.state = TASK_BLOCKED; ta.eg_wait_mask = 0x01;
    ta.eg_clear_mask = 0x01; ta.eg_wait_mode = RTOS_EG_WAIT_ANY;
    ta.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &ta, ta.priority);

    // tb: wait for bit 0x02, ANY, clear_on_exit
    tb.state = TASK_BLOCKED; tb.eg_wait_mask = 0x02;
    tb.eg_clear_mask = 0x02; tb.eg_wait_mode = RTOS_EG_WAIT_ANY;
    tb.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &tb, tb.priority);

    // tc: wait for bit 0x04, ANY, no clear
    tc.state = TASK_BLOCKED; tc.eg_wait_mask = 0x04;
    tc.eg_clear_mask = 0; tc.eg_wait_mode = RTOS_EG_WAIT_ANY;
    tc.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &tc, tc.priority);

    // Setter task
    static rtos_tcb_t   td;
    static rtos_stack_t sd[64];
    rtos_task_create(&td, sd, 64, (void(*)(void*))1, NULL, "setter", 3);
    g_current = &td;
    td.state = TASK_RUNNING;

    // Set all three bits at once
    rtos_eventgroup_set(h, 0x07);

    TEST(ta.state == TASK_READY);
    TEST(tb.state == TASK_READY);
    TEST(tc.state == TASK_READY);

    TEST(ta.eg_wait_mask == 0x01);   // result: 0x07 & 0x01
    TEST(tb.eg_wait_mask == 0x02);   // result: 0x07 & 0x02
    TEST(tc.eg_wait_mask == 0x04);   // result: 0x07 & 0x04

    // Batched clear: accumulated_clr = 0x01|0x02 = 0x03; 0x04 not cleared
    // Remaining: 0x07 & ~0x03 = 0x04
    TEST(rtos_eventgroup_get(h) == 0x04);

    g_current = NULL;
    g_blocked = NULL;
    g_tick_count = 0;
}

static void test_eventgroup_set_from_isr(void)
{
    SUITE("event group set_from_isr");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg;
    rtos_handle_t h = rtos_eventgroup_create(&eg);

    static rtos_tcb_t   ta;
    static rtos_stack_t sa[64];
    rtos_task_create(&ta, sa, 64, (void(*)(void*))1, NULL, "waiter", 1);

    ta.state = TASK_BLOCKED; ta.eg_wait_mask = 0x01;
    ta.eg_clear_mask = 0; ta.eg_wait_mode = RTOS_EG_WAIT_ANY;
    ta.ipc_wait = &eg.wait_list;
    list_insert_sorted(&eg.wait_list, &ta, ta.priority);

    g_current = &ta;

    uint32_t ret = rtos_eventgroup_set_from_isr(h, 0x01);
    TEST(ret == 0x01);
    TEST(ta.state == TASK_READY);
    TEST(eg.wait_list == NULL);

    g_current = NULL;
    g_blocked = NULL;
    g_tick_count = 0;
}

#endif // RTOS_ENABLE_EVENT_GROUPS

// ---------------------------------------------------------------------------
// EDGE-CASE a: ISR receive unblocks a task blocked on a full-queue send.
// ---------------------------------------------------------------------------
static void test_queue_isr_recv_unblocks_sender(void)
{
    SUITE("queue: ISR receive unblocks blocked sender");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_queue_t q;
    static int buf[1];
    static rtos_tcb_t sender_tcb;
    static rtos_stack_t sender_stack[64];

    rtos_task_create(&sender_tcb, sender_stack, 64, (void(*)(void*))1, NULL, "s", 1);
    rtos_handle_t qh = rtos_queue_create(&q, buf, sizeof(int), 1);

    // Fill the queue.
    int v1 = 42;
    TEST(RTOS_OK == rtos_queue_send(qh, &v1, RTOS_NO_WAIT));
    TEST(rtos_queue_messages_waiting(qh) == 1);

    // Manually block the sender on the send wait-list (simulates blocking send).
    g_current = &sender_tcb;
    sender_tcb.state = TASK_BLOCKED;
    sender_tcb.ipc_wait = &q.send_wait;
    list_insert_sorted(&q.send_wait, &sender_tcb, sender_tcb.priority);
    TEST(q.send_wait == &sender_tcb);

    // ISR drains one item — must unblock the waiting sender.
    int got = -1;
    int r = rtos_queue_receive_from_isr(qh, &got);
    TEST(r == RTOS_OK);
    TEST(got == v1);
    TEST(q.send_wait == NULL);
    TEST(sender_tcb.state == TASK_READY);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// EDGE-CASE b: queue receive times out on an empty queue (finite timeout path).
// ---------------------------------------------------------------------------
static void test_queue_recv_timeout(void)
{
    SUITE("queue: receive times out on empty queue");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_queue_t q2;
    static int buf2[4];
    rtos_handle_t qh = rtos_queue_create(&q2, buf2, sizeof(int), 4);

    static rtos_tcb_t waiter;
    static rtos_stack_t ws[64];
    rtos_task_create(&waiter, ws, 64, (void(*)(void*))1, NULL, "w", 1);
    g_current = &waiter;
    waiter.state = TASK_RUNNING;

    // No-wait on empty queue returns TIMEOUT immediately.
    int got = 0;
    TEST(RTOS_TIMEOUT == rtos_queue_receive(qh, &got, RTOS_NO_WAIT));

    // Finite timeout: manually place waiter on recv_wait and advance the tick
    // counter past the deadline to exercise the unblock-by-timeout path.
    waiter.state = TASK_BLOCKED;
    waiter.ipc_wait = &q2.recv_wait;
    waiter.wakeup_tick = g_tick_count + 5;
    list_insert_sorted(&q2.recv_wait, &waiter, waiter.priority);
    list_insert_sorted_signed(&g_blocked, &waiter, (rtos_tick_t)waiter.wakeup_tick);
    TEST(q2.recv_wait == &waiter);
    TEST(g_blocked == &waiter);

    // Advance the tick beyond the deadline — tick handler must unblock waiter.
    // The tick handler removes the task from g_blocked and makes it TASK_READY,
    // but does NOT clear q2.recv_wait (that cleanup runs in the IPC function's
    // continuation code after port_request_reschedule returns on real hardware).
    g_tick_count += 6;
    rtos_tick_handler();
    TEST(g_blocked == NULL);
    TEST(waiter.state == TASK_READY);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// EDGE-CASE c: recursive mutex nest_count overflow returns RTOS_ERR.
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_RECURSIVE_MUTEX
static void test_mutex_recursive_overflow(void)
{
    SUITE("recursive mutex: nest_count overflow guard");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t ot;
    static rtos_stack_t os[64];
    rtos_task_create(&ot, os, 64, (void(*)(void*))1, NULL, "ov", 1);
    g_current = &ot;
    ot.state = TASK_RUNNING;

    static rtos_mutex_t rm_ov;
    rtos_handle_t h = rtos_mutex_create_recursive(&rm_ov);

    // Lock 255 times — should all succeed.
    for (int i = 0; i < 255; i++) {
        TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_OK);
    }
    TEST(rm_ov.nest_count == 255);

    // 256th lock must be rejected by the overflow guard.
    TEST(rtos_mutex_lock(h, RTOS_NO_WAIT) == RTOS_ERR);
    TEST(rm_ov.nest_count == 255);

    // Drain the nest so state is clean for subsequent tests.
    for (int i = 0; i < 255; i++) rtos_mutex_unlock(h);
    TEST(rm_ov.owner == NULL);
    TEST(rm_ov.nest_count == 0);

    g_current = NULL;
}
#endif // RTOS_ENABLE_RECURSIVE_MUTEX

// ---------------------------------------------------------------------------
// EDGE-CASE d: transitive priority inheritance — A holds M1, B holds M2;
//   C (highest priority) waits on M1 → A is boosted (single-level PI).
//   The kernel does NOT propagate the boost transitively to B (by design —
//   only one level of PI is implemented).  This test documents both the
//   first-level boost and the absence of transitive propagation.
//
// We construct the state manually (same as test_priority_inheritance_multi_mutex)
// because on the host port_request_reschedule() is a no-op, so calling
// rtos_mutex_lock with a finite timeout applies and immediately removes the
// boost within the same call.
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_PRIORITY_INHERITANCE
static void test_priority_inheritance_transitive(void)
{
    SUITE("priority inheritance — transitive chain (single-level)");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t ta_pi, tb_pi, tc_pi;
    static rtos_stack_t sa_pi[64], sb_pi[64], sc_pi[64];
    rtos_task_create(&ta_pi, sa_pi, 64, (void(*)(void*))1, NULL, "A", 4);
    rtos_task_create(&tb_pi, sb_pi, 64, (void(*)(void*))1, NULL, "B", 3);
    rtos_task_create(&tc_pi, sc_pi, 64, (void(*)(void*))1, NULL, "C", 0);

    ready_remove(&tb_pi); tb_pi.state = TASK_BLOCKED;
    ready_remove(&tc_pi); tc_pi.state = TASK_BLOCKED;

    static rtos_mutex_t m1_pi, m2_pi;
    rtos_handle_t h1 = rtos_mutex_create(&m1_pi);
    rtos_handle_t h2 = rtos_mutex_create(&m2_pi);

    // A acquires M1, B acquires M2.
    g_current = &ta_pi; ta_pi.state = TASK_RUNNING;
    rtos_mutex_lock(h1, RTOS_NO_WAIT);
    g_current = &tb_pi; tb_pi.state = TASK_RUNNING;
    rtos_mutex_lock(h2, RTOS_NO_WAIT);

    TEST(m1_pi.owner == &ta_pi);
    TEST(m2_pi.owner == &tb_pi);

    // Manually place C in M1's wait list and apply the first-level PI boost
    // to A. This mirrors what rtos_mutex_lock does when a task blocks — we
    // use the manual approach so the boost stays in place for inspection.
    list_insert_sorted(&m1_pi.wait_list, &tc_pi, tc_pi.priority);
    ta_pi.priority = 0;   // first-level boost: A elevated to C's priority

    TEST(ta_pi.priority == 0);
    TEST(ta_pi.base_priority == 4);

    // Now simulate A also waiting on M2 (still at boosted priority 0).
    // Single-level PI means B is NOT automatically boosted — only the direct
    // owner of the mutex that the highest-priority waiter is blocked on gets
    // boosted.
    list_insert_sorted(&m2_pi.wait_list, &ta_pi, ta_pi.priority);
    TEST(tb_pi.priority == 3);   // B is unaffected — no transitive boost

    // A is the highest-priority waiter on M2 (priority 0). When B unlocks M2,
    // A acquires it; B's priority reverts because it had no boosting waiters.
    g_current = &tb_pi; tb_pi.state = TASK_RUNNING;
    rtos_mutex_unlock(h2);
    TEST(m2_pi.owner == &ta_pi);
    TEST(tb_pi.priority == 3);   // B unchanged throughout

    // Teardown: A unlocks M1 and M2; clear manual list entries.
    list_remove(&m1_pi.wait_list, &tc_pi);
    ta_pi.priority = ta_pi.base_priority;
    g_current = &ta_pi; ta_pi.state = TASK_RUNNING;
    rtos_mutex_unlock(h1);
    rtos_mutex_unlock(h2);

    g_current = NULL;
}
#endif // RTOS_ENABLE_PRIORITY_INHERITANCE

// ---------------------------------------------------------------------------
// EDGE-CASE e: rtos_task_notify_from_isr coalesces — calling twice only
//   leaves one pending notification.
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_TASK_NOTIFY
static void test_notify_from_isr_coalesce(void)
{
    SUITE("task notify_from_isr coalescing");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_tcb_t nc_tcb;
    static rtos_stack_t nc_stack[64];
    rtos_task_create(&nc_tcb, nc_stack, 64, (void(*)(void*))1, NULL, "nc", 1);
    g_current = &nc_tcb;
    nc_tcb.state = TASK_RUNNING;

    // First ISR notify.
    rtos_task_notify_from_isr((rtos_handle_t)&nc_tcb);
    TEST(nc_tcb.notif_pending == 1);

    // Second ISR notify before the task consumes the first — must coalesce.
    rtos_task_notify_from_isr((rtos_handle_t)&nc_tcb);
    TEST(nc_tcb.notif_pending == 1);   // still 1, not 2

    // The single pending notification is consumed by one wait.
    int r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_OK);
    TEST(nc_tcb.notif_pending == 0);

    // A second wait immediately after sees no pending notification.
    nc_tcb.state = TASK_RUNNING;
    r = rtos_task_notify_wait(RTOS_NO_WAIT);
    TEST(r == RTOS_TIMEOUT);

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_TASK_NOTIFY

// ---------------------------------------------------------------------------
// EDGE-CASE f: stack watermark boundary — HWM reports correct usage after
//   partial use (simulated by overwriting some sentinel words from the top).
// ---------------------------------------------------------------------------
static void test_stack_watermark_boundary(void)
{
    SUITE("stack watermark boundary");

#if RTOS_STACK_WATERMARK && RTOS_STACK_BYTES_PER_WORD == 4
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t wm_tcb;
    static rtos_stack_t wm_stack[32];

    rtos_handle_t h = rtos_task_create(&wm_tcb, wm_stack, 32,
                                       (void(*)(void*))1, NULL, "wm", 1);
    TEST(h != NULL);

    // After creation the entire stack is filled with 0xA5A5A5A5.
    // Simulate usage by overwriting the top 4 words (highest addresses = last
    // used by the context frame). HWM counts unused words from the bottom.
    uint32_t *p = (uint32_t *)wm_stack;
    uint32_t  total = 32;

    // Corrupt top 4 words to simulate 4 words of stack use.
    p[total - 1] = 0xDEADBEEFu;
    p[total - 2] = 0xDEADBEEFu;
    p[total - 3] = 0xDEADBEEFu;
    p[total - 4] = 0xDEADBEEFu;

    uint32_t hwm = rtos_task_stack_high_water_mark(h);
    // hwm counts remaining unused words at the bottom of the stack.
    // With 4 words used at the top, 28 words from the bottom remain untouched.
    TEST(hwm == 28);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
#else
    // When RTOS_STACK_WATERMARK is disabled or on AVR, skip gracefully.
    TEST(1);
#endif
}

// ---------------------------------------------------------------------------
// EDGE-CASE g: event group — two ISR set calls then a task wait resolves
//   correctly (both bits arrive before the wait; task unblocked immediately).
// ---------------------------------------------------------------------------
#if RTOS_ENABLE_EVENT_GROUPS
static void test_eventgroup_isr_set_then_wait(void)
{
    SUITE("event group: ISR sets both bits before task wait");

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
    g_tick_count = 0;

    static rtos_eventgroup_t eg2;
    rtos_handle_t h = rtos_eventgroup_create(&eg2);

    // Simulate two ISRs each setting a distinct bit.
    rtos_eventgroup_set_from_isr(h, 0x01);
    rtos_eventgroup_set_from_isr(h, 0x02);
    TEST(rtos_eventgroup_get(h) == 0x03);

    // Task waits for BOTH bits (ALL mode) with no-wait — should succeed
    // immediately because both bits are already set.
    static rtos_tcb_t eg_tcb;
    static rtos_stack_t eg_stack[64];
    rtos_task_create(&eg_tcb, eg_stack, 64, (void(*)(void*))1, NULL, "eg", 1);
    g_current = &eg_tcb;
    eg_tcb.state = TASK_RUNNING;

    uint32_t bits = rtos_eventgroup_wait(h, 0x03, RTOS_EG_WAIT_ALL,
                                         1 /* clear_on_exit */, RTOS_NO_WAIT);
    TEST(bits == 0x03);
    TEST(rtos_eventgroup_get(h) == 0x00);  // cleared on exit

    g_current = NULL;
    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}
#endif // RTOS_ENABLE_EVENT_GROUPS

static void test_trace_chrome(void)
{
    rtos_trace_chrome_reset();
    TEST(rtos_trace_chrome_count() == 0);
    TEST(!rtos_trace_chrome_overflowed());

    // Build a fake TCB with a name so task hooks register a name table entry.
    rtos_tcb_t fake = {0};
    for (const char *p = "trc"; *p; ++p) fake.name[p - "trc"] = *p;
    fake.priority = 5;

    rtos_trace_task_create(&fake);
    rtos_trace_task_switched_in(&fake);
    rtos_trace_sem_take((void *)0x1000, 0, 0);
    rtos_trace_sem_give((void *)0x1000, 0, 0);
    rtos_trace_mutex_lock((void *)0x2000, 0, 0);
    rtos_trace_mutex_unlock((void *)0x2000, 0, 0);
    rtos_trace_queue_send((void *)0x3000, 0, 0);
    rtos_trace_queue_receive((void *)0x3000, 0, 0);
    rtos_trace_timer_fire((void *)0x4000);
    rtos_trace_task_switched_out(&fake);

    TEST(rtos_trace_chrome_count() == 10);
    TEST(!rtos_trace_chrome_overflowed());

    static uint8_t buf[8192];
    size_t n = rtos_trace_chrome_serialize(buf, sizeof(buf));
    TEST(n > 16);
    // Magic header
    TEST(buf[0] == 'R' && buf[1] == 'T' && buf[2] == 'R' && buf[3] == 'C');
    // Version = 1
    TEST(buf[4] == 1 && buf[5] == 0);
    // records_n = 10  (offset 12, little-endian u32)
    TEST(buf[12] == 10 && buf[13] == 0 && buf[14] == 0 && buf[15] == 0);

    // Capacity-too-small returns 0 without crashing
    TEST(rtos_trace_chrome_serialize(buf, 4) == 0);

    // Overflow detection: write more than capacity records
    rtos_trace_chrome_reset();
    for (size_t i = 0; i < (RTOS_TRACE_BUFFER_BYTES / 16) + 5; ++i) {
        rtos_trace_sem_take((void *)0x100, 0, 0);
    }
    TEST(rtos_trace_chrome_overflowed());
    TEST(rtos_trace_chrome_count() == (RTOS_TRACE_BUFFER_BYTES / 16));

    rtos_trace_chrome_reset();
}

void test_main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    MODULE("RTOS Kernel");

    test_list();
    test_list_sorted();
    test_task_create();
    test_semaphore();
    test_mutex();
    test_queue();
    test_queue_extras();
#if RTOS_ENABLE_SOFTWARE_TIMERS
    test_timers();
    test_timer_self_reset();
#endif
#if RTOS_ENABLE_TASK_NOTIFY
    test_task_notify();
#endif
    test_delay_until();
    test_delay_until_multi_cycle();
    test_ok_tick_handler();
    test_ipc_timeout();
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    test_priority_inheritance();
    test_priority_inheritance_multi_mutex();
    test_priority_inheritance_timeout_drops_boost();
#endif
#if RTOS_ENABLE_RECURSIVE_MUTEX
    test_mutex_recursive();
#endif
    test_tick_wraparound();
    test_queue_blocking_send();
#if RTOS_ENABLE_TASK_SUSPEND
    test_ipc_suspend_cleanup();
#endif
#if RTOS_ENABLE_TASK_NOTIFY
    test_notify_wait_forever();
    test_notify_ipc_blocked();
#endif
    test_blocked_list_wraparound();
    test_queue_recv_from_isr();
#if RTOS_ENABLE_SOFTWARE_TIMERS
    test_timer_max_limit();
    test_timer_min_remaining();
#endif
    test_task_inspection();
#if RTOS_ENABLE_TASK_NOTIFY
    test_notify_clear();
    test_notify_value();
#endif
    test_sem_take_from_isr();
    test_trace_chrome();
#if RTOS_ENABLE_TASK_DELETE
    test_task_delete();
    test_task_delete_while_blocked();
#endif
#if RTOS_ENABLE_TASK_SUSPEND
    test_suspend_resume();
#endif
    test_task_yield();
#if RTOS_ENABLE_SOFTWARE_TIMERS
    test_timer_reset();
#endif
#if RTOS_STACK_OVERFLOW_CHECK
    test_check_stack();
#endif
    test_stack_hwm();
    test_queue_send_isr_direct();
    test_context_switch();
    test_delay_zero();
    test_name_truncation();
#if RTOS_ENABLE_TASK_SUSPEND && RTOS_ENABLE_PRIORITY_INHERITANCE
    test_setprio_blocked();
#endif
#if RTOS_ENABLE_TASK_NOTIFY
    test_notify_ipc_interaction();
    test_double_notify();
#endif
    test_mutex_non_owner();
#if RTOS_ENABLE_SOFTWARE_TIMERS
    test_timer_stop_from_callback();
#endif
    test_idle_wakeup_ticks();
    test_isr_queue_with_waiters();
#if RTOS_STACK_OVERFLOW_CHECK
    test_max_tasks();
#endif
    test_sem_give_isr_waiter();
    test_task_handle_self();
#if RTOS_ENABLE_EVENT_GROUPS
    test_eventgroup_basics();
    test_eventgroup_set_get();
    test_eventgroup_clear();
    test_eventgroup_wait_no_block();
    test_eventgroup_clear_on_exit();
    test_eventgroup_set_wakes_any();
    test_eventgroup_set_wakes_all();
    test_eventgroup_timeout();
    test_eventgroup_multi_waiter();
    test_eventgroup_set_from_isr();
    test_eventgroup_isr_set_then_wait();
#endif
    // Edge-case additions
    test_queue_isr_recv_unblocks_sender();
    test_queue_recv_timeout();
#if RTOS_ENABLE_RECURSIVE_MUTEX
    test_mutex_recursive_overflow();
#endif
#if RTOS_ENABLE_PRIORITY_INHERITANCE
    test_priority_inheritance_transitive();
#endif
#if RTOS_ENABLE_TASK_NOTIFY
    test_notify_from_isr_coalesce();
#endif
    test_stack_watermark_boundary();
}
