// Copyright 2025. All rights reserved.
//
// wifi_http — Pico W WiFi + HTTP client RTOS demo.
//
// NOTE: This sample requires a Pico W (PICO_BOARD=pico_w).  It will NOT
//       build on a standard Pico (no CYW43 WiFi module).
//
// Three tasks run concurrently:
//
//   wifi_task   — initialises the CYW43 chip and connects to an access point.
//                 Signals a binary semaphore once the IP address is assigned.
//
//   http_task   — waits on the semaphore, then makes a periodic HTTP GET to
//                 httpbin.org/ip and prints the JSON response (your public IP).
//                 Retries automatically on network error.
//
//   led_task    — blinks the Pico W's on-board LED (connected to the CYW43
//                 chip, not GPIO 25) every 500 ms while the other tasks run.
//
// RTOS concepts demonstrated:
//   - Binary semaphore for init-completion sequencing (wifi_task → http_task)
//   - Long-running network I/O in task context (http_task blocks on TCP recv)
//   - Concurrent background task (led_task) unaffected by network activity
//
// Build:
//   export PICO_SDK_PATH=~/pico-sdk
//   cmake -B build_picow \
//         -DPICO_BOARD=pico_w \
//         -DWIFI_SSID="YourNetwork" \
//         -DWIFI_PASSWORD="YourPassword"
//   cmake --build build_picow
//   # Flash build_picow/sample_wifi_http.uf2; open USB serial to see output.
//
// Requires lwipopts.h (included in this directory).

#ifndef PICO_W
#   error "This sample requires PICO_BOARD=pico_w"
#endif

#include "rtos.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Configuration (supplied by CMake -D flags; fallback for IDE browsing)
// ---------------------------------------------------------------------------

#ifndef WIFI_SSID
#   define WIFI_SSID      "MyNetwork"
#endif
#ifndef WIFI_PASSWORD
#   define WIFI_PASSWORD  "MyPassword"
#endif

#define HTTP_HOST      "httpbin.org"
#define HTTP_PATH      "/ip"
#define HTTP_PORT      80
#define POLL_INTERVAL  30000u   // ms between HTTP requests
#define CONNECT_TIMEOUT_MS  30000u

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static rtos_sem_t    g_wifi_ready_sem;
static rtos_handle_t g_wifi_ready;      // signalled when IP is assigned

// ---------------------------------------------------------------------------
// Simple synchronous TCP HTTP GET using lwIP raw API + task blocking
//
// lwIP with NO_SYS and pico_cyw43_arch_lwip_threadsafe_background runs its
// stack in the background via IRQ callbacks.  We drive progress by calling
// cyw43_arch_poll() in a tight loop (or via yield) until the transfer
// completes, signalled by a task notification.
// ---------------------------------------------------------------------------

typedef struct {
    struct tcp_pcb *pcb;
    rtos_handle_t   task;       // task to notify on completion
    int             done;
    int             error;
    char            response[512];
    size_t          rlen;
} http_state_t;

static err_t tcp_recv_cb(void *arg, struct tcp_pcb *tpcb,
                          struct pbuf *p, err_t err)
{
    http_state_t *s = (http_state_t *)arg;
    (void)err;

    if (p == NULL) {
        // Connection closed by server
        s->done = 1;
        rtos_task_notify(s->task);
        return ERR_OK;
    }

    // Copy payload into response buffer
    size_t copy = p->tot_len;
    if (s->rlen + copy > sizeof(s->response) - 1u)
        copy = sizeof(s->response) - 1u - s->rlen;
    pbuf_copy_partial(p, s->response + s->rlen, (uint16_t)copy, 0);
    s->rlen += copy;
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    http_state_t *s = (http_state_t *)arg;
    (void)err;
    s->error = 1;
    s->done  = 1;
    rtos_task_notify(s->task);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    http_state_t *s = (http_state_t *)arg;
    if (err != ERR_OK) {
        s->error = 1;
        s->done  = 1;
        rtos_task_notify(s->task);
        return err;
    }

    // Send HTTP GET request
    char req[128];
    int  rlen = snprintf(req, sizeof(req),
                         "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                         HTTP_PATH, HTTP_HOST);
    tcp_write(tpcb, req, (uint16_t)rlen, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);
    return ERR_OK;
}

// Make a blocking HTTP GET.  Returns 0 on success.
static int http_get(const ip_addr_t *server_ip)
{
    http_state_t state = {0};
    state.task = rtos_task_handle_self();

    cyw43_arch_lwip_begin();

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        cyw43_arch_lwip_end();
        return -1;
    }

    state.pcb = pcb;
    tcp_arg(pcb, &state);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_err(pcb, tcp_err_cb);
    tcp_connect(pcb, server_ip, HTTP_PORT, tcp_connected_cb);

    cyw43_arch_lwip_end();

