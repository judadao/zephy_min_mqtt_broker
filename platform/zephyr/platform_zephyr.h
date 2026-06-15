#ifndef PLATFORM_ZEPHYR_H
#define PLATFORM_ZEPHYR_H

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#ifndef INADDR_ANY
#define INADDR_ANY 0x00000000
#endif

#ifndef INADDR_BROADCAST
#define INADDR_BROADCAST 0xffffffff
#endif

/* --- Mutex --- */
typedef struct k_mutex  plat_mutex_t;
#define PLAT_MUTEX_DEFINE(name)   K_MUTEX_DEFINE(name)
#define plat_mutex_lock(m)        k_mutex_lock(m, K_FOREVER)
#define plat_mutex_unlock(m)      k_mutex_unlock(m)

/* --- Thread --- */
typedef struct k_thread  plat_thread_t;
/* Stack array + k_thread_create handled with #ifdef __ZEPHYR__ in client.c */

/* --- Time --- */
#define plat_uptime_ms()  k_uptime_get()

/* --- Sockets (Zephyr zsock_ prefix) --- */
#define plat_socket      zsock_socket
#define plat_setsockopt  zsock_setsockopt
#define plat_bind        zsock_bind
#define plat_listen      zsock_listen
#define plat_accept      zsock_accept
#define plat_connect     zsock_connect
#define plat_send        zsock_send
#define plat_sendto      zsock_sendto
#define plat_recv        zsock_recv
#define plat_recvfrom    zsock_recvfrom
#define plat_close       zsock_close

/* LOG_MODULE_REGISTER, LOG_INF/WRN/DBG/ERR, ARG_UNUSED
   all provided by <zephyr/kernel.h> + <zephyr/logging/log.h> */

#endif /* PLATFORM_ZEPHYR_H */
