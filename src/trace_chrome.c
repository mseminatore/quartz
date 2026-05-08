// Copyright 2025. All rights reserved.
//
// Chrome-trace ring-buffer recorder.  Built only when RTOS_TRACE_BACKEND=chrome.
// See include/rtos_trace_chrome.h for the public API and the wire format.

#include "../include/rtos_trace_chrome.h"
#include "../include/rtos_task.h"
#include "../include/rtos_config.h"
#include "port.h"
#include "kmem.h"

#include <stdint.h>
#include <stddef.h>

#define RTOS_TRACE_RECORD_SIZE      16  /* sizeof(rtos_trace_record_t) */
#define RTOS_TRACE_RECORD_CAPACITY  (RTOS_TRACE_BUFFER_BYTES / RTOS_TRACE_RECORD_SIZE)
#if RTOS_TRACE_RECORD_CAPACITY < 16
#  error "RTOS_TRACE_BUFFER_BYTES is too small (need at least 256 bytes)"
#endif

// ---------------------------------------------------------------------------
// Handle → name/id table
// ---------------------------------------------------------------------------
#ifndef RTOS_TRACE_HANDLE_TABLE_SIZE
#  define RTOS_TRACE_HANDLE_TABLE_SIZE  32
#endif

typedef struct {
    void   *handle;
    char    name[RTOS_TASK_NAME_LEN];
    uint16_t id;
    uint8_t  is_task;
} trace_name_entry_t;

static trace_name_entry_t g_names[RTOS_TRACE_HANDLE_TABLE_SIZE];
static uint16_t           g_names_count;
static uint16_t           g_next_id = 1;

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
static rtos_trace_record_t g_ring[RTOS_TRACE_RECORD_CAPACITY];
static size_t              g_head;       // next write index
static size_t              g_count;      // number of valid records
static int                 g_overflowed; // set when oldest record is overwritten

// ---------------------------------------------------------------------------
// Helpers (assume caller already in a critical section)
// ---------------------------------------------------------------------------
static uint16_t lookup_or_assign(void *handle, const char *name, int is_task)
{
    if (!handle) return 0;
    for (uint16_t i = 0; i < g_names_count; ++i) {
        if (g_names[i].handle == handle) return g_names[i].id;
    }
    if (g_names_count >= RTOS_TRACE_HANDLE_TABLE_SIZE) {
        return 0;  // table full → unnamed handle
    }
    trace_name_entry_t *e = &g_names[g_names_count++];
    e->handle  = handle;
    e->id      = g_next_id++;
    e->is_task = (uint8_t)is_task;
    if (name) {
        for (size_t k = 0; k < RTOS_TASK_NAME_LEN - 1; ++k) {
            e->name[k] = name[k];
            if (!name[k]) break;
        }
        e->name[RTOS_TASK_NAME_LEN - 1] = '\0';
    } else {
        e->name[0] = '\0';
    }
    return e->id;
}

static void emit(uint8_t type, uint16_t id, uint8_t flags, uint32_t aux1, uint32_t aux2)
{
    rtos_trace_record_t *r = &g_ring[g_head];
    r->timestamp_us = port_timestamp_us();
    r->handle_id    = id;
    r->type         = type;
    r->flags        = flags;
    r->aux1         = aux1;
    r->aux2         = aux2;

    g_head = (g_head + 1) % RTOS_TRACE_RECORD_CAPACITY;
    if (g_count < RTOS_TRACE_RECORD_CAPACITY) {
        g_count++;
    } else {
        g_overflowed = 1;  // oldest record was just overwritten
    }
}

// ---------------------------------------------------------------------------
// rtos_trace_* hook implementations (called from kernel via macros)
// ---------------------------------------------------------------------------
void rtos_trace_task_create(rtos_tcb_t *tcb)
{
    if (!tcb) return;
    port_enter_critical();
    uint16_t id = lookup_or_assign(tcb, tcb->name, 1);
    emit(RTOS_TRACE_EV_TASK_CREATE, id, 1, tcb->priority, 0);
    port_exit_critical();
}

void rtos_trace_task_delete(rtos_tcb_t *tcb)
{
    if (!tcb) return;
    port_enter_critical();
    uint16_t id = lookup_or_assign(tcb, tcb->name, 1);
    emit(RTOS_TRACE_EV_TASK_DELETE, id, 1, tcb->priority, 0);
    port_exit_critical();
}

void rtos_trace_task_switched_in(rtos_tcb_t *tcb)
{
    if (!tcb) return;
    port_enter_critical();
    uint16_t id = lookup_or_assign(tcb, tcb->name, 1);
    emit(RTOS_TRACE_EV_TASK_SWITCH_IN, id, 1, tcb->priority, 0);
    port_exit_critical();
}

void rtos_trace_task_switched_out(rtos_tcb_t *tcb)
{
    if (!tcb) return;
    port_enter_critical();
    uint16_t id = lookup_or_assign(tcb, tcb->name, 1);
    emit(RTOS_TRACE_EV_TASK_SWITCH_OUT, id, 1, tcb->priority, 0);
    port_exit_critical();
}

#define DEFINE_OBJ_HOOK(fn, ev)                                       \
    void fn(void *handle, uint32_t aux1, uint32_t aux2) {             \
        port_enter_critical();                                        \
        uint16_t id = lookup_or_assign(handle, NULL, 0);              \
        emit((ev), id, 0, aux1, aux2);                                \
        port_exit_critical();                                         \
    }

