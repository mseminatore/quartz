// Copyright 2025. All rights reserved.
//
// uart_echo — RTOS ISR-to-task decoupling demo.
//
// A UART RX interrupt fills a small ring buffer and signals a handler task
// via rtos_task_notify_from_isr().  The handler task wakes up, drains the
// buffer one character at a time, and echoes each character back to the sender.
// When a newline is received the whole line is printed with a ">> " prefix.
//
// This pattern is fundamental to embedded RTOS design:
//   - The ISR does the minimum (buffer the byte, poke the task) and returns
//     quickly, keeping interrupt latency low.
//   - All processing (protocol, formatting, output) happens in task context
//     where blocking is allowed.
//
// Build (RP2040 / Pico W):
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build && cmake --build build
//   # Flash build/sample_uart_echo.uf2 to the board, then open a terminal
//   # on the Pico's USB-CDC serial port (115200 baud).  Type lines and see
//   # them echoed back with ">> " prefix.
//
// Build (host simulation):
//   cmake -B build && cmake --build build && ./build/sample_uart_echo
//   # Type lines on stdin; they are echoed with ">> " prefix.

#include "rtos.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Hardware abstraction — real UART on RP2040, stdin/stdout on host
// ---------------------------------------------------------------------------

#ifdef __rp2040__
#   include "pico/stdlib.h"
#   include "hardware/uart.h"
#   include "hardware/irq.h"

#   define UART_ID       uart0
#   define UART_TX_PIN   0
#   define UART_RX_PIN   1
#   define UART_BAUD     115200

static void hw_init(void)
{
    uart_init(UART_ID, UART_BAUD);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(UART_ID, false, false);
    uart_set_format(UART_ID, 8, 1, UART_PARITY_NONE);
}

static void hw_uart_putc(char c)
{
    uart_putc_raw(UART_ID, c);
}

#else  // host simulation

static void hw_init(void) {}

static void hw_uart_putc(char c)
{
    putchar(c);
    fflush(stdout);
}

#endif  // __rp2040__

// ---------------------------------------------------------------------------
// Ring buffer — written from ISR, read from task
// Lock-free single-producer / single-consumer (power-of-two size).
// ---------------------------------------------------------------------------

#define RING_SIZE  128u   // must be a power of two
#define RING_MASK  (RING_SIZE - 1u)

static volatile uint8_t  g_ring[RING_SIZE];
static volatile uint32_t g_ring_head = 0;   // written by ISR
static volatile uint32_t g_ring_tail = 0;   // read  by task

static void ring_push(uint8_t byte)
{
    uint32_t next = (g_ring_head + 1u) & RING_MASK;
    if (next != g_ring_tail) {          // drop on overflow
        g_ring[g_ring_head] = byte;
        g_ring_head = next;
    }
}

static int ring_pop(uint8_t *out)
{
    if (g_ring_tail == g_ring_head) return 0;
    *out = g_ring[g_ring_tail];
    g_ring_tail = (g_ring_tail + 1u) & RING_MASK;
    return 1;
}

// ---------------------------------------------------------------------------
// Handler task
// ---------------------------------------------------------------------------

static rtos_tcb_t  g_uart_tcb;
static rtos_stack_t    g_uart_stack[256];

static char        g_line[RING_SIZE];   // accumulates a line of input
static size_t      g_line_len = 0;

static rtos_handle_t g_uart_task_handle;

static void uart_handler_task(void *arg)
{
    (void)arg;

    for (;;) {
        // Wait for the ISR to signal that data is available
        rtos_task_notify_wait(RTOS_WAIT_FOREVER);

        uint8_t ch;
        while (ring_pop(&ch)) {
            hw_uart_putc((char)ch);     // echo the raw character

            if (ch == '\r' || ch == '\n') {
                // End of line — print with prefix
                if (g_line_len > 0) {
                    g_line[g_line_len] = '\0';
                    printf(">> %s\n", g_line);
                    g_line_len = 0;
                }
            } else if (g_line_len < sizeof(g_line) - 1u) {
                g_line[g_line_len++] = (char)ch;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// UART RX interrupt handler — runs in ISR context
// ---------------------------------------------------------------------------

#ifdef __rp2040__

static void uart_rx_isr(void)
{
    // Drain the UART FIFO as fast as possible, then wake the handler task.
    while (uart_is_readable(UART_ID)) {
        uint8_t ch = uart_getc(UART_ID);
        ring_push(ch);
    }
    rtos_task_notify_from_isr(g_uart_task_handle);
}

static void hw_install_isr(void)
{
    // Enable UART RX FIFO-not-empty interrupt
    irq_set_exclusive_handler(UART0_IRQ, uart_rx_isr);
    irq_set_enabled(UART0_IRQ, true);
    uart_set_irq_enables(UART_ID, /*rx_has_data=*/true, /*tx_needs_data=*/false);
}

#else  // host simulation — a second task feeds characters from stdin

static rtos_tcb_t  g_stdin_tcb;
static rtos_stack_t    g_stdin_stack[256];

static void stdin_feed_task(void *arg)
{
    (void)arg;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            rtos_task_delay(100);
            continue;
        }
        ring_push((uint8_t)c);
        rtos_task_notify_from_isr(g_uart_task_handle);  // safe from task too
    }
}

static void hw_install_isr(void)
{
    // On host, a second task simulates the ISR feed
    rtos_task_create(&g_stdin_tcb, g_stdin_stack, 256,
                     stdin_feed_task, NULL, "stdin", 2);
}

#endif  // __rp2040__

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    hw_init();

    g_uart_task_handle = rtos_task_create(&g_uart_tcb, g_uart_stack, 256,
                                           uart_handler_task, NULL, "uart", 2);

    hw_install_isr();

    printf("uart_echo ready — type a line and press Enter\n");

    rtos_start();   // never returns
}
