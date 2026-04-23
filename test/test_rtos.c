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
// Minimal test harness
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

#define TEST(expr) do { \
    if (expr) { g_pass++; printf("  PASS: %s\n", #expr); } \
    else      { g_fail++; printf("  FAIL: %s  [line %d]\n", #expr, __LINE__); } \
} while(0)

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void test_list(void)
{
    printf("\n--- list ---\n");

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
    printf("\n--- xTaskCreate ---\n");

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
    printf("\n--- semaphore ---\n");

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
    printf("\n--- mutex ---\n");

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
    printf("\n--- queue ---\n");

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
    printf("\n--- list_insert_sorted ---\n");

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
    printf("\n--- task notifications ---\n");

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
    printf("\n--- rtos_task_delay_until ---\n");

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
    printf("\n--- O(k) tick handler ---\n");

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
    printf("\n--- IPC timeout via tick handler ---\n");

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
    printf("\n--- priority inheritance ---\n");

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
    printf("\n--- timers ---\n");

    static rtos_timer_t timer;
    rtos_handle_t h = rtos_timer_create(&timer, "t1", 3, 0 /* one-shot */, timer_cb);
    TEST(h != NULL);
    TEST(timer.active == 0);

    rtos_timer_start(h);
    TEST(timer.active == 1);

    // Tick twice — not yet fired
    rtos_timer_tick();
    rtos_timer_tick();
    TEST(g_timer_fires == 0);

    // Third tick fires it
    rtos_timer_tick();
    TEST(g_timer_fires == 1);
    TEST(timer.active == 0);  // one-shot: removed after firing

    // Periodic timer
    static rtos_timer_t ptimer;
    g_timer_fires = 0;
    rtos_handle_t ph = rtos_timer_create(&ptimer, "p1", 2, 1 /* periodic */, timer_cb);
    rtos_timer_start(ph);
    rtos_timer_tick(); rtos_timer_tick();  // fires once
    TEST(g_timer_fires == 1);
    rtos_timer_tick(); rtos_timer_tick();  // fires again
    TEST(g_timer_fires == 2);
    TEST(ptimer.active == 1);             // still active
    rtos_timer_stop(ph);
    TEST(ptimer.active == 0);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(void)
{
    printf("RTOS unit tests\n");

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

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
