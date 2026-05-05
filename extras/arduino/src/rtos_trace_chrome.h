//---------------------------------------------------------------------------
// Copyright 2025. All rights reserved.
//
// Chrome-trace-event recorder backend (binary capture).
//
// When the kernel is built with RTOS_TRACE_BACKEND=chrome, every kernel event
// (task switch, sem take/give, mutex lock/unlock, queue send/receive, timer
// fire) is appended to a static ring buffer as a fixed 16-byte record.  The
// application is responsible for periodically draining the buffer and feeding
// it to a host-side decoder (tools/trace_to_chrome.py) which produces a JSON
// file viewable in chrome://tracing or https://ui.perfetto.dev .
//
// Wire format — see src/trace_chrome.c for the producer side and
// tools/trace_to_chrome.py for the canonical reader.
//---------------------------------------------------------------------------
#ifndef RTOS_TRACE_CHROME_H
#define RTOS_TRACE_CHROME_H

#include <stddef.h>
#include <stdint.h>
#include "rtos_config.h"

#ifndef RTOS_TRACE_BUFFER_BYTES
// 4 KB → 256 records (16 bytes each).  Override in rtos_config.h.
#   define RTOS_TRACE_BUFFER_BYTES   4096
#endif

// Event type IDs — matched 1:1 in the Python decoder.
enum {
    RTOS_TRACE_EV_TASK_SWITCH_IN  = 1,
    RTOS_TRACE_EV_TASK_SWITCH_OUT = 2,
    RTOS_TRACE_EV_TASK_CREATE     = 3,
    RTOS_TRACE_EV_TASK_DELETE     = 4,
    RTOS_TRACE_EV_SEM_TAKE        = 5,
    RTOS_TRACE_EV_SEM_GIVE        = 6,
    RTOS_TRACE_EV_MUTEX_LOCK      = 7,
    RTOS_TRACE_EV_MUTEX_UNLOCK    = 8,
    RTOS_TRACE_EV_QUEUE_SEND      = 9,
    RTOS_TRACE_EV_QUEUE_RECEIVE   = 10,
    RTOS_TRACE_EV_TIMER_FIRE      = 11,
};

// 16-byte fixed record.  Little-endian on every supported port.
typedef struct {
    uint32_t timestamp_us;   // monotonic µs at event time
    uint16_t handle_id;      // small id assigned by the recorder (see name table)
    uint8_t  type;           // RTOS_TRACE_EV_*
    uint8_t  flags;          // bit 0: handle is a task; reserved otherwise
    uint32_t aux1;           // task: priority; sem/queue: reserved
    uint32_t aux2;           // reserved (zero)
} rtos_trace_record_t;

// Reset the ring buffer (drops any captured events) and the handle/name table.
void rtos_trace_chrome_reset(void);

// Number of records currently in the ring buffer.
size_t rtos_trace_chrome_count(void);

// Whether the buffer has overflowed since the last reset (oldest events lost).
int rtos_trace_chrome_overflowed(void);

// Serialize the captured records + name table into 'out'.  Returns the number
// of bytes written, or 0 if 'out' is too small.  Format:
//
//   header (16 bytes):
//     u32  magic     = 'R','T','R','C'  (0x43525452 little-endian)
//     u16  version   = 1
//     u16  name_len  = RTOS_TASK_NAME_LEN
//     u32  names_n   (number of name table entries that follow)
//     u32  records_n (number of 16-byte records that follow)
//
//   names_n × { u16 handle_id; u16 reserved; char name[RTOS_TASK_NAME_LEN]; }
//   records_n × rtos_trace_record_t (16 bytes each)
//
// Calling this does NOT reset the buffer; use rtos_trace_chrome_reset()
// after a successful drain if you want to start fresh.
size_t rtos_trace_chrome_serialize(void *out, size_t cap);

// Convenience: hex-dump the serialized capture to a stdio-style write callback
// (one byte = two ASCII hex chars, then '\n' every 32 bytes, "---END---\n"
// terminator).  The Python decoder accepts this format directly.
void rtos_trace_chrome_dump_hex(int (*putch)(int c, void *ctx), void *ctx);

#endif // RTOS_TRACE_CHROME_H
