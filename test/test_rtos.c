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

static int g_timer_fires = 0;
static void timer_cb(rtos_handle_t t) { (void)t; g_timer_fires++; }

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
    test_task_create();
    test_semaphore();
    test_mutex();
    test_queue();
    test_timers();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