    // Wait for completion (notified by callbacks)
    rtos_task_notify_wait(10000);   // 10 s timeout

    if (!state.done || state.error)
        return -1;

    // Find and print the body (after the blank header line)
    state.response[state.rlen] = '\0';
    char *body = strstr(state.response, "\r\n\r\n");
    if (body) body += 4;
    else       body = state.response;
    printf("response: %s\n", body);
    return 0;
}

// ---------------------------------------------------------------------------
// WiFi task — connect, then signal g_wifi_ready
// ---------------------------------------------------------------------------

static rtos_tcb_t g_wifi_tcb;
static rtos_stack_t   g_wifi_stack[512];

static void wifi_task(void *arg)
{
    (void)arg;

    printf("[wifi] initialising CYW43...\n");
    if (cyw43_arch_init()) {
        printf("[wifi] init failed\n");
        return;
    }

    cyw43_arch_enable_sta_mode();
    printf("[wifi] connecting to \"%s\"...\n", WIFI_SSID);

    int ret = cyw43_arch_wifi_connect_timeout_ms(
                  WIFI_SSID, WIFI_PASSWORD,
                  CYW43_AUTH_WPA2_AES_PSK, CONNECT_TIMEOUT_MS);
    if (ret != 0) {
        printf("[wifi] connect failed (%d)\n", ret);
        return;
    }

    printf("[wifi] connected, IP: %s\n",
           ip4addr_ntoa(netif_ip4_addr(netif_list)));

    // Signal http_task that the network is up
    rtos_semaphore_give(g_wifi_ready);

    // Keep the WiFi stack alive — poll indefinitely
    for (;;) {
        cyw43_arch_poll();
        rtos_task_delay(10);    // 10 ms poll interval
    }
}

// ---------------------------------------------------------------------------
// HTTP client task — waits for WiFi, then requests every POLL_INTERVAL ms
// ---------------------------------------------------------------------------

static rtos_tcb_t g_http_tcb;
static rtos_stack_t   g_http_stack[1024];  // larger: lwIP raw API + printf

static void http_task(void *arg)
{
    (void)arg;

    // Block until the WiFi is ready
    rtos_semaphore_take(g_wifi_ready, RTOS_WAIT_FOREVER);
    printf("[http] network ready, starting periodic GET\n");

    rtos_tick_t last_req = rtos_task_tick_count();

    for (;;) {
        // Resolve hostname each time (DNS TTL honoured)
        ip_addr_t server_ip;
        err_t     dns_err = ERR_INPROGRESS;

        // dns_gethostbyname is non-blocking; poll until resolved
        cyw43_arch_lwip_begin();
        dns_err = dns_gethostbyname(HTTP_HOST, &server_ip, NULL, NULL);
        cyw43_arch_lwip_end();

        rtos_tick_t dns_start = rtos_task_tick_count();
        while (dns_err == ERR_INPROGRESS &&
               (int32_t)(rtos_task_tick_count() - dns_start) < 5000) {
            cyw43_arch_poll();
            rtos_task_delay(10);
            cyw43_arch_lwip_begin();
            dns_err = dns_gethostbyname(HTTP_HOST, &server_ip, NULL, NULL);
            cyw43_arch_lwip_end();
        }

        if (dns_err == ERR_OK) {
            printf("[http] GET http://%s%s\n", HTTP_HOST, HTTP_PATH);
            int r = http_get(&server_ip);
            if (r != 0)
                printf("[http] request failed, will retry\n");
        } else {
            printf("[http] DNS failed for %s\n", HTTP_HOST);
        }

        // Wait for the next polling interval
        rtos_task_delay_until(&last_req, POLL_INTERVAL);
    }
}

// ---------------------------------------------------------------------------
// LED task — blinks the Pico W's CYW43 LED every 500 ms
// ---------------------------------------------------------------------------

static rtos_tcb_t g_led_tcb;
static rtos_stack_t   g_led_stack[256];

static void led_task(void *arg)
{
    (void)arg;
    int state = 0;
    rtos_tick_t last_wake = rtos_task_tick_count();

    for (;;) {
        state ^= 1;
        // Pico W: LED is connected to the WiFi chip, not GPIO 25
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, state);
        rtos_task_delay_until(&last_wake, 500);
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(void)
{
    stdio_init_all();

    g_wifi_ready = rtos_semaphore_create_binary(&g_wifi_ready_sem);

    // Create tasks — wifi and led at the same priority; http at lower
    rtos_task_create(&g_wifi_tcb, g_wifi_stack, 512,
                     wifi_task, NULL, "wifi", 2);
    rtos_task_create(&g_http_tcb, g_http_stack, 1024,
                     http_task, NULL, "http", 3);   // lower priority
    rtos_task_create(&g_led_tcb,  g_led_stack,  256,
                     led_task, NULL, "led",  2);

    rtos_start();   // never returns
}
