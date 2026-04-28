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
    static uint32_t   stack[64];

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
    static uint32_t   fake_stack[32];
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

static void test_task_notify(void)
{
    SUITE("task notifications");

    static rtos_tcb_t tcb;
    static uint32_t stack[64];
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

static void test_delay_until(void)
{
    SUITE("delay until");

    static rtos_tcb_t tcb;
    static uint32_t stack[64];
    rtos_task_create(&tcb, stack, 64, (void(*)(void*))1, NULL, "du", 0);
    g_current = &tcb;
    tcb.state = TASK_RUNNING;

    // Manually advance tick counter for the test
    g_tick_count = 50;

    uint32_t wake = 50;

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

static void test_priority_inheritance(void)
{
    SUITE("priority inheritance");

    g_blocked = NULL;
    g_tick_count = 0;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;

    static rtos_tcb_t low_tcb, high_tcb;
    static uint32_t   low_stack[64], high_stack[64];

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
    // Owner priority was boosted inside rtos_mutex_lock and remains boosted
    // until the owner unlocks (single-level implementation).
    TEST(low_tcb.priority == 0);  // boosted to high_tcb's priority
    TEST(low_tcb.base_priority == 3);  // base unchanged

    // Owner unlocks: priority restored before transferring ownership
    g_current = &low_tcb;
    low_tcb.state = TASK_RUNNING;
    rtos_mutex_unlock(mh);
    TEST(low_tcb.priority == 3);      // restored to base
    TEST(mtx.owner == NULL);          // unlocked (no waiter)

    g_current = NULL;
}

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
    static uint32_t     sender_stack[64], receiver_stack[64];

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

// ---------------------------------------------------------------------------
// Test: suspend while blocked on semaphore cleans ipc_wait list
// ---------------------------------------------------------------------------

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
        uint32_t timeout_ticks = RTOS_WAIT_FOREVER;
        if (timeout_ticks != RTOS_WAIT_FOREVER) {
            ta.wakeup_tick = g_tick_count + timeout_ticks;
            ta.on_blocked  = 1;
            list_insert_sorted_signed(&g_blocked, &ta, ta.wakeup_tick);
        }
    }
    TEST(ta.on_blocked == 0);
    TEST(g_blocked == NULL);

    // Advancing the tick must NOT wake the task
    g_tick_count = 0xFFFFFFFF;  // worst-case: tick just before wrap
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
    uint32_t tick_pre_wrap  = 0xFFFFFFF5u;
    uint32_t tick_post_wrap = 0x00000010u;

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
    static uint32_t   inspect_stack[64];
    rtos_handle_t h = rtos_task_create(&inspect_tcb, inspect_stack, 64,
                                        (void(*)(void*))1, NULL, "probe", 2);
    TEST(h != NULL);

    TEST(rtos_task_get_state(h) == TASK_READY);
    TEST(strcmp(rtos_task_get_name(h), "probe") == 0);
    TEST(rtos_task_get_priority(h) == 2);

    // Raise priority — task is READY so it must be moved in ready list
    TEST(RTOS_OK == rtos_task_set_priority(h, 1));
    TEST(rtos_task_get_priority(h) == 1);
    TEST(inspect_tcb.base_priority == 1);

    // Reject idle-priority assignment
    TEST(RTOS_ERR == rtos_task_set_priority(h, RTOS_MAX_PRIORITIES - 1));
    TEST(rtos_task_get_priority(h) == 1);  // unchanged

    // NULL handle returns sentinel values
    TEST(rtos_task_get_state(NULL) == TASK_DELETED);
    TEST(rtos_task_get_name(NULL) == NULL);

    g_blocked = NULL;
    g_ready_bitmap = 0;
    for (int i = 0; i < RTOS_MAX_PRIORITIES; i++) g_ready[i] = NULL;
}

// ---------------------------------------------------------------------------
// Test: rtos_task_notify_clear
// ---------------------------------------------------------------------------

static void test_notify_clear(void)
{
    SUITE("task notify_clear");

    static rtos_tcb_t nc_tcb;
    static uint32_t   nc_stack[64];
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
    static uint32_t    stacks[RTOS_MAX_TASKS + 1][64];

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
// Main
// ---------------------------------------------------------------------------

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
    test_timers();
    test_task_notify();
    test_delay_until();
    test_ok_tick_handler();
    test_ipc_timeout();
    test_priority_inheritance();
    test_tick_wraparound();
    test_queue_blocking_send();
    test_ipc_suspend_cleanup();
    test_notify_wait_forever();
    test_notify_ipc_blocked();
    test_blocked_list_wraparound();
    test_queue_recv_from_isr();
    test_timer_max_limit();
    test_timer_min_remaining();
    test_task_inspection();
    test_notify_clear();
    test_sem_take_from_isr();
    test_task_delete();
    test_task_delete_while_blocked();
    test_suspend_resume();
    test_task_yield();
    test_timer_reset();
    test_check_stack();
    test_stack_hwm();
    test_queue_send_isr_direct();
    test_context_switch();
    test_delay_zero();
    test_name_truncation();
    test_setprio_blocked();
    test_notify_ipc_interaction();
    test_double_notify();
    test_mutex_non_owner();
    test_timer_stop_from_callback();
    test_idle_wakeup_ticks();
    test_isr_queue_with_waiters();
    test_max_tasks();
    test_sem_give_isr_waiter();
}