DEFINE_OBJ_HOOK(rtos_trace_sem_take,      RTOS_TRACE_EV_SEM_TAKE)
DEFINE_OBJ_HOOK(rtos_trace_sem_give,      RTOS_TRACE_EV_SEM_GIVE)
DEFINE_OBJ_HOOK(rtos_trace_mutex_lock,    RTOS_TRACE_EV_MUTEX_LOCK)
DEFINE_OBJ_HOOK(rtos_trace_mutex_unlock,  RTOS_TRACE_EV_MUTEX_UNLOCK)
DEFINE_OBJ_HOOK(rtos_trace_queue_send,    RTOS_TRACE_EV_QUEUE_SEND)
DEFINE_OBJ_HOOK(rtos_trace_queue_receive, RTOS_TRACE_EV_QUEUE_RECEIVE)

#if RTOS_ENABLE_EVENT_GROUPS
DEFINE_OBJ_HOOK(rtos_trace_eg_set,        RTOS_TRACE_EV_EG_SET)
DEFINE_OBJ_HOOK(rtos_trace_eg_wait,       RTOS_TRACE_EV_EG_WAIT)
#endif

void rtos_trace_timer_fire(void *handle)
{
    port_enter_critical();
    uint16_t id = lookup_or_assign(handle, NULL, 0);
    emit(RTOS_TRACE_EV_TIMER_FIRE, id, 0, 0, 0);
    port_exit_critical();
}

// ---------------------------------------------------------------------------
// Public introspection / drain API
// ---------------------------------------------------------------------------
void rtos_trace_chrome_reset(void)
{
    port_enter_critical();
    g_head = 0;
    g_count = 0;
    g_overflowed = 0;
    g_names_count = 0;
    g_next_id = 1;
    port_exit_critical();
}

size_t rtos_trace_chrome_count(void)
{
    return g_count;
}

int rtos_trace_chrome_overflowed(void)
{
    return g_overflowed;
}

// ---------------------------------------------------------------------------
// Serialize header + name table + records into 'out'.
// Layout matches the comment in include/rtos_trace_chrome.h.
// ---------------------------------------------------------------------------
size_t rtos_trace_chrome_serialize(void *out, size_t cap)
{
    if (!out) return 0;

    const size_t name_entry_size = 4 + RTOS_TASK_NAME_LEN;  // u16 id + u16 reserved + name
    const size_t header_size     = 16;
    const size_t need = header_size
                     + (size_t)g_names_count * name_entry_size
                     + g_count * sizeof(rtos_trace_record_t);
    if (cap < need) return 0;

    uint8_t *p = (uint8_t *)out;

    // Header (little-endian)
    p[0] = 'R'; p[1] = 'T'; p[2] = 'R'; p[3] = 'C';
    p[4] = 1;   p[5] = 0;                              // version = 1
    p[6] = (uint8_t)(RTOS_TASK_NAME_LEN & 0xFF);
    p[7] = (uint8_t)((RTOS_TASK_NAME_LEN >> 8) & 0xFF);
    uint32_t names_n = g_names_count;
    p[8]  = (uint8_t)(names_n & 0xFF);
    p[9]  = (uint8_t)((names_n >> 8) & 0xFF);
    p[10] = (uint8_t)((names_n >> 16) & 0xFF);
    p[11] = (uint8_t)((names_n >> 24) & 0xFF);
    uint32_t records_n = (uint32_t)g_count;
    p[12] = (uint8_t)(records_n & 0xFF);
    p[13] = (uint8_t)((records_n >> 8) & 0xFF);
    p[14] = (uint8_t)((records_n >> 16) & 0xFF);
    p[15] = (uint8_t)((records_n >> 24) & 0xFF);
    p += header_size;

    // Name table
    for (uint16_t i = 0; i < g_names_count; ++i) {
        p[0] = (uint8_t)(g_names[i].id & 0xFF);
        p[1] = (uint8_t)((g_names[i].id >> 8) & 0xFF);
        p[2] = g_names[i].is_task;  // reserved/flag byte
        p[3] = 0;
        rtos_kmemcpy(p + 4, g_names[i].name, RTOS_TASK_NAME_LEN);
        p += name_entry_size;
    }

    // Records — drain in chronological order (oldest → newest).
    size_t start = (g_count == RTOS_TRACE_RECORD_CAPACITY)
                 ? g_head
                 : 0;
    for (size_t i = 0; i < g_count; ++i) {
        size_t idx = (start + i) % RTOS_TRACE_RECORD_CAPACITY;
        rtos_kmemcpy(p, &g_ring[idx], sizeof(rtos_trace_record_t));
        p += sizeof(rtos_trace_record_t);
    }

    return need;
}

// ---------------------------------------------------------------------------
// Hex dumper: emit serialized capture as ASCII hex (one byte = "%02X", with a
// newline every 32 bytes and a "---END---\n" terminator).  The Python
// decoder reads this format directly, which avoids needing binary stdio.
// ---------------------------------------------------------------------------
static int put_hex(int (*putch)(int, void *), void *ctx, uint8_t b)
{
    static const char hexd[] = "0123456789ABCDEF";
    if (putch(hexd[b >> 4], ctx) < 0) return -1;
    if (putch(hexd[b & 0xF], ctx) < 0) return -1;
    return 0;
}

void rtos_trace_chrome_dump_hex(int (*putch)(int, void *), void *ctx)
{
    if (!putch) return;
    static uint8_t buf[16
                       + RTOS_TRACE_HANDLE_TABLE_SIZE * (4 + RTOS_TASK_NAME_LEN)
                       + RTOS_TRACE_RECORD_CAPACITY * sizeof(rtos_trace_record_t)];
    size_t n = rtos_trace_chrome_serialize(buf, sizeof(buf));
    for (size_t i = 0; i < n; ++i) {
        put_hex(putch, ctx, buf[i]);
        if ((i & 31) == 31) putch('\n', ctx);
    }
    if (n & 31) putch('\n', ctx);
    const char *end = "---END---\n";
    while (*end) putch(*end++, ctx);
}
