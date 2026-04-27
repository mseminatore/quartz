// lwipopts.h — minimal lwIP configuration for the wifi_http sample.
//
// This file is required by the Pico SDK's lwIP integration.  Place it in the
// same directory as the source file that includes <lwip/...> headers, or add
// its directory to the target's include path.
//
// The settings below are the minimum needed for a single-threaded TCP client.
// For production use, review the full lwIP documentation and tune to your
// application's needs.

#ifndef LWIPOPTS_H
#define LWIPOPTS_H

// Use the Pico SDK's single-threaded (NO_SYS) integration with background IRQ.
#define NO_SYS                  1
#define LWIP_SOCKET             0
#define LWIP_NETCONN            0

// Enable IPv4 TCP/IP
#define LWIP_TCP                1
#define LWIP_UDP                1
#define LWIP_ICMP               1
#define LWIP_ARP                1
#define LWIP_DHCP               1
#define LWIP_DNS                1

// Memory
#define MEM_SIZE                4000
#define MEMP_NUM_TCP_SEG        16
#define PBUF_POOL_SIZE          24

// TCP
#define TCP_MSS                 1460
#define TCP_WND                 (4 * TCP_MSS)
#define TCP_SND_BUF             (2 * TCP_MSS)
#define TCP_SND_QUEUELEN        8

// Application send/receive buffers
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

// Disable features not needed for this demo
#define LWIP_NETIF_HOSTNAME     1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// Statistics (disable for smaller code size)
#define LWIP_STATS              0
#define LWIP_STATS_DISPLAY      0

#endif // LWIPOPTS_H
